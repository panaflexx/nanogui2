/**
 * nmail.cpp — nmail: a NanoGUI2 IMAP mail client
 *
 * Based on folderview.cpp.  Pulls folders and messages live from an IMAP
 * server (plain text, no SSL).  Nothing is cached: folders, message lists,
 * and message bodies are fetched on demand over a single connection driven
 * by a worker thread (the GUI never blocks).
 *
 * Account details are entered in the Preferences window and stored in
 * "amail.config" (JSON, via dict.h).
 */

#include "nanogui/widget.h"
#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/split.h>
#include <nanogui/layout.h>
#include <nanogui/icons.h>
#include <nanogui/texteditor.h>
#include <nanogui/textbox.h>
#include <nanogui/menu.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/messagedialog.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <fstream>
#include <functional>
#include <vector>
#include <string>
#include <sstream>
#include <array>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <csignal>

#include "dict.h"
#include "imap_client.h"
#include "smtp_client.h"
#include "nmail_socket.h"
#include "http_fetch.h"
#include "htmldocument.h"
#include "saved_email.h"
#include "gumbo.h"
#include <memory>

using namespace nanogui;

// ---------------------------------------------------------------------------
// Configuration (stored in amail.config)
// ---------------------------------------------------------------------------
struct MailConfig {
    std::string host;
    int         port = 143;
    std::string username;
    std::string password;
    std::string smtp_host;      // empty -> fall back to the IMAP host
    int         smtp_port = 587;
    bool        dark_mode = false;
};

static const char *CONFIG_PATH = "amail.config";

static std::string config_get_str(const DictValue *root, const char *key) {
    const DictValue *v = dict_object_get(root, key);
    return (v && v->type == DICT_STRING && v->string_value)
               ? v->string_value : "";
}

static bool load_config(MailConfig &c) {
    std::ifstream in(CONFIG_PATH, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) return false;

    char err[256] = {0};
    DictValue *root = dict_deserialize_json_len(text.c_str(), text.size(),
                                            err, sizeof(err));
    if (!root) {
        std::cerr << "[nmail] could not parse " << CONFIG_PATH << ": "
                  << err << std::endl;
        return false;
    }
    c.host     = config_get_str(root, "host");
    c.username = config_get_str(root, "username");
    c.password = config_get_str(root, "password");
    c.smtp_host = config_get_str(root, "smtp_host");
    const DictValue *p = dict_object_get(root, "port");
    if (p && p->type == DICT_INT64)  c.port = (int)p->int64_value;
    if (p && p->type == DICT_NUMBER) c.port = (int)p->number_value;
    const DictValue *sp = dict_object_get(root, "smtp_port");
    if (sp && sp->type == DICT_INT64)  c.smtp_port = (int)sp->int64_value;
    if (sp && sp->type == DICT_NUMBER) c.smtp_port = (int)sp->number_value;
    const DictValue *dm = dict_object_get(root, "dark_mode");
    if (dm && dm->type == DICT_BOOL) c.dark_mode = dm->bool_value != 0;
    dict_destroy(root);
    return !c.host.empty();
}

static void save_config(const MailConfig &c) {
    DictValue *root = dict_create_object();
    dict_object_set(root, "host",     dict_create_string(c.host.c_str()));
    dict_object_set(root, "port",     dict_create_int64(c.port));
    dict_object_set(root, "username", dict_create_string(c.username.c_str()));
    dict_object_set(root, "password", dict_create_string(c.password.c_str()));
    dict_object_set(root, "smtp_host", dict_create_string(c.smtp_host.c_str()));
    dict_object_set(root, "smtp_port", dict_create_int64(c.smtp_port));
    dict_object_set(root, "dark_mode", dict_create_bool(c.dark_mode ? 1 : 0));

    char buf[8192];
    if (dict_serialize_json(root, buf, sizeof(buf), /*pretty=*/1)) {
        std::ofstream out(CONFIG_PATH, std::ios::binary | std::ios::trunc);
        out << buf;
    }
    dict_destroy(root);
}

// ---------------------------------------------------------------------------
// MailWorker — worker thread owning the blocking IMAP connection
// ---------------------------------------------------------------------------
class MailWorker {
public:
    /* GUI-thread callbacks (set before start(), each invoked via
     * nanogui::async so they always run on the main thread). */
    std::function<void(const std::vector<MailFolder> &)>        cb_folders;
    std::function<void(const std::string &,
                       const std::vector<MailSummary> &)>       cb_summaries;
    /* Older-message page (appended to the bottom of the list). */
    std::function<void(const std::string &,
                       const std::vector<MailSummary> &)>       cb_older;
    std::function<void(int, const MailMessage &)>               cb_body;
    /* Background prefetch: folder + seq + full message + derived preview. */
    std::function<void(const std::string &, int,
                       const MailMessage &, const std::string &)> cb_prefetched;
    std::function<void(const std::string &,
                       const std::string &)>                    cb_error;
    std::function<void(const std::string &)>                    cb_status;

    void set_config(const MailConfig &c) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = c;
    }

    void start() {
        m_thread = std::thread([this]() { run(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        m_imap.abort();   // wake a blocking recv
        if (m_thread.joinable())
            m_thread.join();
    }

    void connect()                              { post(Type::Connect); }
    void refresh()                              { post(Type::Refresh); }
    void select_folder(const std::string &name) { post(Type::Select, name); }
    void fetch_body(int seq) {
        std::string folder;
        { std::lock_guard<std::mutex> l(m_mutex); folder = m_selected_folder; }
        post(Type::FetchBody, folder, seq);
    }
    void fetch_body(const std::string &folder, int seq) { post(Type::FetchBody, folder, seq); }
    void fetch_older()                          { post(Type::FetchOlder); }
    void ensure_visible_cached(const std::string &folder,
                               const std::vector<int> &visible_seqs) {
        if (visible_seqs.empty() || folder.empty()) return;
        bool need_post = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (folder != m_prefetch_folder) return;
            if (folder != m_selected_folder) return;
            for (auto it = visible_seqs.rbegin(); it != visible_seqs.rend(); ++it) {
                int s = *it;
                bool inflight = false;
                for (auto &c : m_queue)
                    if (c.type == Type::Prefetch && c.seq == s && c.folder == folder) { inflight = true; break; }
                if (inflight) continue;
                auto qit = std::find(m_prefetch_queue.begin(), m_prefetch_queue.end(), s);
                if (qit != m_prefetch_queue.end()) {
                    m_prefetch_queue.erase(qit);
                    m_prefetch_queue.push_front(s);
                } else if (!m_prefetch_queued.count(s)) {
                    m_prefetch_queue.push_front(s);
                    m_prefetch_queued.insert(s);
                }
            }
            need_post = !m_prefetch_queue.empty();
        }
        if (need_post) schedule_next_prefetch();
    }

private:
    enum class Type { Connect, Refresh, Select, FetchBody, FetchOlder, Prefetch };
    struct Cmd {
        Type type;
        std::string folder;
        int seq = 0;
    };

    void post(Type t, const std::string &folder = "", int seq = 0) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back({t, folder, seq});
        }
        m_cv.notify_one();
    }

    /* Marshal a function to the GUI thread and wake the main loop. */
    template <typename F> void deliver(F &&fn) {
        nanogui::async(std::function<void()>(std::forward<F>(fn)));
        glfwPostEmptyEvent();
    }

    bool quitting() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_quit;
    }

    void report_error(const std::string &title, const std::string &msg) {
        if (quitting()) return;
        deliver([this, title, msg]() {
            if (cb_error) cb_error(title, msg);
        });
    }

    void report_status(const std::string &msg) {
        if (quitting()) return;
        deliver([this, msg]() {
            if (cb_status) cb_status(msg);
        });
    }

    /* Connect + LOGIN + LIST.  Returns false on failure (error reported). */
    bool do_connect() {
        MailConfig cfg;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cfg = m_config;
        }
        report_status("Connecting to " + cfg.host + ":" +
                      std::to_string(cfg.port) + "...");
        std::string err;
        if (!m_imap.open(cfg.host, cfg.port, cfg.username, cfg.password,
                         err)) {
            report_error("Connection failed", err);
            report_status("Not connected");
            return false;
        }
        m_selected_folder.clear();
        m_first_loaded = 0;
        return do_list_folders();
    }

    bool do_list_folders() {
        std::vector<MailFolder> folders;
        std::string err;
        if (!m_imap.list_folders(folders, err)) {
            report_error("Could not list folders", err);
            return false;
        }
        report_status("Connected");
        deliver([this, folders]() {
            if (cb_folders) cb_folders(folders);
        });
        return true;
    }

    void do_select(const std::string &folder) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cancel_prefetch_locked();
        }
        report_status("Fetching " + folder + "...");
        std::string err;
        int exists = 0;
        if (!m_imap.select_folder(folder, exists, err)) {
            report_error("Could not open folder", err);
            return;
        }
        m_selected_folder = folder;
        m_first_loaded    = 0;
        do_fetch_summaries(folder, exists);
    }

    void do_fetch_summaries(const std::string &folder, int exists) {
        std::vector<MailSummary> summaries;
        std::string err;
        /* First load shows the newest 150 messages; refreshes re-fetch the
           whole window the user has paged back through. */
        int first = m_first_loaded > 0 ? m_first_loaded
                                       : std::max(1, exists - 149);
        if (!m_imap.fetch_summaries(first, exists, summaries, err)) {
            report_error("Could not fetch messages", err);
            return;
        }
        m_first_loaded = first;
        /* Newest first. */
        std::reverse(summaries.begin(), summaries.end());
        report_status(folder + ": " + std::to_string(exists) +
                      (exists == 1 ? " message" : " messages"));
        deliver([this, folder, summaries]() {
            if (cb_summaries) cb_summaries(folder, summaries);
        });
        // Seed a low-priority backlog with the first 25 (newest first).
        // Further visible rows get enqueued on demand via ensure_visible_cached().
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_prefetch_folder = folder;
            m_prefetch_queue.clear();
            m_prefetch_queued.clear();
            for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
                m_prefetch_queue.push_back(summaries[i].seq);
                m_prefetch_queued.insert(summaries[i].seq);
            }
        }
        schedule_next_prefetch();
    }

    /* Fetch the next older page: the 150 messages just below the oldest
       one currently shown. */
    void do_fetch_older() {
        if (m_selected_folder.empty() || m_first_loaded <= 1)
            return;   // nothing older
        std::string folder = m_selected_folder;
        int last  = m_first_loaded - 1;
        int first = std::max(1, last - 149);

        report_status("Loading older messages...");
        std::vector<MailSummary> summaries;
        std::string err;
        if (!m_imap.fetch_summaries(first, last, summaries, err)) {
            report_error("Could not fetch older messages", err);
            return;
        }
        m_first_loaded = first;
        /* Newest first, so the GUI can append them after the current
           (newer) page. */
        std::reverse(summaries.begin(), summaries.end());
        deliver([this, folder, summaries]() {
            if (cb_older) cb_older(folder, summaries);
        });
    }

    void cancel_prefetch_locked() {
        m_prefetch_folder.clear();
        m_prefetch_queue.clear();
        m_prefetch_queued.clear();
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
            [](const Cmd &c){ return c.type == Type::Prefetch; }), m_queue.end());
    }

    bool already_prefetch_queued_locked(int seq, const std::string &folder) const {
        if (m_prefetch_queued.count(seq)) return true;
        for (auto &c : m_queue)
            if (c.type == Type::Prefetch && c.seq == seq && c.folder == folder) return true;
        return false;
    }

    void schedule_next_prefetch() {
        std::string folder; int seq = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_prefetch_queue.empty()) return;
            if (!m_selected_folder.empty() && m_selected_folder != m_prefetch_folder)
                return;
            folder = m_prefetch_folder;
            seq = m_prefetch_queue.front();
            for (auto &c : m_queue)
                if (c.type == Type::Prefetch && c.seq == seq && c.folder == folder) return;
        }
        post(Type::Prefetch, folder, seq);
    }



    void do_prefetch(const Cmd &cmd) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            bool has_priority = false;
            for (auto &c : m_queue) if (c.type != Type::Prefetch) { has_priority = true; break; }
            if (has_priority) {
                m_queue.push_back(cmd);
                return;
            }
            if (cmd.folder != m_prefetch_folder) return;
            if (cmd.folder != m_selected_folder) return;
        }
        if (!m_imap.is_open()) {
            std::string re; m_imap.reconnect(re);
            if (!m_imap.is_open()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_prefetch_queue.empty() && m_prefetch_queue.front() == cmd.seq) {
                    m_prefetch_queue.pop_front(); m_prefetch_queued.erase(cmd.seq);
                }
                return;
            }
        }
        if (!cmd.folder.empty() && m_imap.selected_folder() != cmd.folder) {
            std::string se; m_imap.ensure_selected(cmd.folder, se);
        }
        MailMessage msg; std::string err;
        bool ok = m_imap.fetch_message(cmd.seq, msg, err);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_prefetch_queue.empty() && m_prefetch_queue.front() == cmd.seq) {
                m_prefetch_queue.pop_front(); m_prefetch_queued.erase(cmd.seq);
            } else {
                auto qit = std::find(m_prefetch_queue.begin(), m_prefetch_queue.end(), cmd.seq);
                if (qit != m_prefetch_queue.end()) { m_prefetch_queue.erase(qit); m_prefetch_queued.erase(cmd.seq); }
            }
        }
        if (!ok) { schedule_next_prefetch(); return; }
        std::string preview = message_preview(msg);
        deliver([this, folder = cmd.folder, seq = cmd.seq, msg, preview]() {
            if (cb_prefetched) cb_prefetched(folder, seq, msg, preview);
        });
        schedule_next_prefetch();
    }

    void run() {
        for (;;) {
            Cmd cmd;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() {
                    return m_quit || !m_queue.empty();
                });
                if (m_quit) return;
                cmd = m_queue.front();
                m_queue.pop_front();
            }

            switch (cmd.type) {
            case Type::Connect:
                do_connect();
                break;
            case Type::Refresh:
                if (!m_imap.is_open()) {
                    if (!do_connect()) break;
                } else if (!do_list_folders()) {
                    break;
                }
                if (!m_selected_folder.empty()) {
                    std::string err;
                    int exists = 0;
                    if (m_imap.select_folder(m_selected_folder, exists, err))
                        do_fetch_summaries(m_selected_folder, exists);
                }
                break;
            case Type::Select:
                if (!m_imap.is_open())
                    report_error("Not connected",
                                 "Set up the server in Preferences first.");
                else
                    do_select(cmd.folder);
                break;
            case Type::FetchBody: {
                if (!m_imap.is_open()) {
                    report_error("Not connected",
                                 "Set up the server in Preferences first.");
                    break;
                }
                // Preserve the folder the FETCH belongs to across a silent
                // reconnect (selected_folder may reset briefly).
                std::string want_folder = cmd.folder.empty() ? m_selected_folder : cmd.folder;
                if (!want_folder.empty() && m_imap.selected_folder() != want_folder) {
                    std::string se;
                    if (!m_imap.ensure_selected(want_folder, se)) {
                        if (!ImapClient::is_connection_error(se)) {
                            report_error("Could not open folder", se);
                            break;
                        }
                        std::string re;
                        if (!m_imap.reconnect(re)) {
                            report_error("Connection lost", re);
                            break;
                        }
                        if (!m_imap.ensure_selected(want_folder, se)) {
                            report_error("Could not open folder", se);
                            break;
                        }
                    }
                }
                report_status("Fetching message...");
                MailMessage msg;
                std::string err;
                if (!m_imap.fetch_message(cmd.seq, msg, err)) {
                    if (ImapClient::is_connection_error(err)) {
                        std::string re;
                        if (m_imap.reconnect(re)) {
                            if (!want_folder.empty())
                                m_imap.ensure_selected(want_folder, re);
                            err.clear();
                            if (m_imap.fetch_message(cmd.seq, msg, err)) {
                                report_status("Ready (reconnected)");
                                deliver([this, seq = cmd.seq, msg]() {
                                    if (cb_body) cb_body(seq, msg);
                                });
                                break;
                            }
                        }
                    }
                    report_error("Could not fetch message", err);
                    report_status("Ready");
                    break;
                }
                report_status("Ready");
                deliver([this, seq = cmd.seq, msg]() {
                    if (cb_body) cb_body(seq, msg);
                });
                break;
            }
            case Type::FetchOlder:
                if (!m_imap.is_open()) {
                    report_error("Not connected",
                                 "Set up the server in Preferences first.");
                    break;
                }
                do_fetch_older();
                break;
            case Type::Prefetch:
                do_prefetch(cmd);
                break;
            }
        }
    }

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::deque<Cmd>         m_queue;
    bool                    m_quit = false;
    MailConfig              m_config;
    ImapClient              m_imap;
    std::string             m_selected_folder;
    /* Oldest sequence number currently shown in m_selected_folder
       (0 = nothing loaded).  Drives "load older" paging. */
    int                     m_first_loaded = 0;
    // background prefetch backlog (low priority, viewport-aware)
    std::string             m_prefetch_folder;
    std::deque<int>         m_prefetch_queue;
    std::unordered_set<int> m_prefetch_queued;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class FolderView;
class FolderSection;
class FolderItem;

// Currently selected item (app-wide)
static FolderItem *g_selected_item = nullptr;

class FolderChildrenContainer : public Widget {
public:
    FolderChildrenContainer(Widget *parent) : Widget(parent) {}

    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        int total_h = 0;
        for (auto *child : m_children) {
            if (child->visible())
                total_h += child->preferred_size(ctx).y();
        }
        return Vector2i(m_parent ? m_parent->size().x() : 100, total_h);
    }
};

class FolderContainer : public Widget {
public:
    FolderContainer(Widget *parent) : Widget(parent) {}

    virtual void perform_layout(NVGcontext *ctx) override {
        Vector2i ps = preferred_size(ctx);
        if (ps.y() > m_size.y())
            m_size.y() = ps.y();
        Widget::perform_layout(ctx);
    }
};

// ---------------------------------------------------------------------------
// FolderItem — a single row in the sidebar
// ---------------------------------------------------------------------------
class FolderItem : public Widget {
public:
    FolderItem(Widget *parent, const std::string &caption, int icon,
               int indent = 0, int badge = 0, bool expandable = false)
        : Widget(parent),
          m_caption(caption), m_icon(icon), m_indent(indent),
          m_badge(badge), m_expandable(expandable),
          m_expanded(false), m_hovered(false),
          m_children_container(nullptr),
          m_select_callback(nullptr)
    {
        set_cursor(Cursor::Hand);
        int row_h = (int)(font_size() * 1.8f);
        set_min_height(row_h);
        set_height(row_h);
    }

    /* ---- Accessors ---- */
    const std::string &caption() const { return m_caption; }
    void set_caption(const std::string &c) { m_caption = c; }

    int badge() const { return m_badge; }
    void set_badge(int b) { m_badge = b; }

    bool selected() const { return g_selected_item == this; }

    bool expanded() const { return m_expanded; }

    void set_select_callback(std::function<void(FolderItem *)> cb) {
        m_select_callback = cb;
    }

    /* ---- Children container for expandable items ---- */
    Widget *children_container() const { return m_children_container; }

    Widget *ensure_children_container() {
        if (!m_children_container) {
            // The container is added as a sibling right after this item
            // in the parent's child list.  It will be animated.
            m_children_container = new FolderChildrenContainer(m_parent);
            m_children_container->set_layout(
                new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));
            m_children_container->set_visible(false);
        }
        return m_children_container;
    }

    virtual Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    /* ---- Events ---- */
    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        m_hovered = enter;
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button,
                                    bool down, int modifiers) override {
        if (button == GLFW_MOUSE_BUTTON_1 && down) {
            if (m_expandable && m_children_container) {
                toggle_expand();
            }
            // Select this item
            g_selected_item = this;
            if (m_select_callback)
                m_select_callback(this);
            return true;
        }
        return Widget::mouse_button_event(p, button, down, modifiers);
    }

    void toggle_expand() {
        if (!m_children_container) return;
        m_expanded = !m_expanded;
        m_children_container->set_animation_duration(0.4f);
        if (m_expanded) {
            m_children_container->set_visible(true);
            m_children_container->start_animation(
                Widget::AnimationType::SlideDown);
        } else {
            m_children_container->start_animation(
                Widget::AnimationType::SlideUp);
        }
        screen()->redraw();
    }

    /* ---- Drawing ---- */
    virtual void draw(NVGcontext *ctx) override {
        float fs = (float)font_size();
        float x = (float)m_pos.x();
        float y = (float)m_pos.y();
        float w = (float)m_size.x();
        float h = (float)m_size.y();

        float base_x = fs * 1.5f + m_indent * fs * 1.25f;
        float rounding = 6.0f;

        if (selected()) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 4, y + 1, w - 8, h - 2, rounding);
            nvgFillColor(ctx, Color(0, 102, 255, 255));
            nvgFill(ctx);
        } else if (m_hovered) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 4, y + 1, w - 8, h - 2, rounding);
            bool dark = screen() && screen()->theme_mode() == ThemeMode::Dark;
            nvgFillColor(ctx, dark ? Color(255, 255, 255, 25)
                                   : Color(0, 0, 0, 20));
            nvgFill(ctx);
        }

        Color text_col = selected() ? Color(255, 255, 255, 255)
                                    : m_theme->m_text_color;
        Color icon_col = selected() ? Color(255, 255, 255, 255)
                                    : m_theme->m_icon_color;

        if (m_expandable) {
            float tx = x + base_x - fs * 1.0f;
            float ty = y + h * 0.5f;
            float angle = m_expanded ? NVG_PI * 0.5f : 0.0f;
            nvgSave(ctx);
            nvgTranslate(ctx, tx, ty);
            nvgRotate(ctx, angle);
            nvgFontSize(ctx, fs * 0.75f);
            nvgFontFace(ctx, "icons");
            nvgFillColor(ctx, m_theme->m_disabled_text_color);
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(ctx, 0, 0, utf8(FA_CARET_RIGHT).data(), nullptr);
            nvgRestore(ctx);
        }

        float icon_x = x + base_x;
        float icon_y = y + h * 0.5f;
        nvgFontSize(ctx, fs * 0.95f);
        nvgFontFace(ctx, "icons");
        nvgFillColor(ctx, icon_col);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(ctx, icon_x, icon_y, utf8(m_icon).data(), nullptr);

        float text_x = icon_x + fs * 1.1f;
        nvgFontSize(ctx, fs);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, text_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        {
            // Clip long folder names so they don't run under the badge
            nvgSave(ctx);
            float badge_rsv = (m_badge > 0) ? fs * 3.2f : fs * 1.2f;
            nvgIntersectScissor(ctx, text_x, y, w - (text_x - x) - badge_rsv, h);
            nvgText(ctx, text_x, y + h * 0.5f, m_caption.c_str(), nullptr);
            nvgRestore(ctx);
        }

        if (m_badge > 0) {
            std::string badge_text = std::to_string(m_badge);
            float badge_fs = fs * 0.7f;
            float bh = badge_fs * 1.6f;
            nvgFontSize(ctx, badge_fs);
            nvgFontFace(ctx, "sans-bold");
            float bounds[4];
            nvgTextBounds(ctx, 0, 0, badge_text.c_str(), nullptr, bounds);
            float bw = (bounds[2] - bounds[0]) + badge_fs * 0.9f;
            if (bw < bh) bw = bh;

            float bx = x + w - bw - fs * 1.5f;
            float by = y + (h - bh) * 0.5f;

            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, bx, by, bw, bh, bh * 0.5f);
            nvgFillColor(ctx, selected() ? Color(255, 255, 255, 200)
                                         : Color(180, 185, 195, 255));
            nvgFill(ctx);

            nvgFillColor(ctx, selected() ? Color(0, 102, 255, 255)
                                         : Color(255, 255, 255, 255));
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(ctx, bx + bw * 0.5f, by + bh * 0.5f,
                    badge_text.c_str(), nullptr);
        }

        Widget::draw(ctx);
    }

private:
    std::string m_caption;
    int m_icon;
    int m_indent;
    int m_badge;
    bool m_expandable;
    bool m_expanded;
    bool m_hovered;
    Widget *m_children_container;
    std::function<void(FolderItem *)> m_select_callback;
};

// ---------------------------------------------------------------------------
// SectionHeader — a small gray header (account name)
// ---------------------------------------------------------------------------
class SectionHeader : public Widget {
public:
    SectionHeader(Widget *parent, const std::string &title)
        : Widget(parent), m_title(title) {
        int row_h = (int)(font_size() * 1.8f);
        set_min_height(row_h);
        set_height(row_h);
    }

    virtual Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    virtual void draw(NVGcontext *ctx) override {
        float fs = (float)font_size();
        float x = (float)m_pos.x();
        float y = (float)m_pos.y();
        float h = (float)m_size.y();

        nvgFontSize(ctx, fs);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, m_theme->m_disabled_text_color);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + fs * 0.9f, y + h * 0.5f + 2.0f,
                m_title.c_str(), nullptr);

        Widget::draw(ctx);
    }

private:
    std::string m_title;
};

// ---------------------------------------------------------------------------
// FolderView — the sidebar widget, populated from the IMAP server
// ---------------------------------------------------------------------------
class FolderView : public Widget {
public:
    FolderView(Widget *parent, std::function<void(FolderItem *)> on_select)
        : Widget(parent), m_on_select(on_select)
    {
        set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

        m_scroll = new ScrollPanel(this);
        m_scroll->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);
        m_scroll->set_grow_parent(true);

        m_container = new FolderContainer(m_scroll);
        m_container->set_layout(
            new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 0));
    }

    virtual void draw(NVGcontext *ctx) override {
        nvgBeginPath(ctx);
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, m_theme->m_window_fill_unfocused);
        nvgFill(ctx);

        Widget::draw(ctx);
    }

    /* Rebuild the sidebar from the server's folder list. */
    void rebuild(const std::string &account,
                 const std::vector<MailFolder> &folders) {
        g_selected_item = nullptr;
        while (!m_container->children().empty())
            m_container->remove_child_at(0);
        m_scroll->set_scroll(0.0f);

        auto *top_spacer = new Widget(m_container);
        top_spacer->set_min_height(6);
        top_spacer->set_height(6);

        new SectionHeader(m_container, account);

        for (const MailFolder &f : folders) {
            auto *item = new FolderItem(m_container, display_name(f.name),
                                        folder_icon(f.name), 0, f.unseen);
            item->set_select_callback(m_on_select);
            item->set_tooltip(f.name);
        }

        auto *bot_spacer = new Widget(m_container);
        bot_spacer->set_min_height(20);
        bot_spacer->set_height(20);

        screen()->perform_layout();
    }

private:
    ScrollPanel *m_scroll;
    Widget *m_container;
    std::function<void(FolderItem *)> m_on_select;

    /* Leaf name after the last hierarchy delimiter for display. */
    static std::string display_name(const std::string &name) {
        size_t p = name.find_last_of("/.");
        return p == std::string::npos ? name : name.substr(p + 1);
    }

    static int folder_icon(const std::string &name) {
        std::string leaf;
        size_t p = name.find_last_of("/.");
        leaf = (p == std::string::npos) ? name : name.substr(p + 1);
        for (char &c : leaf) c = (char)std::tolower((unsigned char)c);

        if (leaf == "inbox")                                 return FA_INBOX;
        if (leaf.find("sent") != std::string::npos)          return FA_PAPER_PLANE;
        if (leaf.find("draft") != std::string::npos)         return FA_EDIT;
        if (leaf.find("junk") != std::string::npos ||
            leaf.find("spam") != std::string::npos)          return FA_MAIL_BULK;
        if (leaf.find("trash") != std::string::npos ||
            leaf.find("deleted") != std::string::npos ||
            leaf.find("bin") != std::string::npos)           return FA_TRASH;
        if (leaf.find("archive") != std::string::npos)       return FA_ARCHIVE;
        if (leaf.find("star") != std::string::npos ||
            leaf.find("flag") != std::string::npos)          return FA_STAR;
        return FA_FOLDER;
    }
};

// ---------------------------------------------------------------------------
// EmailData — plain struct describing one message in the list
// ---------------------------------------------------------------------------
struct EmailData {
    int         seq = 0;           // IMAP message sequence number
    std::string sender;
    std::string subject;
    std::string preview;
    std::string date;
    bool        has_attachment = false;
    bool        seen = true;
};

// ---------------------------------------------------------------------------
// EmailListView — virtual-scroll list: O(visible) draw cost, no child widgets
// ---------------------------------------------------------------------------
class EmailListView : public Widget {
public:
    static constexpr float ROW_SCALE = 4.6f;
    static constexpr float SB_W      = 6.0f;
    static constexpr float SB_MARGIN = 3.0f;

    EmailListView(Widget *parent,
                  std::function<void(int, const EmailData &)> on_select = nullptr)
        : Widget(parent), m_on_select(std::move(on_select)) {
        /* Virtual-scroll + inertia + spinner: repaint every frame instead of
           being baked into a retained parent display list. */
        set_live(true);
    }

    /* ---- geometry helpers ---- */
    float row_h()      const { return std::floor(font_size() * ROW_SCALE); }
    float total_h()    const { return row_h() * (float)m_emails.size(); }
    float max_scroll() const { return std::max(0.0f, total_h() - (float)m_size.y()); }

    /* ---- events ---- */
    std::function<void()> on_viewport_changed; // MailApp hooks scroll/paging prefetch
    void notify_viewport() { if (on_viewport_changed) on_viewport_changed(); }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &,
                                    int, int) override {
        int idx = idx_at(p.y());
        if (idx != m_hovered) { m_hovered = idx; screen()->redraw(); }
        return false;
    }

    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        if (!enter) { m_hovered = -1; screen()->redraw(); }
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button,
                                    bool down, int mods) override {
        if (button != GLFW_MOUSE_BUTTON_1) return false;
        if (down) {
            request_focus();   // grab keyboard focus on any click
            // Scrollbar thumb hit-test
            auto t = thumb_rect();
            if ((float)p.x() >= t[0] && (float)p.x() <= t[0] + t[2] &&
                (float)p.y() >= t[1] && (float)p.y() <= t[1] + t[3]) {
                m_vel            = 0.0f;   // kill inertia while dragging bar
                m_sb_drag        = true;
                m_sb_drag_start  = (float)p.y();
                m_sb_drag_origin = m_scroll;
                return true;
            }
            // Row hit-test
            int idx = idx_at(p.y());
            if (idx >= 0 && idx < (int)m_emails.size()) {
                m_selected = idx;
                if (m_on_select) m_on_select(idx, m_emails[idx]);
                screen()->redraw();
                return true;
            }
        } else {
            if (m_sb_drag) { m_sb_drag = false; screen()->redraw(); return true; }
        }
        return Widget::mouse_button_event(p, button, down, mods);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &,
                                  int, int) override {
        if (!m_sb_drag) return false;
        float track = (float)m_size.y() - thumb_h();
        float delta = ((float)p.y() - m_sb_drag_start) /
                      (track > 0.0f ? track : 1.0f);
        m_scroll = std::clamp(m_sb_drag_origin + delta * max_scroll(),
                              0.0f, max_scroll());
        m_vel = 0.0f;
        screen()->redraw();
        notify_viewport();
        return true;
    }

    virtual bool scroll_event(const Vector2i &, const Vector2f &rel) override {
        m_vel = std::clamp(m_vel - rel.y() * row_h() * 4.0f, -3500.0f, 3500.0f);
        screen()->redraw();
        notify_viewport();
        return true;
    }

    virtual bool keyboard_event(int key, int scancode,
                                int action, int modifiers) override {
        (void)scancode; (void)modifiers;
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return false;
        const int n = (int)m_emails.size();
        if (n == 0) return false;

        if (key == GLFW_KEY_DOWN || key == GLFW_KEY_UP) {
            int next = m_selected + (key == GLFW_KEY_DOWN ? 1 : -1);
            next = std::clamp(next, 0, n - 1);
            if (next != m_selected) {
                m_selected = next;
                scroll_to_show(m_selected);
                if (m_on_select) m_on_select(m_selected, m_emails[m_selected]);
                screen()->redraw();
            }
            notify_viewport();
            return true;
        }
        if (key == GLFW_KEY_PAGE_DOWN || key == GLFW_KEY_PAGE_UP) {
            float page = (float)m_size.y();
            m_vel = (key == GLFW_KEY_PAGE_DOWN ? 1.0f : -1.0f) * page * 6.0f;
            screen()->redraw();
            notify_viewport();
            return true;
        }
        if (key == GLFW_KEY_HOME) {
            m_scroll = 0.0f;  m_vel = 0.0f;
            screen()->redraw(); notify_viewport(); return true;
        }
        if (key == GLFW_KEY_END) {
            m_scroll = max_scroll();  m_vel = 0.0f;
            screen()->redraw(); notify_viewport(); return true;
        }
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
            if (m_selected >= 0 && m_selected < n)
                if (m_on_select) m_on_select(m_selected, m_emails[m_selected]);
            return true;
        }
        return false;
    }

    /* ---- draw (also drives the inertia animation) ---- */
    virtual void draw(NVGcontext *ctx) override {
        // ---- Inertia integration ----
        {
            double now = glfwGetTime();
            float  dt  = (m_last_t > 0.0)
                         ? std::min((float)(now - m_last_t), 0.05f)
                         : 0.0f;
            m_last_t = now;

            if (std::abs(m_vel) > 0.5f) {
                m_scroll += m_vel * dt;
                m_scroll  = std::clamp(m_scroll, 0.0f, max_scroll());
                // Kill velocity at boundaries so we don't jitter
                if (m_scroll <= 0.0f || m_scroll >= max_scroll())
                    m_vel = 0.0f;
                else {
                    m_vel *= std::exp(-8.0f * dt);   // decay; tau ≈ 125 ms
                    if (std::abs(m_vel) < 0.5f) m_vel = 0.0f;
                }
                screen()->redraw();   // keep animating until vel dies
            }
        }

        // Background
        nvgBeginPath(ctx);
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, m_dark ? Color(30, 31, 38, 255)
                                 : Color(228, 230, 238, 255));
        nvgFill(ctx);

        if (m_emails.empty()) {
            draw_scrollbar(ctx);
            return;
        }

        const float rh    = row_h();
        const int   first = std::max(0, (int)(m_scroll / rh));
        const int   last  = std::min((int)m_emails.size(),
                             (int)((m_scroll + (float)m_size.y()) / rh) + 2);

        // Clip rows to widget bounds
        nvgSave(ctx);
        nvgIntersectScissor(ctx, (float)m_pos.x(), (float)m_pos.y(),
                            (float)m_size.x(), (float)m_size.y());
        for (int i = first; i < last; ++i)
            draw_row(ctx, i,
                     (float)m_pos.x(),
                     (float)m_pos.y() + (float)i * rh - m_scroll,
                     (float)m_size.x());
        nvgRestore(ctx);

        if (m_loading_more) {
            draw_loading_strip(ctx);
            screen()->redraw();   // keep the spinner animating
        }

        // Notify when the bottom is reached (drives "load older" paging)
        if (m_on_hit_bottom && m_scroll >= max_scroll() - 2.0f)
            m_on_hit_bottom();

        draw_scrollbar(ctx);  // drawn on top, no clip
    }

    /* Update the preview text for an already-listed row in place. */
    void update_preview(int seq, const std::string &preview) {
        for (auto &e : m_emails) {
            if (e.seq == seq && e.preview != preview) {
                e.preview = preview;
                screen()->redraw();
                break;
            }
        }
    }

    // viewport helpers — used by MailApp to prioritize prefetch
    std::pair<int,int> visible_range() const {
        if (m_emails.empty() || m_size.y() <= 0) return {0,0};
        int first = std::max(0, (int)(m_scroll / row_h()));
        int last  = std::min((int)m_emails.size(), (int)((m_scroll + (float)m_size.y()) / row_h()) + 2);
        return {first, last};
    }
    std::vector<int> visible_seqs(int pad = 6) const {
        auto [first,last] = visible_range();
        int a = std::max(0, first - pad);
        int b = std::min((int)m_emails.size(), last + pad);
        std::vector<int> out; out.reserve(b-a);
        for (int i=a;i<b;++i) out.push_back(m_emails[i].seq);
        return out;
    }
    const std::vector<EmailData>& emails() const { return m_emails; }

    /* ---- data ---- */
    void set_emails(std::vector<EmailData> emails) {
        m_emails   = std::move(emails);
        m_selected = -1;
        m_hovered  = -1;
        m_scroll   = 0.0f;
        m_vel      = 0.0f;
        screen()->redraw();
    }

    /* Append older rows (from a "load more" fetch) without resetting
       scroll or selection. */
    void append_emails(std::vector<EmailData> more) {
        m_emails.insert(m_emails.end(),
                        std::make_move_iterator(more.begin()),
                        std::make_move_iterator(more.end()));
        screen()->redraw();
    }

    /* Spinner strip at the bottom while older messages are fetched. */
    void set_loading_more(bool v) {
        if (m_loading_more == v) return;
        m_loading_more = v;
        screen()->redraw();
    }

    /* Called from draw() whenever the list is scrolled to the bottom. */
    void set_on_hit_bottom(std::function<void()> cb) {
        m_on_hit_bottom = std::move(cb);
    }

    /* ---- appearance ---- */
    void set_dark(bool dark) { m_dark = dark; screen()->redraw(); }

private:
    /* ---- scrollbar geometry ---- */
    float thumb_h() const {
        if (total_h() <= 0.0f) return (float)m_size.y();
        float ratio = std::min(1.0f, (float)m_size.y() / total_h());
        return std::max(28.0f, (float)m_size.y() * ratio);
    }

    // Returns {abs_x, abs_y, w, h} of the scrollbar thumb
    std::array<float, 4> thumb_rect() const {
        float th  = thumb_h();
        float trk = (float)m_size.y() - th;
        float ty  = (max_scroll() > 0.0f)
                    ? (m_scroll / max_scroll()) * trk : 0.0f;
        float tx  = (float)m_pos.x() + (float)m_size.x() - SB_W - SB_MARGIN;
        return { tx, (float)m_pos.y() + ty, SB_W, th };
    }

    void draw_scrollbar(NVGcontext *ctx) const {
        if (total_h() <= (float)m_size.y()) return;
        auto t = thumb_rect();
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, t[0], t[1] + 3.0f, t[2], t[3] - 6.0f, t[2] * 0.5f);
        nvgFillColor(ctx, m_sb_drag
            ? (m_dark ? Color(140, 148, 165, 230) : Color(100, 110, 130, 230))
            : (m_dark ? Color( 95, 100, 115, 180) : Color(150, 155, 165, 180)));
        nvgFill(ctx);
    }

    /* Overlay strip with a spinner + caption shown while older messages
       are being fetched. */
    void draw_loading_strip(NVGcontext *ctx) {
        const float h = 34.0f;
        const float x = (float)m_pos.x();
        const float w = (float)m_size.x();
        const float y = (float)m_pos.y() + (float)m_size.y() - h;

        nvgSave(ctx);
        nvgIntersectScissor(ctx, x, y, w, h);
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, w, h);
        nvgFillColor(ctx, m_dark ? Color( 30,  31,  38, 235)
                                 : Color(228, 230, 238, 235));
        nvgFill(ctx);

        const Color fg = m_dark ? Color(180, 182, 196, 255)
                                : Color( 90,  90, 105, 255);

        // Spinner arc
        const float r  = 8.0f;
        const float cy = y + h * 0.5f;
        float tb[4] = {};
        nvgFontSize(ctx, 14.0f);
        nvgFontFace(ctx, "sans");
        nvgTextBounds(ctx, 0, 0, "Loading older messages...", nullptr, tb);
        const float text_w = tb[2] - tb[0];
        const float cx = x + (w - text_w - r * 2.0f - 10.0f) * 0.5f;
        const float a0 = (float)glfwGetTime() * 6.0f;
        nvgBeginPath(ctx);
        nvgArc(ctx, cx, cy, r, a0, a0 + NVG_PI * 1.5f, NVG_CW);
        nvgStrokeColor(ctx, fg);
        nvgStrokeWidth(ctx, 2.5f);
        nvgStroke(ctx);

        nvgFillColor(ctx, fg);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, cx + r + 10.0f, cy, "Loading older messages...", nullptr);
        nvgRestore(ctx);
    }

    int idx_at(int abs_y) const {
        float iy  = (float)(abs_y - m_pos.y()) + m_scroll;
        int   idx = (int)(iy / row_h());
        return (idx >= 0 && idx < (int)m_emails.size()) ? idx : -1;
    }

    /* ---- row drawing ---- */
    void draw_row(NVGcontext *ctx, int idx,
                  float x, float y, float w) const {
        const bool  sel = (idx == m_selected);
        const bool  hov = (idx == m_hovered) && !sel;
        const auto &d   = m_emails[idx];

        const float fs         = (float)font_size();
        const float h          = row_h();
        const float padx       = 10.0f;
        const float pady       = 6.0f;
        const float scroll_rsv = SB_W + SB_MARGIN;   // rows touch the scrollbar
        const float cw         = w - scroll_rsv;
        const float rounding   = 10.0f;

        const float sender_fs  = fs * 0.85f;
        const float date_fs    = fs * 0.60f;
        const float subject_fs = fs * 0.72f;
        const float preview_fs = fs * 0.63f + 2.0f;
        const float line_gap   = fs * 0.12f;

        const float y1 = y + pady + sender_fs * 0.85f;
        const float y2 = y1 + sender_fs  + line_gap;
        const float y3 = y2 + subject_fs + line_gap;
        const float y4 = y3 + preview_fs * 1.10f;

        // Background
        if (sel) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 3, y + 2, cw - 3, h - 4, rounding);
            nvgFillColor(ctx, Color(58, 90, 210, 255));
            nvgFill(ctx);
        } else if (hov) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 3, y + 2, cw - 3, h - 4, rounding);
            nvgFillColor(ctx, m_dark ? Color( 58,  60,  72, 255)
                                     : Color(210, 214, 222, 255));
            nvgFill(ctx);
        }

        Color name_col = sel ? Color(255,255,255,255)
                       : m_dark ? (d.seen ? Color(232,232,238,255)
                                          : Color(110,160,255,255))
                                : (d.seen ? Color( 15, 15, 20,255)
                                          : Color( 10, 80,220,255)); // unread: accent
        Color date_col = sel ? Color(200,218,255,255)
                       : m_dark ? Color(145,147,160,255) : Color(120,120,135,255);
        Color subj_col = sel ? Color(220,232,255,255)
                       : m_dark ? Color(205,206,216,255) : Color( 35, 35, 45,255);
        Color prev_col = sel ? Color(185,208,255,255)
                       : m_dark ? Color(160,162,175,255) : Color( 95, 95,110,255);

        // Date (right-aligned)
        nvgFontSize(ctx, date_fs);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, date_col);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + cw - padx, y1, d.date.c_str(), nullptr);

        float db[4] = {};
        nvgTextBounds(ctx, 0, 0, d.date.c_str(), nullptr, db);
        const float date_w = (db[2] - db[0]) + padx * 1.8f;

        // Sender name (bold, clipped against date)
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x + padx, y, cw - date_w - padx, h);
        nvgFontSize(ctx, sender_fs);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, name_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + padx, y1, d.sender.c_str(), nullptr);
        nvgRestore(ctx);

        // Subject (small bold, clipped)
        {
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x + padx, y, cw - padx - 3.0f, h);
            nvgFontSize(ctx, subject_fs);
            nvgFontFace(ctx, d.seen ? "sans" : "sans-bold");
            nvgFillColor(ctx, subj_col);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(ctx, x + padx, y2, d.subject.c_str(), nullptr);
            nvgRestore(ctx);
        }

        // Preview body — 2 lines, scissor-clipped
        nvgFontSize(ctx, preview_fs);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, prev_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        {
            float clip_h = (y4 - y3) + preview_fs * 1.35f;
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x + padx, y3 - preview_fs * 0.6f,
                                cw - padx - 3.0f, clip_h + preview_fs * 0.3f);
            nvgTextBox(ctx, x + padx, y3 - preview_fs * 0.55f,
                       cw - padx - 3.0f, d.preview.c_str(), nullptr);
            nvgRestore(ctx);
        }

        // Bottom separator (non-selected rows only)
        if (!sel) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, x + padx,      y + h - 0.5f);
            nvgLineTo(ctx, x + cw, y + h - 0.5f);
            nvgStrokeColor(ctx, m_dark ? Color( 62,  64,  76, 255)
                                       : Color(195, 198, 208, 255));
            nvgStrokeWidth(ctx, 1.0f);
            nvgStroke(ctx);
        }
    }

    /* ---- scroll-to-show (used by keyboard nav) ---- */
    void scroll_to_show(int idx) {
        const float rh  = row_h();
        const float top = (float)idx * rh;
        const float bot = top + rh;
        m_vel = 0.0f;
        if (top < m_scroll)
            m_scroll = top;
        else if (bot > m_scroll + (float)m_size.y())
            m_scroll = bot - (float)m_size.y();
        m_scroll = std::clamp(m_scroll, 0.0f, max_scroll());
    }

    /* ---- state ---- */
    std::vector<EmailData> m_emails;
    int    m_selected = -1;
    int    m_hovered  = -1;
    float  m_scroll   = 0.0f;
    float  m_vel      = 0.0f;   // inertia velocity (px/s)
    double m_last_t   = 0.0;    // timestamp of last draw (for dt)

    bool  m_sb_drag        = false;
    float m_sb_drag_start  = 0.0f;
    float m_sb_drag_origin = 0.0f;
    bool  m_dark           = false;
    bool  m_loading_more   = false;   // spinner strip while paging older mail

    std::function<void(int, const EmailData &)> m_on_select;
    std::function<void()> m_on_hit_bottom;
};

// ---------------------------------------------------------------------------
// parse_markdown — minimal Markdown → Document converter
// Supports: # / ## / ### headers, **bold**, *italic*, `code`, ```fenced```,
// "> " blockquotes, "- " / "* " bullet lists, \escapes.
// Blank lines start a new paragraph.
// ---------------------------------------------------------------------------
static void parse_markdown(Document &doc, const std::string &md,
                           NVGcolor text_color = nvgRGBA(20, 20, 25, 255),
                           float base_size = 16.0f)
{
    doc.paragraphs.clear();

    Style normal;  normal.fontSize = base_size;          normal.fgColor = text_color;
    Style code_s   = normal; code_s.monospace = true;
                             code_s.fontSize  = base_size * 0.875f;
                             code_s.bgColor   = nvgRGBA(220, 220, 228, 255);
    Style h1 = normal; h1.fontSize = base_size * 1.625f; h1.bold = true;
    Style h2 = normal; h2.fontSize = base_size * 1.25f;  h2.bold = true;
    Style h3 = normal; h3.fontSize = base_size * 1.0625f; h3.bold = true;

    // Inline-span parser: **bold**, *italic*, `code`, \escapes, plain text.
    // `base` is the paragraph's base style (blockquote paragraphs pass an
    // italic variant).
    auto append_inline = [&](Paragraph *p, const std::string &text,
                             const Style &base) {
        Style b = base; b.bold      = true;
        Style i = base; i.italic    = true;
        Style c = base; c.monospace = true;
                        c.fontSize  = base.fontSize * 0.875f;
                        c.bgColor   = code_s.bgColor;
        size_t i_ = 0;
        while (i_ < text.size()) {
            if (text[i_] == '\\' && i_ + 1 < text.size() &&
                (text[i_+1] == '*' || text[i_+1] == '`' ||
                 text[i_+1] == '\\' || text[i_+1] == '#' ||
                 text[i_+1] == '-' || text[i_+1] == '>')) {
                p->addText(std::string(1, text[i_+1]), base);
                i_ += 2; continue;
            }
            if (text.compare(i_, 3, "<u>") == 0) {
                size_t s = i_ + 3, e = text.find("</u>", s);
                if (e != std::string::npos) {
                    Style u = base; u.underline = true;
                    if (e > s) p->addText(text.substr(s, e - s), u);
                    i_ = e + 4; continue;
                }
            }
            if (i_ + 2 < text.size() && text[i_] == '*' &&
                text[i_+1] == '*' && text[i_+2] == '*') {
                size_t s = i_ + 3, e = text.find("***", s);
                if (e != std::string::npos) {
                    Style bi = base; bi.bold = true; bi.italic = true;
                    if (e > s) p->addText(text.substr(s, e - s), bi);
                    i_ = e + 3; continue;
                }
            }
            if (i_ + 1 < text.size() && text[i_] == '*' && text[i_+1] == '*') {
                size_t s = i_ + 2, e = text.find("**", s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), b);
                    i_ = e + 2; continue;
                }
            } else if (text[i_] == '*' && (i_ == 0 || text[i_-1] != '*')) {
                size_t s = i_ + 1, e = text.find('*', s);
                if (e != std::string::npos && e > s) {
                    p->addText(text.substr(s, e - s), i);
                    i_ = e + 1; continue;
                }
            } else if (text[i_] == '`') {
                size_t s = i_ + 1, e = text.find('`', s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), c);
                    i_ = e + 1; continue;
                }
            }
            size_t s = i_;
            while (i_ < text.size() && text[i_] != '*' && text[i_] != '`'
                   && text[i_] != '\\') ++i_;
            if (i_ > s) p->addText(text.substr(s, i_ - s), base);
            else        ++i_;
        }
    };

    std::istringstream iss(md);
    std::string line;
    Paragraph *cur = nullptr;
    bool inCode = false;
    std::string codeBuf;

    while (std::getline(iss, line)) {
        bool inline_break = (!line.empty() && line.back() == '\\')
                         || (line.size() >= 2
                             && line[line.size()-1] == ' '
                             && line[line.size()-2] == ' ');

        // strip trailing whitespace (and the backslash if present)
        while (!line.empty() && (std::isspace((unsigned char)line.back())
                                 || line.back() == '\\'))
            line.pop_back();

        if (line.empty()) {
            if (inCode) codeBuf += '\n';
            else        cur = nullptr;
            continue;
        }

        // fenced code block
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (inCode) {
                if (!codeBuf.empty()) doc.addParagraph()->addText(codeBuf, code_s);
                codeBuf.clear(); inCode = false;
            } else {
                inCode = true; codeBuf.clear();
            }
            cur = nullptr; continue;
        }
        if (inCode) { codeBuf += line + "\n"; continue; }

        // horizontal rule  --- / *** / ___  (3+ repeated chars, nothing else)
        if (!inCode && line.size() >= 3) {
            char c = line[0];
            if (c == '-' || c == '*' || c == '_') {
                bool all_same = true;
                for (char ch : line) if (ch != c) { all_same = false; break; }
                if (all_same) {
                    auto *p = doc.addParagraph();
                    p->isRule = true;
                    cur = nullptr;
                    continue;
                }
            }
        }

        // blockquote: "> text" — consecutive quote lines join into one
        // indented, italic paragraph (soft-wrapped like normal text).
        if (line[0] == '>' && (line.size() == 1 || line[1] == ' ')) {
            Style qs = normal; qs.italic = true;
            std::string content = line.size() > 2 ? line.substr(2) : "";
            if (!cur || cur->leftIndent <= 0.0f) {
                cur = doc.addParagraph();
                cur->leftIndent = 16.0f;
            } else {
                cur->addText(" ", qs);
            }
            append_inline(cur, content, qs);
            continue;
        }

        // headings
        if (line[0] == '#') {
            size_t lvl = 0;
            while (lvl < line.size() && line[lvl] == '#') ++lvl;
            if (lvl < line.size() && std::isspace((unsigned char)line[lvl])) {
                const Style &hs = (lvl == 1) ? h1 : (lvl == 2) ? h2 : h3;
                doc.addParagraph()->addText(line.substr(lvl + 1), hs);
                cur = nullptr; continue;
            }
        }

        // unordered list item: "- text" / "* text", optionally indented by
        // spaces for sub-levels (2 spaces per level).  One paragraph per
        // item, indented with a drawn bullet marker.
        {
            size_t sp = 0;
            while (sp < line.size() && line[sp] == ' ') ++sp;
            if (line.size() >= sp + 2 &&
                (line[sp] == '-' || line[sp] == '*') && line[sp + 1] == ' ') {
                Paragraph *p = doc.addParagraph();
                p->isBullet   = true;
                p->leftIndent = 16.0f * (float)(sp / 2 + 1);
                append_inline(p, line.substr(sp + 2), normal);
                cur = nullptr; continue;
            }
        }

        if (!cur || cur->leftIndent > 0.0f) cur = doc.addParagraph();
        else      cur->addText(" ", normal);  // soft-wrap join
        append_inline(cur, line, normal);
        if (inline_break)
            cur->addText("\n", normal);  // tight in-paragraph line break
    }

    if (inCode && !codeBuf.empty())
        doc.addParagraph()->addText(codeBuf, code_s);
    if (doc.paragraphs.empty())
        doc.addParagraph();
}

// ---------------------------------------------------------------------------
// document_to_markdown — serialize a Document back to Markdown.  Inverse of
// parse_markdown; used at send time for markup=markdown messages.
// ---------------------------------------------------------------------------
static std::string md_escape_plain(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '*' || c == '`') out += '\\';
        out += c;
    }
    return out;
}

static std::string document_to_markdown(const Document &doc) {
    auto same_style = [](const Style &a, const Style &b) {
        return a.fontSize == b.fontSize &&
               a.bold == b.bold && a.italic == b.italic &&
               a.underline == b.underline && a.monospace == b.monospace &&
               std::memcmp(&a.fgColor, &b.fgColor, sizeof(NVGcolor)) == 0 &&
               std::memcmp(&a.bgColor, &b.bgColor, sizeof(NVGcolor)) == 0;
    };
    std::string out;
    bool in_code = false;   // inside a fenced ``` block
    for (const auto &para : doc.paragraphs) {
        /* Fenced code block: a paragraph whose runs are all monospace.
         * Consecutive such paragraphs join into one block; a blank code
         * line (empty monospace run) becomes a blank line inside the
         * fence instead of splitting it. */
        bool is_code = !para->isRule && !para->runs.empty();
        for (const Text &r : para->runs)
            if (!r.style.monospace) { is_code = false; break; }
        if (is_code) {
            if (!in_code) { out += "```\n"; in_code = true; }
            std::string t = para->plain_text();
            while (!t.empty() && t.back() == '\n') t.pop_back();
            out += t;
            out += '\n';
            continue;
        }
        if (in_code) { out += "```\n"; in_code = false; }

        std::string line;
        int level = 0;
        if (para->isRule) {
            line = "---";
        } else {
            /* Heading detection mirrors parse_markdown's size ratios at
             * base 16pt (h1 26, h2 20, h3 17). */
            if (!para->runs.empty() && para->runs[0].style.bold) {
                float fs = para->runs[0].style.fontSize;
                if (fs >= 24.0f)       level = 1;
                else if (fs >= 18.5f)  level = 2;
                else if (fs >= 16.75f) level = 3;
            }
            if (level) line = std::string((size_t)level, '#') + " ";

            /* Merge adjacent same-style runs so emphasis markers wrap
             * the longest possible span ("*a b*" not "*a** **b*"). */
            std::vector<Text> merged;
            for (const Text &r : para->runs) {
                if (r.content.empty()) continue;
                if (!merged.empty() &&
                    same_style(merged.back().style, r.style))
                    merged.back().content += r.content;
                else
                    merged.push_back(r);
            }

            for (const Text &r : merged) {
                const std::string &c = r.content;
                if (c.empty()) continue;
                /* Multiline monospace run -> fenced code block. */
                if (r.style.monospace && c.find('\n') != std::string::npos) {
                    line += "```\n" + c + "```";
                    continue;
                }
                bool b = r.style.bold && !level;
                bool i = r.style.italic;
                bool u = r.style.underline;
                bool m = r.style.monospace;
                std::string open, close;
                if (m) { open += '`';    close = "`"    + close; }
                if (b) { open += "**";   close = "**"   + close; }
                if (i) { open += '*';    close = "*"    + close; }
                if (u) { open += "<u>";  close = "</u>" + close; }
                line += open + (m ? c : md_escape_plain(c)) + close;
            }
        }

        /* Escape leading markers that would otherwise be re-parsed as
         * structure (real headings/rules/quotes already handled above). */
        if (!level && !para->isRule && para->leftIndent <= 0.0f &&
            !line.empty() &&
            (line[0] == '#' || line[0] == '>' ||
             (line.size() > 1 && line[0] == '-' && line[1] == ' ')))
            line = '\\' + line;

        /* Blockquote: prefix every line with "> ".  (Bullet items carry
         * leftIndent too, but serialize with "- " instead.) */
        if (para->leftIndent > 0.0f && !para->isBullet) {
            std::string quoted;
            size_t pos = 0;
            for (;;) {
                size_t nl = line.find('\n', pos);
                quoted += "> ";
                if (nl == std::string::npos) {
                    quoted += line.substr(pos);
                    break;
                }
                quoted += line.substr(pos, nl - pos);
                quoted += '\n';
                pos = nl + 1;
            }
            line = quoted;
        }

        /* Bullet list item: "- ", indented 2 spaces per sub-level
         * (level is derived from leftIndent: 16px per level). */
        if (para->isBullet && !para->isRule) {
            int lvl = (int)(para->leftIndent / 16.0f + 0.5f) - 1;
            if (lvl < 0) lvl = 0;
            line = std::string((size_t)lvl * 2, ' ') + "- " + line;
        }

        out += line;
        out += '\n';
    }
    if (in_code) out += "```\n";
    return out;
}

// ---------------------------------------------------------------------------
// document_to_html — serialize a Document to an HTML email body.  Mirrors
// document_to_markdown's structure detection (headings by font size,
// all-monospace paragraphs -> <pre>, isBullet -> <ul>/<li> with nesting by
// indent level, leftIndent -> <blockquote>, isRule -> <hr>).
// ---------------------------------------------------------------------------
static std::string html_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;        break;
        }
    }
    return out;
}

/* Inline runs -> HTML spans.  In-paragraph newlines become <br>.
 * `in_heading` suppresses <strong> (headings are already bold). */
static std::string html_inline(const Paragraph &para, bool in_heading = false) {
    std::string out;
    for (const Text &r : para.runs) {
        if (r.content.empty()) continue;
        std::string c = html_escape(r.content);
        size_t pos = 0;
        while ((pos = c.find('\n', pos)) != std::string::npos) {
            c.replace(pos, 1, "<br>");
            pos += 4;
        }
        std::string open, close;
        if (r.style.monospace) { open += "<code>";    close = "</code>"    + close; }
        if (r.style.bold && !in_heading) { open += "<strong>";  close = "</strong>"  + close; }
        if (r.style.italic)    { open += "<em>";      close = "</em>"      + close; }
        if (r.style.underline) { open += "<u>";       close = "</u>"       + close; }
        out += open + c + close;
    }
    return out;
}

static std::string document_to_html(const Document &doc) {
    std::string out = "<!DOCTYPE html>\n<html><body>\n";
    int  list_depth = 0;   // number of open <ul> elements
    bool in_pre     = false;

    auto close_lists = [&]() {
        while (list_depth > 0) { out += "</ul>\n"; --list_depth; }
    };
    auto close_pre = [&]() {
        if (in_pre) { out += "</code></pre>\n"; in_pre = false; }
    };

    for (const auto &para : doc.paragraphs) {
        /* Code block: all-monospace paragraph (blank code lines included).
         * Consecutive ones share a single <pre>. */
        bool is_code = !para->isRule && !para->isBullet &&
                       !para->runs.empty();
        for (const Text &r : para->runs)
            if (!r.style.monospace) { is_code = false; break; }
        if (is_code) {
            close_lists();
            if (!in_pre) { out += "<pre><code>"; in_pre = true; }
            else         out += '\n';
            std::string t = para->plain_text();
            while (!t.empty() && t.back() == '\n') t.pop_back();
            out += html_escape(t);
            continue;
        }
        close_pre();

        /* Bullet list item, nesting by indent level (16px per level). */
        if (para->isBullet) {
            int lvl = (int)(para->leftIndent / 16.0f + 0.5f) - 1;
            if (lvl < 0) lvl = 0;
            int target = lvl + 1;
            while (list_depth < target) { out += "<ul>\n";  ++list_depth; }
            while (list_depth > target) { out += "</ul>\n"; --list_depth; }
            out += "<li>" + html_inline(*para) + "</li>\n";
            continue;
        }
        close_lists();

        if (para->isRule) { out += "<hr>\n"; continue; }

        /* Heading detection: same size ratios as document_to_markdown. */
        int level = 0;
        if (!para->runs.empty() && para->runs[0].style.bold) {
            float fs = para->runs[0].style.fontSize;
            if (fs >= 24.0f)       level = 1;
            else if (fs >= 18.5f)  level = 2;
            else if (fs >= 16.75f) level = 3;
        }

        std::string content = html_inline(*para, level > 0);
        if (content.empty()) continue;   // blank paragraphs add nothing

        if (level) {
            out += "<h" + std::to_string(level) + ">" + content +
                   "</h" + std::to_string(level) + ">\n";
        } else if (para->leftIndent > 0.0f) {
            out += "<blockquote><p>" + content + "</p></blockquote>\n";
        } else {
            out += "<p>" + content + "</p>\n";
        }
    }
    close_pre();
    close_lists();
    out += "</body></html>\n";
    return out;
}


/* Render a fetched message into a reading-pane Document.  Used for
 * Markdown and plain-text bodies; text/html bodies go straight to
 * HtmlDocument::set_html (see render_current). */
static void render_message(Document &doc, const MailMessage &msg,
                           NVGcolor text_color, NVGcolor meta_color) {
    doc.paragraphs.clear();

    Style normal; normal.fontSize = 17.0f; normal.fgColor = text_color;
    Style bold   = normal; bold.bold = true;
    Style subj   = normal; subj.fontSize = 24.0f; subj.bold = true;
    Style meta   = normal; meta.fgColor = meta_color;

    doc.addParagraph()->addText(msg.subject, subj);

    auto *pf = doc.addParagraph();
    pf->addText("From: ", bold);
    pf->addText(msg.from, normal);

    if (!msg.to.empty()) {
        auto *pt = doc.addParagraph();
        pt->addText("To: ", bold);
        pt->addText(msg.to, normal);
    }
    if (!msg.date.empty()) {
        auto *pd = doc.addParagraph();
        pd->addText("Date: ", bold);
        pd->addText(msg.date, meta);
    }

    auto *rule = doc.addParagraph();
    rule->isRule = true;

    if (msg.body_markdown) {
        /* MailMate-style markup=markdown (or text/markdown): render the
         * plain body as Markdown.  parse_markdown() clears its target, so
         * parse into a scratch document and move the paragraphs over. */
        Document tmp;
        parse_markdown(tmp, msg.body, text_color, 17.0f);
        for (auto &p : tmp.paragraphs)
            doc.paragraphs.push_back(std::move(p));
    } else {
        // Plain paragraphs for the body (no markup interpretation).
        std::istringstream iss(msg.body);
        std::string line;
        Paragraph *cur = nullptr;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) { cur = nullptr; continue; }
            if (!cur) cur = doc.addParagraph();
            else      cur->addText(" ", normal);
            cur->addText(line, normal);
        }
    }
    if (doc.paragraphs.empty())
        doc.addParagraph();
}

// ---------------------------------------------------------------------------
// MailApp — the application
// ---------------------------------------------------------------------------
class MailApp : public Screen {
public:
    FolderView   *m_folder_view = nullptr;
    EmailListView *m_email_list = nullptr;
    ScrollPanel  *m_view_scroll = nullptr;
    HtmlDocument *m_view        = nullptr;
    Label        *m_status      = nullptr;
    Button       *m_theme_btn   = nullptr;
    Button       *m_reply_btn   = nullptr;
    Button       *m_save_btn    = nullptr;
    Button       *m_images_btn  = nullptr;

    MailConfig  m_config;
    MailWorker  m_worker;
    bool        m_config_loaded = false;
    bool        m_dark          = false;

    std::vector<MailSummary> m_summaries;   // current folder, newest first
    std::string              m_current_folder;
    std::string              m_filter;
    int                      m_loading_seq = -1;
    bool                     m_older_inflight = false;  // fetch_older posted
    MailMessage              m_current_message;
    bool                     m_has_message = false;
    /* Preview is not built on GLFW_REPEAT: the list highlight moves
     * immediately, HTML parse / IMAP FETCH wait until the selected seq
     * has been idle for kPreviewSettleSec. */
    int                      m_pending_seq = -1;
    int                      m_rendered_seq = -1;
    double                   m_preview_settle_at = 0.0;
    EmailData                m_pending_email;
    static constexpr double  kPreviewSettleSec = 0.10;

    /* ---- inline/remote images in the reading pane ---- */
    std::unordered_map<std::string, int>         m_img_tex;        // src -> nvg id
    std::unordered_map<std::string, std::string> m_remote_bytes;   // url -> bytes
    std::unordered_set<std::string>              m_remote_pending;
    std::unordered_set<std::string>              m_remote_failed;
    std::unordered_set<std::string>              m_doc_remotes;
    std::deque<std::string>                      m_fetch_queue;
    int                                          m_fetch_inflight = 0;
    static const int                             kMaxInflight = 4;
    std::shared_ptr<bool>                        m_alive = std::make_shared<bool>(true);
    bool                     m_show_remote_images = false;  // user opt-in
    bool                     m_has_remote_images  = false;  // current msg refs

    /* Session-only caches; dropped on Refresh or reconnect. */
    std::map<std::string, std::vector<MailSummary>> m_summary_cache;
    std::map<std::string, MailMessage>              m_body_cache;

    MailApp() : Screen(Vector2i(1100, 700), "nmail") {
        inc_ref();

        // Theme — light background like macOS Mail (toggle with Ctrl/Cmd+T)
        set_theme_mode(ThemeMode::Light);
        m_theme->m_split_divider_width = 2;

        auto *root_flex = new FlexLayout(FlexDirection::Column,
                                         JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        RootWindow *window = new RootWindow(this, root_flex);

        // ---- Toolbar ----
        Widget *toolbar = new Widget(window);
        toolbar->set_min_height(60);
        toolbar->set_height(60);
        toolbar->set_min_size(Vector2i(0, 60));
        toolbar->set_height_flex(SizeMode::Fixed);
        toolbar->set_layout(
            new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 0));

        auto make_button_tool = [&](int icon, const std::string &tip) {
            Button *btn = new Button(toolbar, "", icon);
            btn->set_font_size(52);
            btn->set_transparent(true);
            btn->set_tooltip(tip);
            return btn;
        };

        Button *refresh_btn = make_button_tool(FA_SYNC, "Refresh");
        refresh_btn->set_callback([this]() { do_refresh(); });

        m_reply_btn = make_button_tool(FA_REPLY, "Reply to this message");
        m_reply_btn->set_enabled(false);
        m_reply_btn->set_callback([this]() { show_compose(); });

        m_save_btn = make_button_tool(FA_SAVE,
            "Save this email as HTML for nmail_view (Ctrl+S)");
        m_save_btn->set_enabled(false);
        m_save_btn->set_callback([this]() { save_current_email(); });

        m_images_btn = make_button_tool(FA_IMAGE,
            "Load remote images (off by default to block tracking pixels)");
        m_images_btn->set_enabled(false);
        m_images_btn->set_callback([this]() {
            m_show_remote_images = true;
            m_images_btn->set_enabled(false);
            /* Do not re-parse the HTML: bind_loaded_images() re-runs the
             * resolver, which queues HTTP GETs and leaves placeholders
             * until each texture arrives. */
            if (m_view)
                m_view->bind_loaded_images();
            update_image_status();
        });

        Button *prefs_btn = make_button_tool(FA_COG, "Preferences");
        prefs_btn->set_callback([this]() { show_preferences(); });

        m_theme_btn = make_button_tool(FA_MOON, "Toggle dark mode (Ctrl+T)");
        m_theme_btn->set_callback([this]() {
            apply_theme_mode(m_dark ? ThemeMode::Light : ThemeMode::Dark);
        });

        // Spacer
        Widget *spacer = new Widget(toolbar);
        spacer->set_min_width(10);
        spacer->set_width(10);

        // Search box — press Enter to filter the message list
        TextBox *search_box = new TextBox(toolbar);
        search_box->set_min_width(200);
        search_box->set_placeholder("Search sender or subject...");
        search_box->set_callback([this](const std::string &value) {
            m_filter = value;
            apply_filter();
            return true;
        });

        // ---- Horizontal split: sidebar | content ----
        Split *split = new Split(window, Split::Orientation::Horizontal);
        split->set_min_size(100);
        split->set_max_size({2048, 2048});
        split->set_keep_size_on_resize(true);
        root_flex->set_flex_item(split, FlexLayout::FlexItem(1.0f, 1.0f, 0));

        // ---- Left: FolderView sidebar ----
        m_folder_view = new FolderView(split, [this](FolderItem *item) {
            on_folder_selected(item);
        });
        m_folder_view->set_min_width(250);

        // ---- Right side: inner Split  (email list | message pane) ----
        Split *inner_split = new Split(split, Split::Orientation::Horizontal);
        inner_split->set_min_size(100);
        inner_split->set_max_size({2048, 2048});
        inner_split->set_keep_size_on_resize(true);

        // ---- Middle: email list ----
        m_email_list = new EmailListView(inner_split,
            [this](int idx, const EmailData &d) { on_email_selected(idx, d); });
        m_email_list->set_min_width(280);
        m_email_list->set_font_size(26);
        m_email_list->set_on_hit_bottom([this]() { maybe_fetch_older(); });
        m_email_list->on_viewport_changed = [this]() {
            if (m_current_folder.empty() || !m_email_list) return;
            // debounce: only throttle with a flag — draw() already limits rate
            static double last = 0; double now = glfwGetTime();
            if (now - last < 0.15 && m_email_list->visible_seqs().size() < 30) return;
            last = now;
            // skip prefetch for rows already cached
            auto seqs = m_email_list->visible_seqs(6);
            std::vector<int> need; need.reserve(seqs.size());
            for (int s : seqs) {
                if (m_body_cache.find(m_current_folder + ":" + std::to_string(s)) != m_body_cache.end()) continue;
                need.push_back(s);
            }
            if (!need.empty()) m_worker.ensure_visible_cached(m_current_folder, need);
        };

        // ---- Right: message area ----
        Widget *right = new Widget(inner_split);
        auto *rflex = new FlexLayout(FlexDirection::Column,
                                     JustifyContent::FlexStart,
                                     AlignItems::Stretch, 0, 0);
        right->set_layout(rflex);
        right->set_min_width(100);

        Widget *sep = new Widget(right);
        sep->set_min_height(1);
        sep->set_height(1);

        m_view_scroll = new ScrollPanel(right);
        m_view_scroll->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);
        m_view = new HtmlDocument(m_view_scroll);
        m_view->image_resolver = [this](const std::string &src) {
            return resolve_image(src);
        };
        style_editor();
        m_view_scroll->set_height_flex(SizeMode::Expanding);
        rflex->set_flex_item(m_view_scroll, FlexLayout::FlexItem(1.0f));

        // ---- Status bar ----
        m_status = new Label(window, "Not connected", "sans", 16);
        m_status->set_min_height(26);
        m_status->set_height(26);
        m_status->set_height_flex(SizeMode::Fixed);

        split->set_drag_position(0.22f);
        inner_split->set_drag_position(0.38f);

        show_welcome();

        perform_layout();

        // ---- Worker callbacks (always invoked on the GUI thread) ----
        m_worker.cb_folders = [this](const std::vector<MailFolder> &folders) {
            on_folders(folders);
        };
        m_worker.cb_summaries = [this](const std::string &folder,
                                       const std::vector<MailSummary> &sums) {
            on_summaries(folder, sums);
        };
        m_worker.cb_older = [this](const std::string &folder,
                                   const std::vector<MailSummary> &sums) {
            on_older(folder, sums);
        };
        m_worker.cb_body = [this](int seq, const MailMessage &msg) {
            on_body(seq, msg);
        };
        m_worker.cb_prefetched = [this](const std::string &folder, int seq,
                                        const MailMessage &msg,
                                        const std::string &preview) {
            on_prefetched(folder, seq, msg, preview);
        };
        m_worker.cb_error = [this](const std::string &title,
                                   const std::string &msg) {
            on_worker_error(title, msg);
        };
        m_worker.cb_status = [this](const std::string &msg) {
            m_status->set_caption(msg);
        };
        m_worker.start();

        // ---- Load saved account, or ask for it ----
        if (load_config(m_config)) {
            m_config_loaded = true;
            m_worker.set_config(m_config);
            m_worker.connect();
        } else {
            show_preferences();
        }
        apply_theme_mode(m_config.dark_mode ? ThemeMode::Dark
                                            : ThemeMode::Light);
    }

    virtual ~MailApp() override {
        *m_alive = false;
        clear_image_textures();
        m_worker.stop();
    }

    /* ---- appearance ---- */

    NVGcolor text_color() const {
        return m_dark ? nvgRGBA(226, 227, 233, 255) : nvgRGBA(20, 20, 25, 255);
    }
    NVGcolor meta_color() const {
        return m_dark ? nvgRGBA(150, 152, 166, 255) : nvgRGBA(110, 110, 125, 255);
    }

    void style_editor() {
        m_view->set_background(m_dark ? nvgRGBA(30, 31, 38, 255)
                                      : nvgRGBA(250, 250, 252, 255));
        m_view->set_colors(text_color(), meta_color());
    }

    void show_welcome() {
        Document doc;
        parse_markdown(doc,
            "# nmail\n\n"
            "Set up your IMAP account in **Preferences** (the gear icon) "
            "to begin.\n\n"
            "Messages and folders are pulled live from the server; "
            "nothing is cached locally.",
            text_color(), 18.0f);
        m_view->set_document(std::move(doc));
    }

    /* Switch light/dark appearance (mirrors example1's apply_theme_mode). */
    void apply_theme_mode(ThemeMode mode) {
        m_dark = (mode == ThemeMode::Dark);
        set_theme_mode(mode);
        m_theme->m_split_divider_width = 2;
        m_theme_btn->set_icon(m_dark ? FA_SUN : FA_MOON);
        m_email_list->set_dark(m_dark);
        style_editor();
        if (m_has_message)
            render_current();
        else
            show_welcome();
        perform_layout();
        redraw();
        /* Persist alongside the account settings (only once a config
         * exists, so a first-run toggle doesn't create an empty one). */
        if (m_config_loaded && m_config.dark_mode != m_dark) {
            m_config.dark_mode = m_dark;
            save_config(m_config);
        }
    }

    /* ---- GUI-side handlers ---- */

    void on_folders(const std::vector<MailFolder> &folders) {
        /* Fresh connection / explicit refresh: drop all cached state. */
        m_summary_cache.clear();
        m_body_cache.clear();

        std::string account = m_config.username.empty()
            ? m_config.host
            : m_config.username + " @ " + m_config.host;
        m_folder_view->rebuild(account, folders);

        // Auto-open the INBOX after the first connect.
        if (m_current_folder.empty()) {
            for (const auto &f : folders) {
                std::string lower = f.name;
                for (char &c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower == "inbox") {
                    m_worker.select_folder(f.name);
                    return;
                }
            }
        }
    }

    void on_summaries(const std::string &folder,
                      const std::vector<MailSummary> &sums) {
        m_current_folder = folder;
        m_summaries      = sums;
        m_summary_cache[folder] = sums;
        m_older_inflight = false;
        m_email_list->set_loading_more(false);
        apply_filter();
        // after layout, kick viewport prefetch so rows actually on screen win
        redraw();
        nanogui::async([this, folder]() {
            if (folder != m_current_folder || !m_email_list) return;
            auto seqs = m_email_list->visible_seqs(6);
            std::vector<int> need; need.reserve(seqs.size());
            for (int s : seqs)
                if (m_body_cache.find(folder + ":" + std::to_string(s)) == m_body_cache.end())
                    need.push_back(s);
            if (!need.empty()) m_worker.ensure_visible_cached(folder, need);
        });
        glfwPostEmptyEvent();
    }

    /* Ask the worker for the next older page when the list hits bottom. */
    void maybe_fetch_older() {
        if (m_older_inflight || m_current_folder.empty() ||
            m_summaries.empty())
            return;
        int oldest = m_summaries.back().seq;   // list is newest-first
        if (oldest <= 1) return;               // already at the first message
        m_older_inflight = true;
        m_email_list->set_loading_more(true);
        m_status->set_caption("Loading older messages...");
        m_worker.fetch_older();
    }

    void on_older(const std::string &folder,
                  const std::vector<MailSummary> &sums) {
        m_older_inflight = false;
        m_email_list->set_loading_more(false);
        if (folder != m_current_folder || sums.empty()) return;

        m_summaries.insert(m_summaries.end(), sums.begin(), sums.end());
        m_summary_cache[folder] = m_summaries;
        // make newly paged-in older rows eligible for viewport prefetch too
        {
            std::vector<int> seqs; seqs.reserve(sums.size());
            for (auto &s : sums) seqs.push_back(s.seq);
            m_worker.ensure_visible_cached(folder, seqs);
        }

        /* Append only the rows passing the active filter; unlike
           apply_filter() this leaves scroll position and selection alone. */
        std::string needle = m_filter;
        for (char &c : needle) c = (char)std::tolower((unsigned char)c);
        std::vector<EmailData> rows;
        rows.reserve(sums.size());
        for (const MailSummary &s : sums) {
            if (!needle.empty()) {
                std::string hay = s.from + "\n" + s.subject;
                for (char &c : hay) c = (char)std::tolower((unsigned char)c);
                if (hay.find(needle) == std::string::npos) continue;
            }
            EmailData d;
            d.seq     = s.seq;
            d.sender  = s.from;
            d.subject = s.subject;
            d.preview = s.preview;
            d.date    = s.date;
            d.seen    = s.seen;
            rows.push_back(d);
        }
        m_email_list->append_emails(std::move(rows));
        m_status->set_caption(folder + ": showing " +
                              std::to_string(m_summaries.size()) +
                              " messages");
        redraw();
    }

    void on_body(int seq, const MailMessage &msg) {
        // Always enrich the preview + cache, even if this wasn't the
        // foreground fetch — background prefetches land here too when
        // the user happens to be looking at that message.
        std::string preview = message_preview(msg);
        if (!preview.empty()) {
            for (auto &s : m_summaries)
                if (s.seq == seq && s.preview != preview) { s.preview = preview; break; }
            auto it = m_summary_cache.find(m_current_folder);
            if (it != m_summary_cache.end())
                for (auto &s : it->second)
                    if (s.seq == seq && s.preview != preview) { s.preview = preview; break; }
            if (m_email_list) m_email_list->update_preview(seq, preview);
        }
        if (m_body_cache.size() > 256) m_body_cache.clear();
        // every full fetch is cacheable; on_prefetched also caches, so this
        // is idempotent — just keep the freshest copy
        m_body_cache[m_current_folder + ":" + std::to_string(seq)] = msg;
        if (seq != m_loading_seq) return;   // not the foreground fetch
        if (m_pending_seq >= 0 && seq != m_pending_seq)
            return;   // still scrubbing a different message
        m_loading_seq = -1;
        m_current_message = msg;
        m_has_message     = true;
        m_rendered_seq    = seq;
        m_reply_btn->set_enabled(true);
        if (m_save_btn) m_save_btn->set_enabled(true);
        render_current();
    }

    void on_prefetched(const std::string &folder, int seq,
                       const MailMessage &msg, const std::string &preview) {
        if (m_body_cache.size() > 256) m_body_cache.clear();
        m_body_cache[folder + ":" + std::to_string(seq)] = msg;
        if (folder != m_current_folder) return;
        // only enrich empty/thin previews — never clobber a real one with
        // a shorter derived snippet from a failed decode edge case
        bool enriched = false;
        for (auto &s : m_summaries) {
            if (s.seq != seq) continue;
            if (preview.size() > s.preview.size()) { s.preview = preview; enriched = true; }
            break;
        }
        auto it = m_summary_cache.find(folder);
        if (it != m_summary_cache.end())
            for (auto &s : it->second)
                if (s.seq == seq && preview.size() > s.preview.size()) { s.preview = preview; break; }
        if (enriched && m_email_list && preview.size() > 0)
            m_email_list->update_preview(seq, preview);
    }

    void on_worker_error(const std::string &title, const std::string &msg) {
        m_older_inflight = false;
        if (m_email_list) m_email_list->set_loading_more(false);
        auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                                      title, msg, "OK", "", false);
        dlg->center();
    }

    void on_folder_selected(FolderItem *item) {
        // The tooltip carries the full folder name (caption is the leaf).
        std::string folder = item->tooltip();
        if (folder.empty()) folder = item->caption();
        if (folder == m_current_folder) return;
        m_email_list->set_emails({});
        m_loading_seq  = -1;
        m_pending_seq  = -1;
        m_rendered_seq = -1;
        m_has_message  = false;
        m_older_inflight = false;
        m_email_list->set_loading_more(false);
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);

        /* Serve the last-known list instantly, then refresh from the
         * server in the background. */
        auto cached = m_summary_cache.find(folder);
        if (cached != m_summary_cache.end()) {
            m_current_folder = folder;
            m_summaries      = cached->second;
            apply_filter();
        } else {
            Document doc;
            parse_markdown(doc, "*Loading folder...*", text_color(), 18.0f);
            m_view->set_document(std::move(doc));
        }
        redraw();
        m_worker.select_folder(folder);
    }

    void on_email_selected(int idx, const EmailData &d) {
        /* List highlight already moved.  Do not parse HTML or FETCH on
         * every GLFW_REPEAT — wait until this seq sits still. */
        if (d.seq == m_pending_seq)
            return;
        if (d.seq == m_rendered_seq && m_pending_seq < 0)
            return;
        // speculatively prioritize neighbors of the selection — the user is
        // walking the list sequentially, so ±6 around idx are most likely next.
        if (m_email_list && idx >= 0) {
            std::vector<int> around; around.reserve(13);
            auto &rows = m_email_list->emails();
            for (int i = std::max(0, idx-6); i <= std::min((int)rows.size()-1, idx+6); ++i) {
                int s = rows[i].seq;
                if (s == d.seq) continue;
                if (m_body_cache.find(m_current_folder + ":" + std::to_string(s)) != m_body_cache.end()) continue;
                around.push_back(s);
            }
            if (!around.empty()) m_worker.ensure_visible_cached(m_current_folder, around);
        }
        m_loading_seq = -1;   // drop in-flight body for a previous seq
        const bool switched = (d.seq != m_rendered_seq);
        m_pending_seq       = d.seq;
        m_pending_email     = d;
        m_preview_settle_at = glfwGetTime() + kPreviewSettleSec;
        if (switched)
            show_preview_stub(d);
        redraw();
    }

    void show_preview_stub(const EmailData &d) {
        Document doc;
        Style title; title.fontSize = 24.f; title.bold = true;
        title.fgColor = text_color();
        Style meta;  meta.fontSize = 16.f; meta.fgColor = meta_color();
        Style body;  body.fontSize = 16.f; body.fgColor = text_color();
        doc.addParagraph()->addText(d.subject.empty() ? "(no subject)"
                                                      : d.subject, title);
        auto *from = doc.addParagraph();
        from->addText(d.sender, meta);
        if (!d.date.empty()) {
            auto *dt = doc.addParagraph();
            dt->addText(d.date, meta);
        }
        auto *rule = doc.addParagraph();
        rule->isRule = true;
        if (!d.preview.empty())
            doc.addParagraph()->addText(d.preview, body);
        m_view->set_document(std::move(doc));
        m_has_message = false;
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);
        m_view_scroll->set_scroll(0.0f);
    }

    // Build a stub doc that appends a subtle "Loading…" row to the same
    // header/preview content that show_preview_stub shows, so the user
    // still sees what they selected while the FETCH is in flight.
    void show_preview_stub_with_loading(const EmailData &d) {
        Document doc;
        Style title; title.fontSize = 24.f; title.bold = true;
        title.fgColor = text_color();
        Style meta;  meta.fontSize = 16.f; meta.fgColor = meta_color();
        Style body;  body.fontSize = 16.f; body.fgColor = text_color();
        Style loading; loading.fontSize = 14.f; loading.italic = true;
        loading.fgColor = meta_color();
        doc.addParagraph()->addText(d.subject.empty() ? "(no subject)"
                                                      : d.subject, title);
        auto *from = doc.addParagraph(); from->addText(d.sender, meta);
        if (!d.date.empty()) { auto *dt = doc.addParagraph(); dt->addText(d.date, meta); }
        auto *rule = doc.addParagraph(); rule->isRule = true;
        if (!d.preview.empty()) doc.addParagraph()->addText(d.preview, body);
        auto *rule2 = doc.addParagraph(); rule2->isRule = true;
        doc.addParagraph()->addText("Loading message\u2026", loading);
        m_view->set_document(std::move(doc));
        m_has_message = false;
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);
        // Keep scroll at top — the stub is the loading view, not a separate page.
        m_view_scroll->set_scroll(0.0f);
    }

    void commit_pending_preview() {
        if (m_pending_seq < 0)
            return;
        const EmailData d = m_pending_email;
        const int seq = m_pending_seq;
        m_pending_seq = -1;
        auto cached = m_body_cache.find(m_current_folder + ":" +
                                        std::to_string(seq));
        if (cached != m_body_cache.end()) {
            m_loading_seq     = -1;
            m_current_message = cached->second;
            m_has_message     = true;
            m_rendered_seq    = seq;
            m_reply_btn->set_enabled(true);
            if (m_save_btn) m_save_btn->set_enabled(true);
            render_current();
            return;
        }
        m_loading_seq = seq;
        show_preview_stub_with_loading(d);
        m_worker.fetch_body(seq);
        redraw();
    }

    void pump_preview() {
        if (m_pending_seq < 0)
            return;
        if (glfwGetTime() < m_preview_settle_at) {
            /* Need another frame after the settle deadline; WaitEvents
             * would otherwise sleep until the next key. */
            redraw();
            return;
        }
        commit_pending_preview();
    }

    void do_refresh() {
        if (m_config.host.empty()) {
            show_preferences();
            return;
        }
        /* Explicit refresh means "forget everything I know". */
        m_summary_cache.clear();
        m_body_cache.clear();
        m_worker.refresh();
    }

    /* Filter the current folder's summaries into the list widget. */
    void apply_filter() {
        std::string needle = m_filter;
        for (char &c : needle) c = (char)std::tolower((unsigned char)c);

        std::vector<EmailData> rows;
        rows.reserve(m_summaries.size());
        for (const MailSummary &s : m_summaries) {
            if (!needle.empty()) {
                std::string hay = s.from + "\n" + s.subject;
                for (char &c : hay) c = (char)std::tolower((unsigned char)c);
                if (hay.find(needle) == std::string::npos) continue;
            }
            EmailData d;
            d.seq     = s.seq;
            d.sender  = s.from;
            d.subject = s.subject;
            d.preview = s.preview;
            d.date    = s.date;
            d.seen    = s.seen;
            rows.push_back(d);
        }
        m_email_list->set_emails(std::move(rows));
    }

    /* ---- inline / remote images in the reading pane ---- */

    void clear_image_textures() {
        for (auto &kv : m_img_tex)
            nvgDeleteImage(nvg_context(), kv.second);
        m_img_tex.clear();
    }

    HtmlImageInfo make_image_info(int id) {
        HtmlImageInfo ri;
        if (id <= 0)
            return ri;
        ri.id = id;
        int w = 0, h = 0;
        nvgImageSize(nvg_context(), id, &w, &h);
        ri.w = (float)w;
        ri.h = (float)h;
        return ri;
    }

    int create_image_texture(const std::string &src, const std::string &bytes) {
        if (bytes.empty())
            return 0;
        int id = nvgCreateImageMem(nvg_context(), NVG_IMAGE_PREMULTIPLIED,
                                   (unsigned char *)bytes.data(),
                                   (int)bytes.size());
        if (id <= 0)
            return 0;
        m_img_tex[src] = id;
        return id;
    }

    /* Resolve an <img> src to an NVG image, creating the texture on first
       use.  Remote URLs load only after the user opts in; a cache miss
       kicks an async fetch and resolves to id 0 (placeholder) this pass. */
    HtmlImageInfo resolve_image(const std::string &src) {
        auto cached = m_img_tex.find(src);
        if (cached != m_img_tex.end())
            return make_image_info(cached->second);

        if (src.rfind("cid:", 0) == 0) {
            std::string cid = src.substr(4);
            for (const MailImage &img : m_current_message.images) {
                if (img.cid == cid)
                    return make_image_info(
                        create_image_texture(src, img.data));
            }
            return HtmlImageInfo{};
        }

        if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0) {
            m_has_remote_images = true;
            m_doc_remotes.insert(src);
            if (!m_show_remote_images)
                return HtmlImageInfo{};
            auto it = m_remote_bytes.find(src);
            if (it != m_remote_bytes.end()) {
                int id = create_image_texture(src, it->second);
                if (id <= 0)
                    m_remote_failed.insert(src);
                return make_image_info(id);
            }
            if (!m_remote_failed.count(src))
                queue_remote_fetch(src);
            return HtmlImageInfo{};
        }
        return HtmlImageInfo{};
    }

    void queue_remote_fetch(const std::string &url) {
        if (m_remote_pending.count(url) || m_remote_bytes.count(url) ||
            m_remote_failed.count(url))
            return;
        m_remote_pending.insert(url);
        m_fetch_queue.push_back(url);
        pump_fetches();
    }

    void pump_fetches() {
        while (m_fetch_inflight < kMaxInflight && !m_fetch_queue.empty()) {
            std::string url = m_fetch_queue.front();
            m_fetch_queue.pop_front();
            ++m_fetch_inflight;
            auto alive = m_alive;
            std::thread([this, url, alive]() {
                std::string bytes;
                bool ok = nmail_http_get(url, bytes);
                nanogui::async([this, url, ok, bytes = std::move(bytes),
                                alive]() mutable {
                    if (!alive || !*alive)
                        return;
                    on_fetch_done(url, ok, std::move(bytes));
                });
                glfwPostEmptyEvent();
            }).detach();
        }
    }

    void on_fetch_done(const std::string &url, bool ok, std::string &&bytes) {
        --m_fetch_inflight;
        m_remote_pending.erase(url);
        if (ok && !bytes.empty()) {
            m_remote_bytes[url] = std::move(bytes);
            if (!m_img_tex.count(url)) {
                int id = create_image_texture(url, m_remote_bytes[url]);
                if (id <= 0)
                    m_remote_failed.insert(url);
            }
        } else {
            m_remote_failed.insert(url);
        }

        if (m_has_message && m_view) {
            Vector2f sc = m_view_scroll ? m_view_scroll->scroll()
                                        : Vector2f(0.f, 0.f);
            m_view->bind_loaded_images();
            if (m_view_scroll)
                m_view_scroll->set_scroll(sc.y());
        }
        update_image_status();
        pump_fetches();
    }

    void update_image_status() {
        if (m_doc_remotes.empty() || !m_show_remote_images)
            return;
        int have = 0, fail = 0;
        int n = (int)m_doc_remotes.size();
        for (const auto &u : m_doc_remotes) {
            if (m_img_tex.count(u))
                ++have;
            else if (m_remote_failed.count(u))
                ++fail;
        }
        char buf[256];
        if (have + fail >= n) {
            if (fail)
                std::snprintf(buf, sizeof(buf),
                              "Images %d/%d (%d failed)", have, n, fail);
            else
                std::snprintf(buf, sizeof(buf), "Images %d/%d", have, n);
        } else {
            std::snprintf(buf, sizeof(buf), "Loading images %d/%d", have, n);
        }
        m_status->set_caption(buf);
    }

    /* (Re-)render the current message — on select, body arrival, theme
       change.  Image bytes bind in place via bind_loaded_images(). */
    void render_current() {
        if (!m_has_message) return;
        clear_image_textures();
        m_has_remote_images = false;
        m_doc_remotes.clear();
        const MailMessage &msg = m_current_message;
        if (!msg.html.empty()) {
            /* Rich render of the HTML part (preferred, like other
             * clients), with the header fields as a small HTML fragment
             * on top. */
            std::string h;
            h += "<p><span style=\"font-size:24px\"><b>" +
                 html_escape(msg.subject) + "</b></span></p>";
            h += "<p><b>From: </b>" + html_escape(msg.from) + "</p>";
            if (!msg.to.empty())
                h += "<p><b>To: </b>" + html_escape(msg.to) + "</p>";
            if (!msg.date.empty())
                h += "<p><b>Date: </b>" + html_escape(msg.date) + "</p>";
            h += "<hr>";
            m_view->set_html(h + msg.html);
            m_has_remote_images = m_view->has_remote_images();
        } else {
            Document doc;
            render_message(doc, msg, text_color(), meta_color());
            m_view->set_document(std::move(doc));
        }
        m_images_btn->set_enabled(m_has_remote_images &&
                                  !m_show_remote_images);
        m_view_scroll->set_scroll(0.0f);
        redraw();
    }

    void save_current_email() {
        if (!m_has_message) return;
        const MailMessage &msg = m_current_message;
        bool as_html = !msg.html.empty();
        auto paths = file_dialog(
            { {"html", "HTML email"}, {"txt", "Plain text"} },
            true, false, "");
        if (paths.empty() || paths[0].empty())
            return;
        std::string path = paths[0];
        bool has_ext = path.size() >= 5 &&
            (path.rfind(".html") == path.size() - 5 ||
             path.rfind(".htm") == path.size() - 4 ||
             path.rfind(".txt") == path.size() - 4);
        if (!has_ext)
            path += as_html ? ".html" : ".txt";

        SavedEmail e;
        e.from    = msg.from;
        e.to      = msg.to;
        e.subject = msg.subject;
        e.date    = msg.date;
        e.html    = msg.html;
        e.body    = msg.body;

        std::ofstream out(path, std::ios::binary);
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        std::string blob = nmail_serialize_email(e);
        out.write(blob.data(), (std::streamsize)blob.size());
        out.close();
        m_status->set_caption("Saved " + path);
    }

    /* ---- Preferences window ---- */
    void show_preferences() {
        Window *win = new Window(this, "IMAP Preferences", false);
        win->set_traffic_lights_mask(0x1);   // close (red) button only
        win->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill,
                                      12, 10));
        win->set_min_width(420);

        Widget *form = new Widget(win);
        form->set_layout(new GridLayout(Orientation::Horizontal, 2,
                                        Alignment::Middle, 0, 8));

        new Label(form, "IMAP server:", "sans-bold");
        TextBox *host = new TextBox(form);
        host->set_value(m_config.host);
        host->set_placeholder("imap.example.com");
        host->set_editable(true);

        new Label(form, "Port:", "sans-bold");
        IntBox<int> *port = new IntBox<int>(form);
        port->set_value(m_config.port);
        port->set_editable(true);

        new Label(form, "Username:", "sans-bold");
        TextBox *user = new TextBox(form);
        user->set_value(m_config.username);
        user->set_placeholder("you@example.com");
        user->set_editable(true);

        new Label(form, "Password:", "sans-bold");
        TextBox *pass = new TextBox(form);
        pass->set_value(m_config.password);
        pass->set_editable(true);

        new Label(form, "SMTP server:", "sans-bold");
        TextBox *smtp_host = new TextBox(form);
        smtp_host->set_value(m_config.smtp_host);
        smtp_host->set_placeholder("(same as IMAP server)");
        smtp_host->set_editable(true);

        new Label(form, "SMTP port:", "sans-bold");
        IntBox<int> *smtp_port = new IntBox<int>(form);
        smtp_port->set_value(m_config.smtp_port);
        smtp_port->set_editable(true);

        Widget *buttons = new Widget(win);
        buttons->set_layout(new BoxLayout(Orientation::Horizontal,
                                          Alignment::Middle, 0, 8));

        Button *save = new Button(buttons, "Save && Connect", FA_CHECK);
        save->set_callback([this, win, host, port, user, pass,
                            smtp_host, smtp_port]() {
            m_config.host     = host->value();
            m_config.port     = port->value();
            m_config.username = user->value();
            m_config.password = pass->value();
            m_config.smtp_host = smtp_host->value();
            m_config.smtp_port = smtp_port->value();
            save_config(m_config);
            win->dispose();
            m_worker.set_config(m_config);
            m_worker.connect();
        });

        Button *cancel = new Button(buttons, "Cancel", FA_TIMES);
        cancel->set_callback([win]() { win->dispose(); });

        win->center();
        win->request_focus();
    }

    /* ---- Reply compose window ---- */
    void show_compose() {
        if (!m_has_message) return;
        const MailMessage orig = m_current_message;   // copy: stays stable

        Window *win = new Window(this, "Reply", false);
        win->set_traffic_lights_mask(0x1);   // close (red) button only
        win->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill,
                                      12, 10));
        win->set_min_width(760);
        win->set_min_height(460);

        Widget *form = new Widget(win);
        form->set_layout(new GridLayout(Orientation::Horizontal, 2,
                                        Alignment::Middle, 0, 8));

        new Label(form, "To:", "sans-bold");
        TextBox *to = new TextBox(form);
        to->set_value(orig.from_addr.empty() ? orig.from : orig.from_addr);
        to->set_editable(true);

        new Label(form, "Subject:", "sans-bold");
        TextBox *subj = new TextBox(form);
        std::string s = orig.subject;
        if (s.size() < 3 || (s[0] != 'R' && s[0] != 'r') ||
            (s[1] != 'e' && s[1] != 'E') || s[2] != ':')
            s = "Re: " + s;
        subj->set_value(s);
        subj->set_editable(true);

        /* Format toolbar: WYSIWYG style toggles (Ctrl+B/I/U also work). */
        Widget *fmt = new Widget(win);
        fmt->set_layout(new BoxLayout(Orientation::Horizontal,
                                      Alignment::Middle, 0, 4));

        TextEditor *body = new TextEditor(win, TextEditor::Mode::RichText);
        body->set_read_only(false);
        body->set_background_color(m_dark ? Color(30, 31, 38, 255)
                                          : Color(250, 250, 252, 255));
        Style bs;
        bs.fgColor = text_color();
        bs.fontSize = 16.f;
        body->set_default_style(bs);
        body->set_min_height(300);
        body->set_padding(10);

        auto make_fmt = [&](int icon, TextEditor::StyleFlag f,
                            const std::string &tip) {
            Button *b = new Button(fmt, "", icon);
            b->set_flags(Button::Flags::ToggleButton);
            b->set_font_size(20);
            b->set_tooltip(tip);
            b->set_callback([body, f]() { body->toggle_style(f); });
            return b;
        };
        Button *fmt_b = make_fmt(FA_BOLD,      TextEditor::StyleFlag::Bold,
                                 "Bold (Ctrl+B)");
        Button *fmt_i = make_fmt(FA_ITALIC,    TextEditor::StyleFlag::Italic,
                                 "Italic (Ctrl+I)");
        Button *fmt_u = make_fmt(FA_UNDERLINE, TextEditor::StyleFlag::Underline,
                                 "Underline (Ctrl+U)");

        /* Paragraph-level formatting: headings, code block, bullet list.
         * These restyle the whole caret paragraph (see TextEditor). */
        auto make_par = [&](const std::string &caption, int icon,
                            const std::string &tip,
                            std::function<void()> fn) {
            Button *b = icon ? new Button(fmt, "", icon)
                             : new Button(fmt, caption);
            b->set_flags(Button::Flags::ToggleButton);
            b->set_font_size(icon ? 20 : 15);
            b->set_tooltip(tip);
            b->set_callback([fn]() { fn(); });
            return b;
        };
        Button *fmt_h1 = make_par("H1", 0, "Heading 1",
                                  [body]() { body->set_paragraph_header(1); });
        Button *fmt_h2 = make_par("H2", 0, "Heading 2",
                                  [body]() { body->set_paragraph_header(2); });
        Button *fmt_h3 = make_par("H3", 0, "Heading 3",
                                  [body]() { body->set_paragraph_header(3); });
        Button *fmt_cb = make_par("</>", 0, "Code block",
                                  [body]() { body->toggle_paragraph_code(); });
        Button *fmt_ls = make_par("", FA_LIST_UL, "Bullet list",
                                  [body]() { body->toggle_paragraph_bullet(); });

        /* Toolbar state follows the caret. */
        std::function<void()> refresh_fmt =
            [body, fmt_b, fmt_i, fmt_u,
             fmt_h1, fmt_h2, fmt_h3, fmt_cb, fmt_ls]() {
            Style st = body->style_at_caret();
            fmt_b->set_pushed(st.bold);
            fmt_i->set_pushed(st.italic);
            fmt_u->set_pushed(st.underline);
            int h = body->paragraph_header();
            fmt_h1->set_pushed(h == 1);
            fmt_h2->set_pushed(h == 2);
            fmt_h3->set_pushed(h == 3);
            fmt_cb->set_pushed(body->paragraph_code());
            fmt_ls->set_pushed(body->paragraph_bullet());
        };
        body->caret_callback = [refresh_fmt](TextEditor::Position) {
            refresh_fmt();
        };
        body->change_callback = refresh_fmt;

        /* Prefill: empty paragraph for the reply, then the quoted original
         * as indented paragraphs (serialized back to "> " lines). */
        {
            Document *doc = body->document().get();
            doc->paragraphs.clear();
            doc->addParagraph();   // reply goes here
            doc->addParagraph();   // spacer

            Style meta_s = bs; meta_s.fgColor = meta_color();
            doc->addParagraph("On " + orig.date + ", " + orig.from +
                              " wrote:", meta_s);

            std::istringstream iss(orig.body);
            std::string qline;
            while (std::getline(iss, qline)) {
                if (!qline.empty() && qline.back() == '\r') qline.pop_back();
                if (qline.empty()) continue;
                Paragraph *qp = doc->addParagraph(qline, bs);
                qp->leftIndent = 16.0f;
            }
            doc->markLayoutDirty();
        }
        body->set_caret({0, 0});
        refresh_fmt();

        Widget *buttons = new Widget(win);
        buttons->set_layout(new BoxLayout(Orientation::Horizontal,
                                          Alignment::Middle, 0, 8));

        /* Send format: plain text, Markdown (MailMate-style markup=
         * markdown), or a generated HTML body. */
        new Label(buttons, "Format:", "sans-bold");
        Dropdown *fmt_box = new Dropdown(buttons, Dropdown::ComboBox,
                                         "Format");
        /* NB: use the 5-arg add_item — the 2-arg overload installs no
         * callback, so clicking an item would never update the selection. */
        fmt_box->add_item({"Plain text", "fmt_plain"}, FA_FONT,
                          [] {}, {{0, 0}}, true);
        fmt_box->add_item({"Markdown", "fmt_markdown"}, FA_HASHTAG,
                          [] {}, {{0, 0}}, true);
        fmt_box->add_item({"HTML", "fmt_html"}, FA_CODE,
                          [] {}, {{0, 0}}, true);
        fmt_box->set_selected_index(1);   // Markdown
        fmt_box->set_tooltip(
            "Plain: raw text.  Markdown: plain text with markup=markdown; "
            "aware clients render it styled.  HTML: generated text/html");

        Button *send = new Button(buttons, "Send", FA_PAPER_PLANE);
        send->set_callback([this, win, send, to, subj, body, fmt_box]() {
            std::string to_s  = to->value();
            std::string sub_s = subj->value();
            if (to_s.empty()) {
                auto *dlg = new MessageDialog(this,
                    MessageDialog::Type::Warning, "Missing recipient",
                    "Enter a recipient address first.", "OK", "", false);
                dlg->center();
                return;
            }
            send->set_enabled(false);
            int fmt = fmt_box->selected_index();
            if (fmt < 0) fmt = 1;   // default to Markdown
            MailFormat format = fmt == 0 ? MailFormat::Plain
                              : fmt == 2 ? MailFormat::Html
                                         : MailFormat::Markdown;
            std::string text = fmt == 0 ? body->plain_text()
                             : fmt == 2 ? document_to_html(*body->document())
                                        : document_to_markdown(*body->document());
            send_reply(win, send, to_s, sub_s, text, format);
        });

        Button *cancel = new Button(buttons, "Cancel", FA_TIMES);
        cancel->set_callback([win]() { win->dispose(); });

        win->center();
        win->request_focus();
    }

    /* Send on a one-shot thread (SMTP is a separate connection from the
     * IMAP worker); the result is marshalled back with nanogui::async. */
    void send_reply(Window *win, Button *send_btn,
                    const std::string &to, const std::string &subject,
                    const std::string &body, MailFormat format) {
        SmtpConfig sc;
        sc.host     = m_config.smtp_host.empty() ? m_config.host
                                                 : m_config.smtp_host;
        sc.port     = m_config.smtp_port;
        sc.username = m_config.username;
        sc.password = m_config.password;
        std::string from = m_config.username;
        std::string irt  = m_current_message.message_id;

        m_status->set_caption("Sending reply...");
        std::thread([this, win, send_btn, sc, from, to, subject, body,
                     irt, format]() {
            SmtpClient smtp;
            std::string err;
            bool ok = smtp.send(sc, from, to, subject, body, irt, format,
                                err);
            nanogui::async(std::function<void()>(
                [this, win, send_btn, ok, err]() {
                    if (ok) {
                        m_status->set_caption("Reply sent");
                        win->dispose();
                    } else {
                        m_status->set_caption("Send failed");
                        send_btn->set_enabled(true);
                        auto *dlg = new MessageDialog(this,
                            MessageDialog::Type::Warning,
                            "Could not send reply", err, "OK", "", false);
                        dlg->center();
                    }
                }));
            glfwPostEmptyEvent();
        }).detach();
    }

    virtual bool keyboard_event(int key, int scancode,
                                int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        // Ctrl/Cmd+T toggles light/dark theme at runtime
        if (key == GLFW_KEY_T && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            apply_theme_mode(m_dark ? ThemeMode::Light : ThemeMode::Dark);
            return true;
        }
        // Ctrl/Cmd+R refreshes from the server
        if (key == GLFW_KEY_R && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            do_refresh();
            return true;
        }
        // Ctrl/Cmd+S saves the current message as HTML for nmail_view
        if (key == GLFW_KEY_S && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            save_current_email();
            return true;
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        pump_preview();
        // Background gradient
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, m_size.x(), m_size.y());
        NVGpaint bg = m_dark
            ? nvgLinearGradient(ctx, 0, 0, 0, (float)m_size.y(),
                                nvgRGBA(42, 44, 52, 255),
                                nvgRGBA(30, 31, 38, 255))
            : nvgLinearGradient(ctx, 0, 0, 0, (float)m_size.y(),
                                nvgRGBA(235, 237, 242, 255),
                                nvgRGBA(225, 228, 235, 255));
        nvgFillPaint(ctx, bg);
        nvgFill(ctx);
        nvgRestore(ctx);
        Screen::draw(ctx);
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    try {
        /* The IMAP worker writes to a socket the server may have closed;
         * nanoproxy's socket_write handles EPIPE, but only if SIGPIPE
         * doesn't kill the process first. */
        signal(SIGPIPE, SIG_IGN);
        nanogui::init();
        {
            ref<MailApp> app = new MailApp();
            app->dec_ref();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop(-1);
        }
        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::string error_msg =
            std::string("Caught a fatal error: ") + std::string(e.what());
        std::cerr << error_msg << std::endl;
        return -1;
    }
    return 0;
}
