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
 *
 * This file holds the application shell (MailApp) and main().  The pieces
 * broken out of it:
 *   nmail_config.h/.cpp  — MailConfig, config file I/O, password encryption
 *   mail_worker.h/.cpp   — MailWorker, the IMAP worker thread
 *   mail_widgets.h/.cpp  — FolderView sidebar and EmailListView widgets
 *   mail_format.h/.cpp   — Markdown/Document/HTML conversion, header card
 *   mail_debug.h         — mail_dbg() logging macro
 */

#include "nanogui/widget.h"
#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanogui/split.h>
#include <nanogui/layout.h>
#include <nanogui/icons.h>
#include <nanogui/texteditor.h>
#include <nanogui/textbox.h>
#include <nanogui/menu.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/messagedialog.h>
#include <nanogui/spinner.h>
#include <nanogui/autocomplete.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <fstream>
#include <functional>
#include <iterator>
#include <vector>
#include <string>
#include <sstream>
#include <array>
#include <initializer_list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <csignal>

#include "imap_client.h"

#include "mail_debug.h"
#include "smtp_client.h"
#include "http_fetch.h"
#include "htmldocument.h"
#include "attachment_widgets.h"
#include "contacts.h"
#include <memory>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#ifdef _WIN32
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libgen.h>
#endif
#include <stb_image.h>
#include "nmail_icon.h"
#include "nmail_config.h"
#include "mail_worker.h"
#include "mail_widgets.h"
#include "nmail_toolbar.h"
#include "mail_format.h"

using namespace nanogui;

/* Give the window the nmail logo.  Works on Windows and X11; GLFW documents
   this as unsupported on macOS (the bundle icon is used) and on Wayland. */
static void set_window_icon(GLFWwindow *win) {
#if !defined(__APPLE__)
    std::vector<GLFWimage> images;
    std::vector<unsigned char *> pixels;
    for (const NmailIconBlob &blob : nmail_icon_pngs) {
        int w = 0, h = 0, ch = 0;
        unsigned char *px = stbi_load_from_memory(blob.data, (int)blob.size,
                                                  &w, &h, &ch, 4);
        if (!px) continue;
        pixels.push_back(px);
        GLFWimage img;
        img.width  = w;
        img.height = h;
        img.pixels = px;
        images.push_back(img);
    }
    if (!images.empty())
        glfwSetWindowIcon(win, (int)images.size(), images.data());
    for (unsigned char *px : pixels)
        stbi_image_free(px);
#else
    (void)win;
#endif
}

/* Launched from Finder a bundle gets "/" as its working directory, but
   theme.cpp loads "resources/NotoColorEmoji.ttf" relative to it.  Point the
   CWD at Contents/Resources, where the CMake bundle rules put the faces. */
static void chdir_to_bundle_resources() {
#ifdef __APPLE__
    char exe[4096];
    uint32_t len = sizeof(exe);
    if (_NSGetExecutablePath(exe, &len) != 0) return;
    /* <bundle>/Contents/MacOS/nmail -> <bundle>/Contents/Resources */
    std::string dir = dirname(exe);            /* .../Contents/MacOS   */
    std::string res = dir + "/../Resources";
    if (::chdir(res.c_str()) != 0)
        std::cerr << "[nmail] could not chdir to " << res << std::endl;
#endif
}

// ---------------------------------------------------------------------------
// Attachment helpers that stay with MailApp (position mapping).
// ---------------------------------------------------------------------------

/* Widget::absolute_position walks m_pos and does not see ZoomScrollPanel's
 * pan/zoom draw transform. Map a logical screen point to where it is drawn. */
static Vector2i visual_screen_pos(const Widget *w, const Vector2i &logical_abs) {
    const ZoomScrollPanel *zsp = nullptr;
    for (const Widget *p = w; p && !zsp; p = p->parent())
        zsp = dynamic_cast<const ZoomScrollPanel *>(p);
    if (!zsp)
        return logical_abs;
    Vector2i zsp_abs = zsp->absolute_position();
    Vector2i rel = logical_abs - zsp_abs;
    double z = zsp->zoom();
    auto pan = zsp->pan_offset();
    return zsp_abs + Vector2i(
        (int)std::lround(pan.x() + rel.x() * z),
        (int)std::lround(pan.y() + rel.y() * z));
}

// ---------------------------------------------------------------------------
// MailApp — the application
// ---------------------------------------------------------------------------
// AccountSession — everything that is per-connection state: one IMAP/SMTP
// account's config, its own worker thread, its own folder list, and its own
// summary/body caches. Every configured account gets one of these and they
// all run concurrently (see MailApp::m_accounts).
// ---------------------------------------------------------------------------
struct AccountSession {
    MailAccount              config;
    std::unique_ptr<MailWorker> worker = std::make_unique<MailWorker>();
    std::vector<MailFolder>  folders;
    std::string              current_folder;   // folder whose list is on screen (meaningful only while active)
    std::string              wanted_folder;     // folder the user last clicked, for this account
    bool                     folder_loading = false;
    bool                     older_inflight = false;
    bool                     move_inflight  = false;
    /* Seq of a message we just MOVE'd.  The server's EXPUNGE echo of that
     * MOVE must not run on_expunged after we have already resequenced. */
    std::string              local_expunge_folder;
    int                      local_expunge_seq = 0;
    /* Session-only caches; dropped on Refresh or reconnect. */
    std::map<std::string, std::vector<MailSummary>> summary_cache;
    std::map<std::string, MailMessage>              body_cache;
};

// ---------------------------------------------------------------------------
class MailApp : public Screen {
public:
    FolderView   *m_folder_view = nullptr;
    EmailListView *m_email_list = nullptr;
    ZoomScrollPanel *m_view_scroll = nullptr;
    HtmlDocument *m_view        = nullptr;
    Label        *m_status      = nullptr;
    Label        *m_compress_label = nullptr;
    IndeterminateBar *m_load_bar = nullptr;
    Button       *m_theme_btn   = nullptr;
    Button       *m_compose_btn = nullptr;
    Button       *m_reply_btn   = nullptr;
    Button       *m_fwd_btn     = nullptr;
    Button       *m_save_btn    = nullptr;
    Button       *m_images_btn  = nullptr;
    Button       *m_trash_btn   = nullptr;
    Button       *m_junk_btn    = nullptr;
    Button       *m_restore_btn = nullptr;
    /* Message-list filter bar. Date and Attachment are exclusive sort
     * keys; Unread composes with either. VIP is a placeholder. */
    Button       *m_sort_date_btn   = nullptr;
    Button       *m_sort_unread_btn = nullptr;
    Button       *m_sort_attach_btn = nullptr;
    Button       *m_sort_vip_btn    = nullptr;
    Button       *m_sort_dir_btn    = nullptr;
    bool         m_sort_by_size     = false;
    bool         m_filter_unread    = false;
    bool         m_filter_attach    = false;
    bool         m_sort_desc        = true;

    MailConfig  m_config;
    bool        m_config_loaded = false;
    bool        m_dark          = false;

    /* One session per configured account; all connect and poll at once.
     * m_current_account_id names whichever account's folder is on screen
     * (m_summaries / m_email_list / m_view below are shared, singular UI --
     * only one account's message list is ever visible at a time). */
    std::vector<std::unique_ptr<AccountSession>> m_accounts;
    std::string m_current_account_id;

    AccountSession *account(const std::string &id) {
        for (auto &a : m_accounts) if (a->config.id() == id) return a.get();
        return nullptr;
    }
    const AccountSession *account(const std::string &id) const {
        return const_cast<MailApp*>(this)->account(id);
    }
    /* Whichever account is on screen. Before any folder has ever been
     * selected (or if the active account was since removed), returns a
     * harmless never-connected placeholder -- every field on it reads as
     * "nothing selected" (empty folder, not loading, ...), which is exactly
     * the right answer for e.g. update_move_buttons() at startup. */
    AccountSession &current_acct() {
        static AccountSession dummy;
        AccountSession *a = account(m_current_account_id);
        return a ? *a : dummy;
    }
    const AccountSession &current_acct() const {
        return const_cast<MailApp*>(this)->current_acct();
    }

    std::vector<MailSummary> m_summaries;   // current folder, newest first
    std::string              m_filter;
    int                      m_loading_seq = -1;
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
    /* Dwell time before a viewed message is flagged \Seen on the server. */
    static constexpr double  kMarkReadSec = 2.0;

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

    // UID-aware body cache key: "folder|U<uid>" when uid!=0 else "folder|S<seq>".
    // Keeps seq path working when uid==0 (server without CONDSTORE/QRESYNC).
    static std::string body_key(const std::string &folder, uint32_t uid, int seq) {
        if (uid) return folder + "|U" + std::to_string(uid);
        return folder + "|S" + std::to_string(seq);
    }
    static std::string body_key(const std::string &folder, const MailSummary &s) {
        return body_key(folder, s.uid, s.seq);
    }
    static std::string body_key(const std::string &folder, const EmailData &d) {
        return body_key(folder, d.uid, d.seq);
    }
    /* Every cache helper below takes an explicit AccountSession& so callback
     * handlers (which may be reporting for a background, not-currently-
     * viewed account) always update *that* account's cache. Overloads
     * without an account default to current_acct() -- the account whose
     * folder is on screen -- for the UI-driven call sites (viewport
     * prefetch, render, selection) that only ever act on what's visible. */

    // Resolve uid for a seq via current summaries/cache (for on_body/on_prefetched where only seq is known).
    uint32_t uid_for_seq(AccountSession &acct, const std::string &folder, int seq) const {
        if (seq <= 0) return 0;
        if (&acct == &current_acct() && folder == current_acct().current_folder) {
            for (auto &s : m_summaries) if (s.seq == seq && s.uid) return s.uid;
        }
        auto it = acct.summary_cache.find(folder);
        if (it != acct.summary_cache.end())
            for (auto &s : it->second) if (s.seq == seq && s.uid) return s.uid;
        return 0;
    }
    uint32_t uid_for_seq(const std::string &folder, int seq) const {
        return uid_for_seq(const_cast<MailApp*>(this)->current_acct(), folder, seq);
    }
    uint32_t uid_for_seq_any(int seq) const { return uid_for_seq(current_acct().current_folder, seq); }
    // Dual-read helper: check UID key first (authoritative on QRESYNC), then SEQ key, then legacy ":" key.
    bool body_has(const AccountSession &acct, const std::string &folder, uint32_t uid, int seq) const {
        const auto &cache = acct.body_cache;
        if (uid) {
            if (cache.find(body_key(folder, uid, 0)) != cache.end()) return true;
            // fallback to seq key if seq is known (dual-write period)
            if (seq && cache.find(body_key(folder, 0, seq)) != cache.end()) return true;
            if (seq && cache.find(folder + ":" + std::to_string(seq)) != cache.end()) return true;
            return false;
        }
        if (cache.find(body_key(folder, 0, seq)) != cache.end()) return true;
        if (cache.find(folder + ":" + std::to_string(seq)) != cache.end()) return true;
        return false;
    }
    bool body_has(const std::string &folder, uint32_t uid, int seq) const {
        return body_has(current_acct(), folder, uid, seq);
    }
    bool body_has(const std::string &folder, const EmailData &d) const { return body_has(folder, d.uid, d.seq); }
    auto body_find(const AccountSession &acct, const std::string &folder, uint32_t uid, int seq) const {
        const auto &cache = acct.body_cache;
        if (uid) {
            auto it = cache.find(body_key(folder, uid, 0));
            if (it != cache.end()) return it;
            if (seq) {
                it = cache.find(body_key(folder, 0, seq));
                if (it != cache.end()) return it;
                it = cache.find(folder + ":" + std::to_string(seq));
                if (it != cache.end()) return it;
            }
            return cache.end();
        }
        auto it = cache.find(body_key(folder, 0, seq));
        if (it != cache.end()) return it;
        it = cache.find(folder + ":" + std::to_string(seq));
        return it;
    }
    auto body_find(const std::string &folder, uint32_t uid, int seq) const {
        return body_find(current_acct(), folder, uid, seq);
    }
    auto body_find(const std::string &folder, const EmailData &d) const { return body_find(folder, d.uid, d.seq); }
    void body_put(AccountSession &acct, const std::string &folder, uint32_t uid, int seq, const MailMessage &msg) {
        auto &cache = acct.body_cache;
        if (cache.size() > 256) cache.clear();
        if (uid) {
            cache[body_key(folder, uid, 0)] = msg;
            if (seq) {
                // dual-write for transition: keep seq key so old lookups still hit
                cache[body_key(folder, 0, seq)] = msg;
                cache[folder + ":" + std::to_string(seq)] = msg;
            }
        } else if (seq) {
            cache[body_key(folder, 0, seq)] = msg;
            cache[folder + ":" + std::to_string(seq)] = msg;
        }
    }
    void body_put(const std::string &folder, uint32_t uid, int seq, const MailMessage &msg) {
        body_put(current_acct(), folder, uid, seq, msg);
    }
    void body_put(const std::string &folder, const EmailData &d, const MailMessage &msg) { body_put(folder, d.uid, d.seq, msg); }

    /* After EXPUNGE/MOVE: drop the vanished UID key and every seq-keyed
     * body for this folder (those numbers just shifted). Other UID keys
     * stay — they are stable and the next click should not re-FETCH. */
    void drop_expunged_bodies(AccountSession &acct, const std::string &folder,
                              int seq, uint32_t uid) {
        std::vector<std::string> drop;
        const std::string gone_u = uid ? body_key(folder, uid, 0) : "";
        const std::string s_prefix = folder + "|S";
        const std::string l_prefix = folder + ":";
        for (auto &kv : acct.body_cache) {
            const std::string &k = kv.first;
            if (!gone_u.empty() && k == gone_u) { drop.push_back(k); continue; }
            if (k.rfind(s_prefix, 0) == 0 || k.rfind(l_prefix, 0) == 0)
                drop.push_back(k);
        }
        for (auto &k : drop) acct.body_cache.erase(k);
    }
    std::string m_status_base;

    ContactStore m_contacts;

    static const std::string &contacts_path() {
        static const std::string p = config_file("contacts.json");
        return p;
    }

    /* Every correspondent we see becomes a completion candidate. */
    void harvest(const std::vector<MailSummary> &sums) {
        for (const MailSummary &s : sums)
            m_contacts.observe(s.from, s.from_addr);
    }
    void harvest(const MailMessage &msg) {
        m_contacts.observe(msg.from, msg.from_addr);
        m_contacts.observe_header(msg.to);
    }
    std::string m_hover_url;
    /* Header recipients the user has clicked to reveal, lowercased.
     * Reset whenever a different message is rendered. */
    std::set<std::string> m_expanded_addrs;

    PopupMenu *m_att_popup = nullptr;
    struct AttTemp { std::string file; std::string dir; };
    std::vector<AttTemp> m_att_temps;
    bool     m_att_preview = false;
    Vector2f m_att_preview_scroll{0.f, 0.f};
    std::string m_att_preview_name;
    struct GalleryItem {
        size_t att_index = 0;
        bool   decodable = false;
    };
    std::vector<GalleryItem> m_gallery;
    int m_gallery_sel = -1;

    /* Taskbar: one button per open dialog, bottom-right of the root window.
     * Windows opt in by carrying one of the ids below. */
    Widget *m_taskbar = nullptr;
    std::vector<Window *> m_task_order;   // stable creation order

    static bool taskbar_kind(const std::string &id, int &icon,
                             std::string &tip) {
        if (id == "nmail-prefs")   { icon = FA_SLIDERS_H; tip = "Preferences";  return true; }
        if (id == "nmail-compose") { icon = FA_PEN;       tip = "New Message";  return true; }
        if (id == "nmail-reply")   { icon = FA_REPLY;     tip = "Reply";        return true; }
        if (id == "nmail-forward") { icon = FA_SHARE;     tip = "Forward";      return true; }
        return false;
    }

    /* Single exit for the dialogs the taskbar tracks.  Dispose once the event
     * stack has unwound, then refresh the buttons and ask for a frame: the
     * taskbar syncs during draw(), and closing a window does not by itself
     * request a redraw. */
    void close_dialog(Window *win) {
        if (!win) return;
        auto alive = m_alive;
        nanogui::async([this, win, alive] {
            if (!*alive) return;
            if (win && win->parent()) win->dispose();
            sync_taskbar();
            redraw();
        });
        redraw();
        glfwPostEmptyEvent();
    }

    bool is_live_window(const Window *w) const {
        for (const Widget *c : children())
            if (c == w) return true;
        return false;
    }

    /* Rebuild the buttons when the set of open dialogs changes.  Driven off
     * the live child list rather than close callbacks, so a window disposed
     * by any route simply stops appearing. */
    void sync_taskbar() {
        if (!m_taskbar) return;

        std::vector<Window *> live;
        for (Widget *c : children()) {
            auto *w = dynamic_cast<Window *>(c);
            if (!w || w->is_root() || dynamic_cast<Popup *>(w) || !w->visible())
                continue;
            int icon; std::string tip;
            if (taskbar_kind(w->id(), icon, tip)) live.push_back(w);
        }

        /* children() gets reordered every time a window is raised, so keep
         * our own order: drop the closed, append the newly opened. */
        std::vector<Window *> order;
        for (Window *w : m_task_order)
            if (std::find(live.begin(), live.end(), w) != live.end())
                order.push_back(w);
        for (Window *w : live)
            if (std::find(order.begin(), order.end(), w) == order.end())
                order.push_back(w);

        if (order == m_task_order &&
            m_taskbar->child_count() == (int)order.size())
            return;                                  // nothing changed
        m_task_order = order;

        while (m_taskbar->child_count() > 0)
            m_taskbar->remove_child_at(m_taskbar->child_count() - 1);

        for (Window *w : m_task_order) {
            int icon = 0; std::string tip;
            taskbar_kind(w->id(), icon, tip);
            Button *b = new Button(m_taskbar, "", icon);
            b->set_tooltip(tip);
            b->set_fixed_size(Vector2i(30, 22));
            b->set_callback([this, w] {
                /* The window may have been disposed between the rebuild that
                 * created this button and the click. */
                if (!is_live_window(w)) { sync_taskbar(); return; }
                w->set_visible(true);
                move_window_to_front(w);
                w->request_focus();
                sync_taskbar();
                redraw();
            });
        }
        perform_layout();
    }

    // helpers for Trash/Junk moves
    std::string resolve_dest_folder(const std::string &kind) const {
        auto lower = [](std::string s) {
            for (char &c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        const std::vector<MailFolder> &folders = current_acct().folders;
        if (folders.empty()) return kind;
        std::string want = lower(kind);
        // exact leaf match first
        for (const auto &f : folders) {
            std::string low = lower(f.name);
            size_t p = low.find_last_of("/.");
            std::string leaf = (p == std::string::npos) ? low : low.substr(p + 1);
            if (leaf == want) return f.name;
        }
        std::vector<std::string> keys;
        if (want == "trash") keys = {"trash","deleted","deleted messages","bin"};
        else if (want == "junk") keys = {"junk","spam","junk email","bulk mail"};
        else keys = {want};
        for (const std::string &k : keys) {
            for (const auto &f : folders) {
                std::string low = lower(f.name);
                size_t p = low.find_last_of("/.");
                std::string leaf = (p == std::string::npos) ? low : low.substr(p + 1);
                if (leaf.find(k) != std::string::npos) return f.name;
                if (low.find(k) != std::string::npos) return f.name;
            }
        }
        return kind;
    }
    static std::string link_domain(const std::string &url) {
        std::string s = url;
        // trim whitespace
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        s = s.substr(b, e - b + 1);
        std::string low; low.reserve(s.size());
        for (char c : s) low.push_back((char)std::tolower((unsigned char)c));
        std::string host;
        if (low.rfind("http://", 0) == 0) host = s.substr(7);
        else if (low.rfind("https://", 0) == 0) host = s.substr(8);
        else if (low.rfind("mailto:", 0) == 0) {
            size_t at = s.find('@');
            if (at != std::string::npos) {
                size_t end = s.find_first_of(" ?#", at);
                host = s.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
                // strip trailing > etc
                while (!host.empty() && (host.back() == '>' || host.back() == '"' || host.back() == '\'')) host.pop_back();
                if (host.rfind("www.", 0) == 0) host = host.substr(4);
                return host;
            }
            return s.substr(7);
        } else {
            // unknown scheme - return as-is up to slash
            host = s;
        }
        // strip userinfo if present
        size_t at = host.rfind('@');
        if (at != std::string::npos) host = host.substr(at + 1);
        // cut at / ? #
        size_t end = host.find_first_of("/?#");
        if (end != std::string::npos) host = host.substr(0, end);
        // strip port
        size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
        if (host.rfind("www.", 0) == 0) host = host.substr(4);
        return host;
    }
    void set_status(const std::string &s) {
        m_status_base = s;
        if (m_hover_url.empty() && m_status) m_status->set_caption(s);
    }
    std::string compressSuffix() const {
        return current_acct().worker->is_compressed() ? " \u00B7 DEFLATE" : "";
    }
    void update_compress_badge() {
        bool comp = current_acct().worker->is_compressed();
        if (m_compress_label) {
            m_compress_label->set_visible(comp);
            if (comp) {
                m_compress_label->set_caption("DEFLATE");
                m_compress_label->set_color(Color(40, 160, 60, 255));
            } else {
                m_compress_label->set_caption("");
            }
            // re-layout statusbar if visibility changed
            if (m_compress_label->parent()) perform_layout();
        }
    }
    /* Folder-open busy flag + optional status-bar indeterminate bar. */
    void set_folder_busy(bool busy, const std::string &msg = "") {
        current_acct().folder_loading = busy;
        if (m_load_bar) {
            if (busy) m_load_bar->start();
            else      m_load_bar->stop();
        }
        if (!msg.empty()) set_status(msg);
        redraw();
    }
    void handle_link_hover(const std::string &url) {
        if (url.empty()) {
            m_hover_url.clear();
            if (m_status) m_status->set_caption(m_status_base.empty() ? "Ready" : m_status_base);
            return;
        }
        if (url.compare(0, std::strlen(kAddrScheme), kAddrScheme) == 0) {
            const std::string addr = url.substr(std::strlen(kAddrScheme));
            std::string low_a = addr;
            for (char &c : low_a) c = (char)std::tolower((unsigned char)c);
            m_hover_url.clear();
            if (m_status)
                m_status->set_caption(m_expanded_addrs.count(low_a)
                                          ? "Click to show the display name"
                                          : "Click to show " + addr);
            return;
        }
        std::string low; low.reserve(url.size());
        for (char c : url) low.push_back((char)std::tolower((unsigned char)c));
        bool is_http = (low.rfind("http://", 0) == 0 || low.rfind("https://", 0) == 0);
        if (!is_http) {
            // non-HTTP links (e.g. mailto) keep normal status; don't show hover
            m_hover_url.clear();
            if (m_status) m_status->set_caption(m_status_base.empty() ? "Ready" : m_status_base);
            return;
        }
        std::string dom = link_domain(url);
        if (dom.empty()) dom = url;
        m_hover_url = url;
        if (m_status) m_status->set_caption("Open link to " + dom);
    }
    void update_move_buttons() {
        const std::string &cur_folder = current_acct().current_folder;
        bool has_sel = m_email_list && m_email_list->selected_seq() != -1
                       && !cur_folder.empty() && !current_acct().move_inflight;
        auto lower = [](std::string s) {
            for (char &c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        std::string curLow = lower(cur_folder);
        std::string trashDest = cur_folder.empty() ? "" : resolve_dest_folder("Trash");
        std::string junkDest  = cur_folder.empty() ? "" : resolve_dest_folder("Junk");
        bool trashSame = !trashDest.empty() && lower(trashDest) == curLow;
        bool junkSame  = !junkDest.empty() && lower(junkDest) == curLow;
        bool in_trash_or_junk = trashSame || junkSame;
        if (m_trash_btn) m_trash_btn->set_enabled(has_sel && !trashSame);
        if (m_junk_btn)  m_junk_btn->set_enabled(has_sel && !junkSame);
        if (m_restore_btn) m_restore_btn->set_enabled(has_sel && in_trash_or_junk);
        /* While viewing Trash/Junk, hide the trash/junk buttons and show
         * a single "restore to Inbox" button instead. */
        bool vis_changed = false;
        if (m_trash_btn && m_trash_btn->visible() == in_trash_or_junk) {
            m_trash_btn->set_visible(!in_trash_or_junk); vis_changed = true;
        }
        if (m_junk_btn && m_junk_btn->visible() == in_trash_or_junk) {
            m_junk_btn->set_visible(!in_trash_or_junk); vis_changed = true;
        }
        if (m_restore_btn && m_restore_btn->visible() != in_trash_or_junk) {
            m_restore_btn->set_visible(in_trash_or_junk); vis_changed = true;
        }
        if (vis_changed) perform_layout();
        // keep tooltips reflecting resolved destination
        if (m_trash_btn) m_trash_btn->set_tooltip(has_sel && !trashDest.empty()
            ? "Move to " + trashDest + " (Delete)" : "Move to Trash (Delete)");
        if (m_junk_btn)  m_junk_btn->set_tooltip(has_sel && !junkDest.empty()
            ? "Move to " + junkDest : "Move to Junk / Spam");
    }
    void move_selected_to(const std::string &kind) {
        AccountSession &acct = current_acct();
        if (acct.move_inflight) return;
        if (!m_email_list) return;
        int seq = m_email_list->selected_seq();
        if (seq <= 0) { set_status("No message selected"); return; }
        if (acct.current_folder.empty()) return;
        std::string dest = resolve_dest_folder(kind);
        if (dest.empty()) { set_status("No " + kind + " folder found"); return; }
        auto lower = [](std::string s){ for(char &c:s) c=(char)std::tolower((unsigned char)c); return s; };
        if (lower(dest) == lower(acct.current_folder)) {
            set_status("Already in " + dest);
            return;
        }
        acct.move_inflight = true;
        update_move_buttons();
        set_status("Moving to " + dest + "...");
        acct.worker->cancel_seen();
        acct.worker->move_message(acct.current_folder, seq, dest);
    }
    void on_moved(const std::string &account_id, const std::string &folder, int seq,
                 const std::string &dest) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        acct.move_inflight = false;
        uint32_t moved_uid = uid_for_seq(acct, folder, seq);
        if (is_unseen(acct, folder, seq, moved_uid)) {
            bump_folder_unseen(acct, account_id, folder, -1);
            bump_folder_unseen(acct, account_id, dest, +1);
        }
        auto remove_from_vec = [&](std::vector<MailSummary> &vec){
            if (moved_uid) {
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [&](const MailSummary &s){ return s.uid == moved_uid; }), vec.end());
            } else {
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [&](const MailSummary &s){ return s.seq == seq; }), vec.end());
            }
        };
        const bool viewing_source =
            account_id == m_current_account_id &&
            folder == acct.wanted_folder && folder == acct.current_folder;
        if (viewing_source) {
            remove_from_vec(m_summaries);
            // MOVE/EXPUNGE always renumbers IMAP sequence numbers, even
            // when UIDs are stable. Skipping this left FETCH seq pointing
            // past EXISTS ("NO No messages matched") for later rows.
            if (seq > 0) for (auto &s : m_summaries) if (s.seq > seq) --s.seq;
        }
        auto it = acct.summary_cache.find(folder);
        if (it != acct.summary_cache.end()) {
            remove_from_vec(it->second);
            if (seq > 0) for (auto &s : it->second) if (s.seq > seq) --s.seq;
        }
        drop_expunged_bodies(acct, folder, seq, moved_uid);
        acct.local_expunge_folder = folder;
        acct.local_expunge_seq = seq;
        if (!viewing_source) {
            set_status("Moved to " + dest);
            update_move_buttons();
            redraw();
            return;
        }
        if (m_rendered_seq == seq) {
            m_has_message = false;
            m_rendered_seq = -1;
            m_reply_btn->set_enabled(false);
            if (m_fwd_btn) m_fwd_btn->set_enabled(false);
            if (m_save_btn) m_save_btn->set_enabled(false);
            m_images_btn->set_enabled(m_show_remote_images);
            Document doc;
            parse_markdown(doc, "*Message moved to " + dest + "*", text_color(), 18.f);
            m_view->set_document(std::move(doc));
            m_view_scroll->set_scroll(0.0f);
        } else if (seq > 0 && m_rendered_seq > seq) {
            --m_rendered_seq;
        }
        if (m_loading_seq == seq) m_loading_seq = -1;
        else if (seq > 0 && m_loading_seq > seq) --m_loading_seq;
        if (m_pending_seq == seq) { m_pending_seq = -1; m_preview_settle_at = 0; }
        else if (seq > 0 && m_pending_seq > seq) { --m_pending_seq; --m_pending_email.seq; }
        bool removed = false;
        if (m_email_list) {
            if (moved_uid) removed = m_email_list->remove_by_uid(moved_uid);
            if (!removed) removed = m_email_list->remove_seq(seq);
        }
        if (removed && m_email_list) {
            const EmailData* nd = m_email_list->selected_data();
            if (nd) {
                int nidx = m_email_list->selected_index();
                on_email_selected(nidx, *nd);
            }
        }
        if (m_email_list && m_email_list->emails().empty()) {
            m_has_message = false;
            m_rendered_seq = -1;
            m_pending_seq = -1;
            Document doc;
            parse_markdown(doc, "*No messages*", text_color(), 18.f);
            m_view->set_document(std::move(doc));
        }
        set_status("Moved to " + dest);
        update_move_buttons();
        redraw();
    }

    /* Wire one account's worker callbacks (each closes over its own account
     * id, which is how every handler below learns which account a given
     * IMAP event came from without MailWorker itself knowing about
     * multi-account at all). */
    void wire_worker(AccountSession &acct) {
        const std::string id = acct.config.id();
        MailWorker &w = *acct.worker;
        w.cb_folders = [this, id](const std::vector<MailFolder> &folders) {
            on_folders(id, folders);
        };
        w.cb_summaries = [this, id](const std::string &folder,
                                    const std::vector<MailSummary> &sums) {
            on_summaries(id, folder, sums);
        };
        w.cb_auto_summaries = [this, id](const std::string &folder,
                                         const std::vector<MailSummary> &sums) {
            on_auto_summaries(id, folder, sums);
        };
        w.cb_older = [this, id](const std::string &folder,
                                const std::vector<MailSummary> &sums) {
            on_older(id, folder, sums);
        };
        w.cb_body = [this, id](const std::string &folder, int seq,
                               const MailMessage &msg) {
            on_body(id, folder, seq, msg);
        };
        w.cb_prefetched = [this, id](const std::string &folder, int seq,
                                     const MailMessage &msg,
                                     const std::string &preview) {
            on_prefetched(id, folder, seq, msg, preview);
        };
        w.cb_error = [this, id](const std::string &title, const std::string &msg) {
            on_worker_error(id, title, msg);
        };
        w.cb_status = [this, id](const std::string &msg, const std::string &folder) {
            /* A late "Opening Trash..." must not overwrite Inbox after a
             * folder click.  While a folder is loading, ignore untagged
             * status (Connected/Ready) as well. */
            AccountSession *acct = account(id);
            if (!acct) return;
            if (!folder.empty() && folder != acct->wanted_folder) return;
            if (acct->folder_loading && folder.empty()) return;
            std::string out = m_accounts.size() > 1
                ? acct->config.display_name() + ": " + msg : msg;
            if (acct->worker->is_compressed() && msg.rfind("Connected",0)==0)
                out += " · DEFLATE";
            set_status(out);
            update_compress_badge();
        };
        w.cb_progress = [this, id](const std::string &folder, int done, int total) {
            /* The shared load bar/progress text stays tied to whichever
             * account is on screen -- a background account's fetch
             * progress would otherwise make the bar flicker between
             * unrelated accounts. */
            AccountSession *acct = account(id);
            if (!acct || id != m_current_account_id) return;
            if (folder != acct->wanted_folder || !acct->folder_loading) return;
            if (m_load_bar && total > 0)
                m_load_bar->set_progress((float)done / (float)total);
            std::string prefix = m_accounts.size() > 1
                ? acct->config.display_name() + ": " : "";
            set_status(prefix + folder + ": " + std::to_string(done) + " / " +
                       std::to_string(total) + compressSuffix());
            redraw();
        };
        w.cb_seen = [this, id](const std::string &folder, int seq) {
            on_seen(id, folder, seq);
        };
        w.cb_flag_seen = [this, id](const std::string &folder, int seq,
                                    bool seen) {
            on_remote_seen(id, folder, seq, seen);
        };
        w.cb_expunged = [this, id](const std::string &folder, int seq) {
            on_expunged(id, folder, seq);
        };
        w.cb_moved = [this, id](const std::string &folder, int seq,
                                const std::string &dest) {
            on_moved(id, folder, seq, dest);
        };
    }

    /* Create the runtime session for one configured account: sidebar
     * section, worker thread, callbacks -- then start connecting. Used both
     * at startup (one call per saved account) and when Preferences adds a
     * new account. */
    AccountSession &add_account_session(const MailAccount &ma) {
        auto session = std::make_unique<AccountSession>();
        session->config = ma;
        AccountSession &acct = *session;
        m_accounts.push_back(std::move(session));
        m_folder_view->add_account(ma.id(), ma.display_name());
        wire_worker(acct);
        acct.worker->set_config(ma);
        acct.worker->set_check_interval_min(m_config.check_interval_min);
        acct.worker->start();
        acct.worker->connect();
        return acct;
    }

    /* Tear down one account's session: stop its worker thread and drop its
     * sidebar section (Preferences "remove account"). */
    void remove_account_session(const std::string &id) {
        AccountSession *acct = account(id);
        if (!acct) return;
        acct->worker->stop();
        m_folder_view->remove_account(id);
        if (m_current_account_id == id) m_current_account_id.clear();
        m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
            [&](const std::unique_ptr<AccountSession> &a) { return a->config.id() == id; }),
            m_accounts.end());
    }

    MailApp() : Screen(Vector2i(1100, 700), "nmail") {
        inc_ref();
        set_window_icon(glfw_window());
        // Theme — light background like macOS Mail (toggle with Ctrl/Cmd+T)
        set_theme_mode(ThemeMode::Light);
        m_theme->m_split_divider_width = 2;

        auto *root_flex = new FlexLayout(FlexDirection::Column,
                                         JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        RootWindow *window = new RootWindow(this, root_flex);

        // ---- Toolbar ----
        // A macOS-Mail-style unified toolbar: a flat glass strip holding
        // rounded "pill" clusters of related buttons instead of one long
        // row of identical icons. See nmail_toolbar.h.
        MailToolbar *toolbar = new MailToolbar(window);

        MailToolbarGroup *refresh_group = toolbar->add_group();
        Button *refresh_btn = refresh_group->add_button(FA_SYNC, "Refresh");
        refresh_btn->set_callback([this]() { do_refresh(); });

        MailToolbarGroup *compose_group = toolbar->add_group();
        m_compose_btn = compose_group->add_button(FA_PEN, "Compose a new message");
        m_compose_btn->set_callback([this]() { show_compose(false); });

        MailToolbarGroup *reply_group = toolbar->add_group();
        m_reply_btn = reply_group->add_button(FA_REPLY, "Reply to this message");
        m_reply_btn->set_enabled(false);
        m_reply_btn->set_callback([this]() { show_compose(true); });

        m_fwd_btn = reply_group->add_button(FA_SHARE, "Forward this message");
        m_fwd_btn->set_enabled(false);
        m_fwd_btn->set_callback([this]() { show_compose(false, true); });

        MailToolbarGroup *message_group = toolbar->add_group();
        m_save_btn = message_group->add_button(FA_SAVE,
            "Save this email as an .eml file (Ctrl+S)");
        m_save_btn->set_enabled(false);
        m_save_btn->set_callback([this]() { save_current_email(); });

        m_images_btn = message_group->add_button(FA_IMAGE,
            "Load remote images (off by default to block tracking pixels)");
        m_images_btn->set_flags(Button::ToggleButton);
        m_images_btn->set_enabled(false);
        /* change_callback, not callback: a ToggleButton only fires
         * callback() on the click that pushes it IN — the un-push click
         * would never reach us. */
        m_images_btn->set_change_callback([this](bool on) {
            m_show_remote_images = on;
            /* The transparent toolbar buttons have no visible toggle state,
             * so light up a solid green pill while loading is on (alpha 0
             * falls back to the normal transparent look when off). */
            m_images_btn->set_background_color(on ? Color(40, 160, 60, 255)
                                                  : Color(0, 0, 0, 0));
            m_images_btn->set_tooltip(on
                ? "Remote images on -- click to stop loading (cached stay)"
                : "Load remote images (off by default to block tracking pixels)");
            if (on) {
                /* Do not re-parse the HTML: bind_loaded_images() re-runs the
                 * resolver, which queues HTTP GETs and leaves placeholders
                 * until each texture arrives. */
                if (m_view)
                    m_view->bind_loaded_images();
                update_image_status();
            } else {
                /* Stop future loading: drop queued fetches that have not
                 * started.  In-flight ones still land in the cache, and
                 * everything already cached keeps showing — resolve_image()
                 * consults m_img_tex before the m_show_remote_images gate. */
                for (const std::string &u : m_fetch_queue)
                    m_remote_pending.erase(u);
                m_fetch_queue.clear();
            }
            redraw();
        });

        MailToolbarGroup *move_group = toolbar->add_group();
        m_trash_btn = move_group->add_button(FA_TRASH, "Move to Trash (Delete)");
        m_trash_btn->set_enabled(false);
        m_trash_btn->set_callback([this]() { move_selected_to("Trash"); });

        m_junk_btn = move_group->add_button(FA_BROOM, "Move to Junk / Spam");
        m_junk_btn->set_enabled(false);
        m_junk_btn->set_callback([this]() { move_selected_to("Junk"); });

        /* Shown in place of trash/junk while viewing Trash or Junk. */
        m_restore_btn = move_group->add_button(FA_ARROW_UP, "Move back to Inbox");
        m_restore_btn->set_enabled(false);
        m_restore_btn->set_visible(false);
        m_restore_btn->set_callback([this]() { move_selected_to("Inbox"); });

        MailToolbarGroup *settings_group = toolbar->add_group();
        Button *prefs_btn = settings_group->add_button(FA_COG, "Preferences");
        prefs_btn->set_callback([this]() { show_preferences(); });

        m_theme_btn = settings_group->add_button(FA_MOON, "Toggle dark mode (Ctrl+T)");
        m_theme_btn->set_callback([this]() {
            apply_theme_mode(m_dark ? ThemeMode::Light : ThemeMode::Dark);
        });

        // Flexible gap pushes the search box to the right edge of the bar.
        toolbar->add_flex_spacer();

        // Search box — press Enter to filter the message list
        TextBox *search_box = new TextBox(toolbar);
        search_box->set_min_width(200);
        search_box->set_placeholder("Search sender or subject...");
        search_box->set_callback([this](const std::string &value) {
            m_filter = value;
            apply_filter(true);
            return true;
        });

        // ---- Horizontal split: sidebar | content ----
        Split *split = new Split(window, Split::Orientation::Horizontal);
        split->set_min_size(100);
        split->set_max_size({2048, 2048});
        split->set_keep_size_on_resize(true);
        root_flex->set_flex_item(split, FlexLayout::FlexItem(1.0f, 1.0f, 0));

        // ---- Left: FolderView sidebar ----
        m_folder_view = new FolderView(split, [this](const std::string &account_id, FolderItem *item) {
            on_folder_selected(account_id, item);
        });
        m_folder_view->set_min_width(250);

        // ---- Right side: inner Split  (email list | message pane) ----
        Split *inner_split = new Split(split, Split::Orientation::Horizontal);
        inner_split->set_min_size(100);
        inner_split->set_max_size({2048, 2048});
        inner_split->set_keep_size_on_resize(true);

        // ---- Middle: filter bar + email list ----
        Widget *list_pane = new Widget(inner_split);
        list_pane->set_min_width(280);
        auto *list_flex = new FlexLayout(FlexDirection::Column,
                                         JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        list_pane->set_layout(list_flex);

        MailToolbar *list_bar = new MailToolbar(list_pane, 36);
        list_bar->set_gap(6);
        list_bar->set_padding(8, 0);
        list_bar->set_height_flex(SizeMode::Fixed);

        auto compact = [](Button *b) {
            b->set_font_size(16);
            b->set_fixed_size(Vector2i(28, 28));
        };

        MailToolbarGroup *filter_group = list_bar->add_group();
        m_sort_date_btn = filter_group->add_button(FA_CLOCK, "Sort by date");
        compact(m_sort_date_btn);
        m_sort_date_btn->set_flags(Button::ToggleButton);
        m_sort_date_btn->set_pushed(true);
        m_sort_date_btn->set_change_callback([this](bool on) {
            if (!on) { m_sort_date_btn->set_pushed(true); return; }
            m_sort_by_size = false;
            m_filter_attach = false;
            m_sort_attach_btn->set_pushed(false);
            sync_list_filter_bar();
            apply_filter(true);
            redraw();
        });

        m_sort_attach_btn = filter_group->add_button(FA_PAPERCLIP,
            "Attachments only, sorted by size");
        compact(m_sort_attach_btn);
        m_sort_attach_btn->set_flags(Button::ToggleButton);
        m_sort_attach_btn->set_change_callback([this](bool on) {
            if (!on) { m_sort_attach_btn->set_pushed(true); return; }
            m_sort_by_size = true;
            m_filter_attach = true;
            m_sort_date_btn->set_pushed(false);
            sync_list_filter_bar();
            apply_filter(true);
            redraw();
        });

        m_sort_unread_btn = filter_group->add_button(FA_ENVELOPE, "Show unread only");
        compact(m_sort_unread_btn);
        m_sort_unread_btn->set_flags(Button::ToggleButton);
        m_sort_unread_btn->set_change_callback([this](bool on) {
            m_filter_unread = on;
            sync_list_filter_bar();
            apply_filter(true);
            redraw();
        });

        m_sort_vip_btn = filter_group->add_button(FA_FLAG,
            "VIP contacts (coming soon)");
        compact(m_sort_vip_btn);
        m_sort_vip_btn->set_enabled(false);

        list_bar->add_flex_spacer();

        MailToolbarGroup *dir_group = list_bar->add_group();
        m_sort_dir_btn = dir_group->add_button(FA_ARROW_DOWN, "Newest first");
        compact(m_sort_dir_btn);
        m_sort_dir_btn->set_callback([this]() {
            m_sort_desc = !m_sort_desc;
            sync_list_filter_bar();
            apply_filter(true);
            redraw();
        });
        sync_list_filter_bar();

        m_email_list = new EmailListView(list_pane,
            [this](int idx, const EmailData &d) { on_email_selected(idx, d); });
        m_email_list->set_height_flex(SizeMode::Expanding);
        list_flex->set_flex_item(m_email_list, FlexLayout::FlexItem(1.0f, 1.0f, 0));
        m_email_list->set_font_size(26);
        m_email_list->set_on_hit_bottom([this]() { maybe_fetch_older(); });
        m_email_list->on_viewport_changed = [this]() {
            AccountSession &acct = current_acct();
            if (acct.current_folder.empty() || !m_email_list) return;
            static double last = 0; double now = glfwGetTime();
            if (now - last < 0.15 && m_email_list->visible_seqs().size() < 30) return;
            last = now;
            auto rows = m_email_list->emails();
            auto vr = m_email_list->visible_range();
            int a = std::max(0, vr.first - 6);
            int b = std::min((int)rows.size(), vr.second + 6);
            std::vector<int> need_seq; need_seq.reserve(b-a);
            std::vector<uint32_t> need_uid; need_uid.reserve(b-a);
            for (int i=a;i<b;++i) {
                const EmailData &d = rows[i];
                if (body_has(acct.current_folder, d)) continue;
                if (d.uid) need_uid.push_back(d.uid);
                else if (d.seq) need_seq.push_back(d.seq);
            }
            if (!need_uid.empty()) acct.worker->ensure_visible_cached_uid(acct.current_folder, need_uid);
            if (!need_seq.empty()) acct.worker->ensure_visible_cached(acct.current_folder, need_seq);
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

        m_view_scroll = new ZoomScrollPanel(right, ZoomScrollPanel::ScrollTypes::Both);
        m_view_scroll->set_reflow_on_zoom(false);
        m_view_scroll->set_zoom_range(0.5, 3.0);
        m_view_scroll->set_zoom_enabled(true);
        m_view = new HtmlDocument(m_view_scroll);
        m_view->image_resolver = [this](const std::string &src) {
            return resolve_image(src);
        };
        m_view->on_link_click = [this](const std::string &url) -> bool {
            const size_t n = std::strlen(kAddrScheme);
            if (url.compare(0, n, kAddrScheme) != 0) return false;
            toggle_expanded_addr(url.substr(n));
            return true;                     // handled: do not open a browser
        };
        m_view->on_link_hover = [this](const std::string &url) {
            handle_link_hover(url);
        };
        m_view->embed_widget = [this](Widget *parent, const HtmlEmbedSpec &spec)
                -> Widget * {
            if (spec.id == "nmail-att-preview-bar")
                return make_att_preview_bar(parent);
            if (spec.id == "nmail-att-gallery")
                return make_gallery_strip(parent);
            if (spec.id != "nmail-attachments" || m_att_preview)
                return nullptr;
            return make_attachment_strip(parent);
        };
        style_editor();
        m_view_scroll->set_height_flex(SizeMode::Expanding);
        rflex->set_flex_item(m_view_scroll, FlexLayout::FlexItem(1.0f));

        // ---- Status bar (left) + window taskbar (right) ----
        Widget *statusbar = new Widget(window);
        statusbar->set_min_height(26);
        statusbar->set_height(26);
        statusbar->set_height_flex(SizeMode::Fixed);
        statusbar->set_layout(new FlexLayout(FlexDirection::Row,
                                             JustifyContent::SpaceBetween,
                                             AlignItems::Center, 0, 6));
        Widget *status_left = new Widget(statusbar);
        status_left->set_layout(new BoxLayout(Orientation::Horizontal,
                                              Alignment::Middle, 0, 8));
        m_status = new Label(status_left, "Not connected", "sans", 16);
        m_load_bar = new IndeterminateBar(status_left);
        m_load_bar->set_fixed_size(Vector2i(180, 8));
        m_load_bar->set_visible(false);
        m_compress_label = new Label(status_left, "", "sans", 13);
        m_compress_label->set_color(Color(40, 160, 60, 255));
        m_compress_label->set_visible(false);
        m_taskbar = new Widget(statusbar);
        m_taskbar->set_layout(new BoxLayout(Orientation::Horizontal,
                                            Alignment::Middle, 0, 4));
        m_status_base = "Not connected";

        split->set_drag_position(0.22f);
        inner_split->set_drag_position(0.38f);

        show_welcome();

        perform_layout();

        // ---- Load saved accounts, or ask for one ----
        if (load_config(m_config)) {
            m_config_loaded = true;
            for (const MailAccount &ma : m_config.accounts)
                add_account_session(ma);
        } else {
            show_preferences();
        }
        /* Only touch contacts.json when the user has opted in. */
        if (m_config.save_contacts)
            m_contacts.load(contacts_path());
        apply_theme_mode(m_config.dark_mode ? ThemeMode::Dark
                                            : ThemeMode::Light);
    }

    virtual ~MailApp() override {
        *m_alive = false;
        clear_image_textures();
        cleanup_att_temps();
        for (auto &a : m_accounts) a->worker->stop();
        if (m_config.save_contacts && m_contacts.dirty())
            m_contacts.save(contacts_path());
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

    void on_folders(const std::string &account_id, const std::vector<MailFolder> &folders) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        acct.folders = folders;
        acct.move_inflight = false;
        /* Fresh connection / explicit refresh: drop all cached state. */
        acct.summary_cache.clear();
        acct.body_cache.clear();
        update_move_buttons();
        update_compress_badge();

        std::string label = acct.config.display_name();
        std::string highlight = !acct.wanted_folder.empty() ? acct.wanted_folder
                                                             : acct.current_folder;

        // Auto-open an INBOX only for the very first account to connect
        // app-wide (nothing shown yet). Accounts that connect afterwards
        // just populate their own sidebar section without stealing focus
        // from whatever the user is already looking at.
        if (highlight.empty() && m_current_account_id.empty()) {
            for (const auto &f : folders) {
                std::string lower = f.name;
                for (char &c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower == "inbox") {
                    highlight = f.name;
                    acct.wanted_folder = f.name;
                    m_current_account_id = account_id;
                    set_folder_busy(true, "Opening " + f.name + "...");
                    m_folder_view->update_account(account_id, label, folders, highlight);
                    acct.worker->select_folder(f.name);
                    return;
                }
            }
        }

        m_folder_view->update_account(account_id, label, folders, highlight);
    }

    void on_summaries(const std::string &account_id, const std::string &folder,
                      const std::vector<MailSummary> &sums) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        const bool viewing_this_account = (account_id == m_current_account_id);
        mail_dbg("[mail] UI on_summaries folder='%s' wanted='%s' n=%zu loading=%d\n",
                folder.c_str(), acct.wanted_folder.c_str(), sums.size(),
                (int)acct.folder_loading);
        harvest(sums);
        if (folder != acct.wanted_folder) {
            /* Stale: INBOX headers arriving after a Trash click.  Keep the
             * cache for that folder unless the payload is empty and we
             * already have a better list.
             * For QRESYNC (Option A): MailWorker already patched
             * m_summaries_cache[folder] by removing vanished UIDs before
             * delivering cb_summaries — but MailApp's per-account
             * summary_cache is separate. So even stale deliveries honor the
             * patched list (vanished removed) when empty check would
             * otherwise mask it. The empty guard here only applies when we
             * already have a non-empty cache and the delivery is literally
             * empty (not a patched smaller list).
             */
            if (!sums.empty() || acct.summary_cache.find(folder) == acct.summary_cache.end())
                acct.summary_cache[folder] = sums;
            // Body cache for stale folder: evict vanished UIDs defensively
            {
                auto itc = acct.summary_cache.find(folder);
                if (itc != acct.summary_cache.end() && !itc->second.empty() && !sums.empty()) {
                    std::unordered_set<uint32_t> new_uids;
                    new_uids.reserve(sums.size());
                    bool any = false;
                    for (auto &s : sums) if (s.uid) { new_uids.insert(s.uid); any = true; }
                    if (any) {
                        for (auto &s : itc->second) {
                            if (s.uid && new_uids.find(s.uid)==new_uids.end()) {
                                acct.body_cache.erase(body_key(folder, s.uid, s.seq));
                                if (s.seq) acct.body_cache.erase(folder + ":" + std::to_string(s.seq));
                            }
                        }
                    }
                }
            }
            return;
        }

        /* A SELECT that missed EXISTS used to replace a good cached list
         * with nothing ("flash then clear").  Keep what we have and say so.
         * BUT: if MailWorker delivered a QRESYNC-patched list (vanished UIDs
         * removed), do not mask it — honor the smaller list and evict body
         * cache entries for the vanished UIDs (MailWorker already stripped
         * m_summaries_cache[folder]; MailApp must follow through on the
         * account's body_cache and selection).  When uid==0 (no
         * CONDSTORE/QRESYNC) this degenerates to the old behaviour.
         * Only meaningful while this account is the one on screen -- m_summaries
         * belongs to whichever account is currently viewed. */
        if (viewing_this_account && sums.empty() && !m_summaries.empty() &&
            acct.current_folder == folder) {
            // Detect genuine vanished vs. empty-fetch: if the cache entry
            // was intentionally replaced with a smaller list by QRESYNC delta,
            // sums would not be what we would cache — but here sums IS empty so
            // it's not a QRESYNC delta delivery. Keep cached.
            set_folder_busy(false, folder + ": keeping " +
                std::to_string(m_summaries.size()) +
                " cached (server sent none)" + compressSuffix());
            update_compress_badge();
            return;
        }
        // Vanished handling (Option B): evict Body cache for UIDs that disappeared
        // between the previous list and this delivery.  MailWorker already patched
        // m_summaries_cache[folder] by removing vanished UIDs before delivering
        // cb_summaries, but the account's body_cache still has stale UID keys. Also
        // covers direct server expunge without QRESYNC (seq-shift) when m_summaries
        // (this account's visible list) is not empty.
        if ((viewing_this_account && !m_summaries.empty()) ||
            acct.summary_cache.find(folder) != acct.summary_cache.end()) {
            const std::vector<MailSummary> *prev_ptr = nullptr;
            if (viewing_this_account && !m_summaries.empty() && acct.current_folder == folder)
                prev_ptr = &m_summaries;
            else {
                auto itc = acct.summary_cache.find(folder);
                if (itc != acct.summary_cache.end()) prev_ptr = &itc->second;
            }
            if (prev_ptr) {
                const auto &prev = *prev_ptr;
                std::unordered_set<uint32_t> new_uids;
                new_uids.reserve(sums.size());
                std::unordered_set<int> new_seqs;
                new_seqs.reserve(sums.size());
                bool any_new_uid = false;
                for (auto &s : sums) { if (s.uid) { new_uids.insert(s.uid); any_new_uid = true; } new_seqs.insert(s.seq); }
                std::vector<std::string> body_evict_keys;
                for (auto &s : prev) {
                    bool gone = false;
                    if (s.uid && any_new_uid) gone = (new_uids.find(s.uid) == new_uids.end());
                    else if (!s.uid || !any_new_uid) gone = (new_seqs.find(s.seq) == new_seqs.end());
                    else gone = (new_uids.find(s.uid) == new_uids.end());
                    if (gone) {
                        body_evict_keys.push_back(body_key(folder, s.uid, s.seq));
                        // also evict legacy ":" key if present
                        if (s.seq) body_evict_keys.push_back(folder + ":" + std::to_string(s.seq));
                    }
                }
                for (auto &k : body_evict_keys) acct.body_cache.erase(k);
            }
        }
        acct.summary_cache[folder] = sums;

        if (!viewing_this_account) {
            // Background account: cache is up to date, but don't touch the
            // shared UI (m_summaries/m_email_list/m_view) -- the user isn't
            // looking at this account right now.
            return;
        }

        acct.current_folder = folder;
        m_summaries      = sums;
        acct.older_inflight = false;
        acct.move_inflight = false;
        m_email_list->set_loading_more(false);
        apply_filter();
        update_move_buttons();
        set_folder_busy(false,
            folder + ": " + std::to_string(sums.size()) +
            (sums.size() == 1 ? " message" : " messages") + compressSuffix());
        update_compress_badge();
        // after layout, kick viewport prefetch so rows actually on screen win
        redraw();
        nanogui::async([this, account_id, folder]() {
            AccountSession *acct = account(account_id);
            if (!acct || account_id != m_current_account_id ||
                folder != acct->wanted_folder || folder != acct->current_folder ||
                !m_email_list)
                return;
            // UID-aware need check: prefer UID key when present, route via UID prefetch
            auto rows = m_email_list->emails();
            auto vr = m_email_list->visible_range();
            int a = std::max(0, vr.first - 6);
            int b = std::min((int)rows.size(), vr.second + 6);
            std::vector<int> need_seq; need_seq.reserve(b-a);
            std::vector<uint32_t> need_uid; need_uid.reserve(b-a);
            for (int i=a;i<b;++i) {
                const EmailData &d = rows[i];
                if (body_has(*acct, folder, d.uid, d.seq)) continue;
                if (d.uid) need_uid.push_back(d.uid);
                else if (d.seq) need_seq.push_back(d.seq);
            }
            if (!need_uid.empty()) acct->worker->ensure_visible_cached_uid(folder, need_uid);
            if (!need_seq.empty()) acct->worker->ensure_visible_cached(folder, need_seq);
        });
        glfwPostEmptyEvent();
    }

    /* Periodic background check (see MailWorker::Type::AutoRefresh):
       splice newly-arrived mail in at the top instead of replacing the
       list, so the user's scroll position and selection are undisturbed.
       Announces the count on the status bar rather than jumping the view
       to show it. */
    void on_auto_summaries(const std::string &account_id, const std::string &folder,
                           const std::vector<MailSummary> &sums) {
        if (sums.empty()) return;
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        const bool viewing_this_account = (account_id == m_current_account_id);
        // Merge into the cache for *this* folder.  Never splice into
        // m_summaries unless the user is still looking at this account and
        // `folder` — otherwise an INBOX auto-check would pollute the Trash
        // list, or a background account's check would pollute whichever
        // account is actually on screen.
        auto merge_fresh = [](std::vector<MailSummary> &dst,
                              const std::vector<MailSummary> &incoming) {
            std::unordered_set<int> known_seq;
            std::unordered_set<uint32_t> known_uid;
            known_seq.reserve(dst.size());
            known_uid.reserve(dst.size());
            for (const MailSummary &s : dst) { known_seq.insert(s.seq); if (s.uid) known_uid.insert(s.uid); }
            std::vector<MailSummary> fresh;
            fresh.reserve(incoming.size());
            for (const MailSummary &s : incoming) {
                if (s.uid && known_uid.count(s.uid)) continue;
                if (!s.uid && known_seq.count(s.seq)) continue;
                // if uid present but seq zero (QRESYNC) treat uid as authoritative
                fresh.push_back(s);
            }
            if (!fresh.empty())
                dst.insert(dst.begin(), fresh.begin(), fresh.end());
            return fresh;
        };

        auto apply_new_unseen = [&](const std::vector<MailSummary> &fresh) {
            int n = 0;
            for (const MailSummary &s : fresh)
                if (!s.seen) ++n;
            if (n)
                bump_folder_unseen(acct, account_id, folder, n);
        };

        if (folder != acct.wanted_folder || !viewing_this_account) {
            auto &cache = acct.summary_cache[folder];
            auto fresh = merge_fresh(cache, sums);
            harvest(fresh);
            apply_new_unseen(fresh);
            redraw();
            return;
        }

        auto fresh = merge_fresh(m_summaries, sums);
        acct.summary_cache[folder] = m_summaries;
        harvest(fresh);
        apply_new_unseen(fresh);
        // Vanished on auto-refresh: MailWorker patched its per-folder cache but
        // MailApp may still hold stale body entries for vanished UIDs. When
        // on_auto_summaries is not used for QRESYNC delta (auto path fetches only
        // new messages), we still defensively handle stale body keys if the
        // background worker later delivers a QRESYNC-style delta via cb_auto_summaries
        // (or if another client expunged messages between auto checks).
        if (!fresh.empty()) update_compress_badge();
        if (fresh.empty()) return;
        if (folder != acct.current_folder) {   // not looking at this folder
            redraw();
            return;
        }

        /* Rebuild through the active filter and sort so new mail does not
         * always land at the top (oldest-first and size sort). */
        std::string needle = m_filter;
        for (char &c : needle) c = (char)std::tolower((unsigned char)c);
        int shown = 0;
        for (const MailSummary &s : fresh)
            if (summary_visible(s, needle)) ++shown;
        apply_filter(true);
        if (shown == 0) {
            update_compress_badge();
            redraw();
            glfwPostEmptyEvent();
            return;
        }
        set_status(std::to_string(shown) + " New email" +
                  (shown == 1 ? "" : "s") + compressSuffix());
        update_compress_badge();
        redraw();
        glfwPostEmptyEvent();
    }

    /* Ask the worker for the next older page when the list hits bottom. */
    void maybe_fetch_older() {
        AccountSession &acct = current_acct();
        if (acct.folder_loading || acct.older_inflight ||
            acct.wanted_folder.empty() || m_summaries.empty())
            return;
        if (acct.wanted_folder != acct.current_folder)
            return;
        int oldest = m_summaries.back().seq;   // list is newest-first
        if (oldest <= 1) return;               // already at the first message
        acct.older_inflight = true;
        m_email_list->set_loading_more(true);
        set_status("Loading older messages in " + acct.wanted_folder + "...");
        acct.worker->fetch_older(acct.wanted_folder);
    }

    void on_older(const std::string &account_id, const std::string &folder,
                  const std::vector<MailSummary> &sums) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        if (folder != acct.wanted_folder || folder != acct.current_folder) return;
        acct.older_inflight = false;
        if (account_id != m_current_account_id) return;   // not looking at this account
        m_email_list->set_loading_more(false);
        if (sums.empty()) return;

        m_summaries.insert(m_summaries.end(), sums.begin(), sums.end());
        acct.summary_cache[folder] = m_summaries;
        harvest(sums);
        // make newly paged-in older rows eligible for viewport prefetch too
        {
            std::vector<uint32_t> uids; uids.reserve(sums.size());
            std::vector<int> seqs; seqs.reserve(sums.size());
            for (auto &s : sums) { if (s.uid) uids.push_back(s.uid); else seqs.push_back(s.seq); }
            if (!uids.empty()) acct.worker->ensure_visible_cached_uid(folder, uids);
            if (!seqs.empty()) acct.worker->ensure_visible_cached(folder, seqs);
        }

        /* Older pages join the same filter and sort as the rows already shown. */
        apply_filter(true);
        set_status(folder + ": showing " +
                              std::to_string(m_summaries.size()) +
                              " messages" + compressSuffix());
        update_compress_badge();
        redraw();
    }

    void on_body(const std::string &account_id, const std::string &folder, int seq,
                const MailMessage &msg) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        const bool viewing_this_account = (account_id == m_current_account_id);
        harvest(msg);
        std::string key_folder = folder.empty() ? acct.wanted_folder : folder;
        // Resolve UID before preview patch so VANISHED/QRESYNC seq-0 still matches.
        uint32_t uid = uid_for_seq(acct, key_folder, seq);
        // Always enrich the preview + cache, even if this wasn't the
        // foreground fetch — background prefetches land here too when
        // the user happens to be looking at that message.
        std::string preview = message_preview(msg);
        if (!preview.empty() && viewing_this_account && key_folder == acct.current_folder) {
            for (auto &s : m_summaries) {
                if ((uid && s.uid == uid) || (!uid && s.seq == seq)) {
                    if (s.preview != preview) s.preview = preview;
                    break;
                }
            }
            auto it = acct.summary_cache.find(key_folder);
            if (it != acct.summary_cache.end())
                for (auto &s : it->second)
                    if ((uid && s.uid == uid) || (!uid && s.seq == seq)) {
                        if (s.preview != preview) s.preview = preview;
                        break;
                    }
            if (m_email_list) {
                if (uid) m_email_list->update_preview_by_uid(uid, preview);
                // dual-write: seq lookup still works for legacy path
                m_email_list->update_preview(seq, preview);
            }
        }
        if (!key_folder.empty())
            body_put(acct, key_folder, uid, seq, msg);
        if (!viewing_this_account) return;
        if (key_folder != acct.wanted_folder) return;
        if (seq != m_loading_seq) return;   // not the foreground fetch
        if (m_pending_seq >= 0 && seq != m_pending_seq)
            return;   // still scrubbing a different message
        m_loading_seq = -1;
        m_current_message = msg;
        m_has_message     = true;
        m_rendered_seq    = seq;
        m_expanded_addrs.clear();   // reveals belong to the message shown
        arm_read_timer(seq);
        m_reply_btn->set_enabled(true);
        if (m_fwd_btn) m_fwd_btn->set_enabled(true);
        if (m_save_btn) m_save_btn->set_enabled(true);
        render_current();
    }

    void on_prefetched(const std::string &account_id, const std::string &folder, int seq,
                       const MailMessage &msg, const std::string &preview) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        // Prefetch may arrive via UID path (seq==0 when seq stale); resolve uid via cache
        uint32_t uid = 0;
        if (seq) uid = uid_for_seq(acct, folder, seq);
        // If seq==0 (UID prefetch), try to harvest uid from body cache already? msg has no uid.
        // Body cache key will use uid if we can infer it; otherwise seq key.
        // For UID prefetch path we already popped uid queue; deliver carries resolved seq.
        // Still try uid resolution: if seq==0, scan summaries for any uid that matches body?
        // Instead prefer: if uid==0 and seq==0 we cannot key by UID — still store by seq 0 is noop.
        // Worker delivers seq_for_cb looked up in its summaries_cache, so seq should be non-zero
        // when UID had a known seq. Keep fallback to uid lookup via m_summaries.
        if (!uid && seq == 0) {
            // Attempt to find the message's uid by matching that this prefetch was the only one
            // outstanding — not reliable, just keep seq path. Body cache will store under seq.
        }
        body_put(acct, folder, uid, seq, msg);
        // Also ensure UID key exists when uid known but delivered seq doesn't give it:
        // if we resolved uid, dual-write already happened via body_put above.
        harvest(msg);
        if (account_id != m_current_account_id ||
            folder != acct.wanted_folder || folder != acct.current_folder) return;
        // only enrich empty/thin previews — never clobber a real one with
        // a shorter derived snippet from a failed decode edge case
        bool enriched = false;
        for (auto &s : m_summaries) {
            bool match = uid ? (s.uid == uid) : (s.seq == seq);
            if (!match) continue;
            if (preview.size() > s.preview.size()) { s.preview = preview; enriched = true; }
            break;
        }
        auto it = acct.summary_cache.find(folder);
        if (it != acct.summary_cache.end())
            for (auto &s : it->second) {
                bool match = uid ? (s.uid == uid) : (s.seq == seq);
                if (!match) continue;
                if (preview.size() > s.preview.size()) { s.preview = preview; break; }
            }
        if (enriched && m_email_list && preview.size() > 0) {
            if (uid) m_email_list->update_preview_by_uid(uid, preview);
            m_email_list->update_preview(seq, preview);
        }
    }

    void on_worker_error(const std::string &account_id, const std::string &title,
                        const std::string &msg) {
        AccountSession *acct = account(account_id);
        if (acct) { acct->older_inflight = false; acct->move_inflight = false; }
        if (account_id == m_current_account_id) {
            if (m_email_list) m_email_list->set_loading_more(false);
            set_folder_busy(false, title);
            update_move_buttons();
        }
        std::string shown_title = (acct && m_accounts.size() > 1)
            ? acct->config.display_name() + ": " + title : title;
        /* Auto-reconnect / periodic check can fail on every attempt.  One
         * dialog is enough; skip while the previous one is still open. */
        for (Widget *c : children())
            if (c->id() == "nmail-imap-error")
                return;
        auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                                      shown_title, msg, "OK", "", false);
        dlg->set_id("nmail-imap-error");
        dlg->center();
    }

    void on_folder_selected(const std::string &account_id, FolderItem *item) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        // The tooltip carries the full folder name (caption is the leaf).
        std::string folder = item->tooltip();
        if (folder.empty()) folder = item->caption();
        const bool switching_account = (account_id != m_current_account_id);
        /* Already opening this folder — ignore the duplicate click. */
        if (!switching_account && folder == acct.wanted_folder && acct.folder_loading) return;
        /* Already showing this folder with a populated list. */
        if (!switching_account && folder == acct.wanted_folder && folder == acct.current_folder &&
            !acct.folder_loading && !m_summaries.empty())
            return;
        /* A mark-read timer armed in the account/folder being left must not
         * fire here -- the message is no longer on screen (and seqs may
         * shift). Cancel on the outgoing account before repointing
         * m_current_account_id below. */
        current_acct().worker->cancel_seen();
        m_loading_seq  = -1;
        m_pending_seq  = -1;
        m_rendered_seq = -1;
        m_has_message  = false;
        acct.older_inflight = false;
        acct.move_inflight = false;
        m_email_list->set_loading_more(false);
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);

        /* Pin the wanted account+folder *before* any async IMAP callback can
         * land, so a late INBOX summary cannot hijack the Trash view -- or a
         * different account's delivery cannot hijack this one. */
        m_current_account_id = account_id;
        acct.wanted_folder  = folder;
        acct.current_folder = folder;

        /* Serve the last-known list instantly, then refresh from the
         * server in the background.  Do not clear the widget first —
         * that one-frame empty list was the "flash then clear". */
        auto cached = acct.summary_cache.find(folder);
        if (cached != acct.summary_cache.end()) {
            m_summaries = cached->second;
            apply_filter();
            set_folder_busy(true, folder + ": " +
                std::to_string(m_summaries.size()) +
                " cached, fetching latest..." + compressSuffix());
        } else {
            m_summaries.clear();
            m_email_list->set_emails({});
            Document doc;
            parse_markdown(doc, "*Loading " + folder + "...*", text_color(), 18.0f);
            m_view->set_document(std::move(doc));
            set_folder_busy(true, "Opening " + folder + "..." + compressSuffix());
        }
        update_compress_badge();
        update_move_buttons();
        redraw();
        acct.worker->select_folder(folder);
    }

    void on_email_selected(int idx, const EmailData &d) {
        update_move_buttons();
        /* List highlight already moved.  Do not parse HTML or FETCH on
         * every GLFW_REPEAT — wait until this seq sits still. */
        if (d.seq == m_pending_seq)
            return;
        if (d.seq == m_rendered_seq && m_pending_seq < 0)
            return;
        AccountSession &acct = current_acct();
        // speculatively prioritize neighbors of the selection — the user is
        // walking the list sequentially, so ±6 around idx are most likely next.
        if (m_email_list && idx >= 0) {
            std::vector<int> around_seq; around_seq.reserve(13);
            std::vector<uint32_t> around_uid; around_uid.reserve(13);
            auto &rows = m_email_list->emails();
            for (int i = std::max(0, idx-6); i <= std::min((int)rows.size()-1, idx+6); ++i) {
                const EmailData &r = rows[i];
                if (r.seq == d.seq && r.uid == d.uid) continue;
                if (body_has(acct, acct.current_folder, r.uid, r.seq)) continue;
                if (r.uid) around_uid.push_back(r.uid);
                else if (r.seq) around_seq.push_back(r.seq);
            }
            if (!around_uid.empty()) acct.worker->ensure_visible_cached_uid(acct.current_folder, around_uid);
            if (!around_seq.empty()) acct.worker->ensure_visible_cached(acct.current_folder, around_seq);
        }
        m_loading_seq = -1;   // drop in-flight body for a previous seq
        /* Cancel now rather than waiting for the preview to settle, so a
         * near-expired timer on the previous message cannot still fire. */
        if (d.seq != m_rendered_seq) acct.worker->cancel_seen();
        //const bool switched = (d.seq != m_rendered_seq);
        m_pending_seq       = d.seq;
        m_pending_email     = d;
        m_preview_settle_at = glfwGetTime() + kPreviewSettleSec;
        //if (switched)
        //    show_preview_stub(d);
        redraw();
    }

    /* Same parchment card as render_current() so the preview does not
     * jump when the full message arrives.  `loading` appends a subtle
     * "Loading…" row under the envelope snippet while FETCH is in flight. */
    void apply_preview_stub(const EmailData &d, bool loading) {
        MailMessage stub;
        stub.subject = d.subject.empty() ? "(no subject)" : d.subject;
        stub.from = d.sender;
        stub.date = d.date;
        std::string html = header_html(stub, m_expanded_addrs);
        if (!d.preview.empty())
            html += "<p>" + html_escape(d.preview) + "</p>";
        if (loading)
            html += "<p style=\"font-size:14px\"><em>Loading message\u2026</em></p>";
        m_view->set_html(html);
        m_has_message = false;
        m_att_preview = false;
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);
        m_view_scroll->set_scroll(0.0f);
    }

    void show_preview_stub(const EmailData &d) {
        apply_preview_stub(d, false);
    }

    void show_preview_stub_with_loading(const EmailData &d) {
        apply_preview_stub(d, true);
    }

    /* Start the read clock for the message now on screen.  Already-read mail
     * needs no STORE, and a message with no folder cannot be addressed. */
    void arm_read_timer(int seq) {
        AccountSession &acct = current_acct();
        if (seq <= 0 || acct.current_folder.empty()) { acct.worker->cancel_seen(); return; }
        uint32_t uid = 0;
        uint64_t modseq = 0;
        for (const MailSummary &s : m_summaries) if (s.seq == seq) {
            if (s.seen) { acct.worker->cancel_seen(); return; }
            uid = s.uid;
            modseq = s.modseq;
            break;
        }
        if (uid != 0)
            acct.worker->schedule_seen_uid(acct.current_folder, seq, uid, modseq, kMarkReadSec);
        else if (modseq != 0)
            acct.worker->schedule_seen(acct.current_folder, seq, modseq, kMarkReadSec);
        else
            acct.worker->schedule_seen(acct.current_folder, seq, kMarkReadSec);
    }

    bool is_unseen(const AccountSession &acct, const std::string &folder,
                   int seq, uint32_t uid) const {
        auto match = [&](const MailSummary &s) {
            return uid ? s.uid == uid : s.seq == seq;
        };
        auto it = acct.summary_cache.find(folder);
        if (it != acct.summary_cache.end())
            for (const MailSummary &s : it->second)
                if (match(s)) return !s.seen;
        if (folder == acct.current_folder)
            for (const MailSummary &s : m_summaries)
                if (match(s)) return !s.seen;
        return false;
    }

    void bump_folder_unseen(AccountSession &acct, const std::string &account_id,
                            const std::string &folder, int delta) {
        if (!delta) return;
        for (MailFolder &f : acct.folders) {
            if (f.name != folder) continue;
            f.unseen = std::max(0, f.unseen + delta);
            if (m_folder_view)
                m_folder_view->set_folder_unseen(account_id, folder, f.unseen);
            return;
        }
    }

    /* The server confirmed the flag: mirror it locally so the row stops
     * rendering as unread. */
    void on_seen(const std::string &account_id, const std::string &folder, int seq) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        bool already = false;
        auto mark = [seq, &already](std::vector<MailSummary> &v) {
            for (MailSummary &s : v)
                if (s.seq == seq) { already = already || s.seen; s.seen = true; break; }
        };
        auto it = acct.summary_cache.find(folder);
        if (it != acct.summary_cache.end()) mark(it->second);
        const bool viewing = account_id == m_current_account_id &&
            folder == acct.wanted_folder && folder == acct.current_folder;
        if (viewing) mark(m_summaries);
        if (!already) bump_folder_unseen(acct, account_id, folder, -1);
        if (!viewing) { redraw(); return; }
        if (m_filter_unread) {
            apply_filter(true);
            redraw();
            return;
        }
        // Try UID first (QRESYNC path where seq may be 0), then fallback to seq
        bool done = false;
        if (m_email_list) {
            uint32_t uid = uid_for_seq(acct, folder, seq);
            if (uid) done = m_email_list->set_seen_by_uid(uid, true);
            if (!done) done = m_email_list->set_seen(seq, true);
        }
        redraw();
    }

    /* IDLE push: another session (phone, webmail, ...) flipped a message's
     * \Seen flag.  Same bookkeeping as on_seen, but honors the pushed state
     * in both directions (read AND unread). */
    void on_remote_seen(const std::string &account_id, const std::string &folder,
                        int seq, bool seen) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        bool found = false, was = false;
        auto mark = [seq, seen, &found, &was](std::vector<MailSummary> &v) {
            for (MailSummary &s : v)
                if (s.seq == seq) {
                    if (!found) { found = true; was = s.seen; }
                    s.seen = seen;
                    break;
                }
        };
        auto it = acct.summary_cache.find(folder);
        if (it != acct.summary_cache.end()) mark(it->second);
        const bool viewing = account_id == m_current_account_id &&
            folder == acct.wanted_folder && folder == acct.current_folder;
        if (viewing) mark(m_summaries);
        if (found && was != seen)
            bump_folder_unseen(acct, account_id, folder, seen ? -1 : 1);
        if (!viewing) { redraw(); return; }
        if (m_filter_unread) {
            apply_filter(true);
            redraw();
            return;
        }
        bool done = false;
        if (m_email_list) {
            uint32_t uid = uid_for_seq(acct, folder, seq);
            if (uid) done = m_email_list->set_seen_by_uid(uid, seen);
            if (!done) done = m_email_list->set_seen(seq, seen);
        }
        redraw();
    }

    /* IDLE push: a message was expunged from the folder by another session.
     * Mirrors on_moved's removal path (uid-aware, seq renumber fallback). */
    void on_expunged(const std::string &account_id, const std::string &folder, int seq) {
        AccountSession *acct_ptr = account(account_id);
        if (!acct_ptr) return;
        AccountSession &acct = *acct_ptr;
        /* EXPUNGE echo of our own MOVE: on_moved already removed the row
         * and decremented higher seqs.  After that shift, `seq` now names
         * a different message — applying this event would delete it too. */
        if (folder == acct.local_expunge_folder && seq == acct.local_expunge_seq) {
            acct.local_expunge_folder.clear();
            acct.local_expunge_seq = 0;
            redraw();
            return;
        }
        uint32_t gone_uid = uid_for_seq(acct, folder, seq);
        if (is_unseen(acct, folder, seq, gone_uid))
            bump_folder_unseen(acct, account_id, folder, -1);
        /* A duplicate push (e.g. the EXPUNGE echo of our own MOVE arriving
         * after on_moved already cleaned up) must not renumber a second
         * time -- only adjust seqs when this event actually removed a row. */
        bool removed_any = false;
        auto remove_from_vec = [&](std::vector<MailSummary> &vec){
            size_t before = vec.size();
            if (gone_uid) {
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [&](const MailSummary &s){ return s.uid == gone_uid; }), vec.end());
            } else {
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [&](const MailSummary &s){ return s.seq == seq; }), vec.end());
            }
            if (vec.size() != before) {
                removed_any = true;
                if (seq > 0) for (auto &s : vec) if (s.seq > seq) --s.seq;
            }
        };
        auto it = acct.summary_cache.find(folder);
        if (it != acct.summary_cache.end())
            remove_from_vec(it->second);
        drop_expunged_bodies(acct, folder, seq, gone_uid);
        const bool viewing =
            account_id == m_current_account_id &&
            folder == acct.wanted_folder && folder == acct.current_folder;
        if (!viewing) {
            redraw();
            return;
        }
        remove_from_vec(m_summaries);
        bool removed = false;
        if (m_email_list) {
            if (gone_uid) removed = m_email_list->remove_by_uid(gone_uid);
            if (!removed) removed = m_email_list->remove_seq(seq);
        }
        if (!removed_any && !removed) {
            /* Stale/duplicate push: nothing was removed anywhere, so none of
             * the seq-dependent state may be adjusted. */
            redraw();
            return;
        }
        if (m_rendered_seq == seq) {
            m_has_message = false;
            m_rendered_seq = -1;
            m_reply_btn->set_enabled(false);
            if (m_fwd_btn) m_fwd_btn->set_enabled(false);
            if (m_save_btn) m_save_btn->set_enabled(false);
            Document doc;
            parse_markdown(doc, "*Message deleted*", text_color(), 18.f);
            m_view->set_document(std::move(doc));
            m_view_scroll->set_scroll(0.0f);
        } else if (seq > 0 && m_rendered_seq > seq) {
            --m_rendered_seq;
        }
        if (m_loading_seq == seq) m_loading_seq = -1;
        else if (seq > 0 && m_loading_seq > seq) --m_loading_seq;
        if (m_pending_seq == seq) { m_pending_seq = -1; m_preview_settle_at = 0; }
        else if (seq > 0 && m_pending_seq > seq) { --m_pending_seq; --m_pending_email.seq; }
        if (removed && m_email_list) {
            const EmailData* nd = m_email_list->selected_data();
            if (nd) {
                int nidx = m_email_list->selected_index();
                on_email_selected(nidx, *nd);
            }
        }
        if (m_email_list && m_email_list->emails().empty()) {
            m_has_message = false;
            m_rendered_seq = -1;
            m_pending_seq = -1;
            Document doc;
            parse_markdown(doc, "*No messages*", text_color(), 18.f);
            m_view->set_document(std::move(doc));
        }
        update_move_buttons();
        redraw();
    }

    void commit_pending_preview() {
        if (m_pending_seq < 0)
            return;
        AccountSession &acct = current_acct();
        const EmailData d = m_pending_email;
        const int seq = m_pending_seq;
        m_pending_seq = -1;
        auto cached = body_find(acct, acct.current_folder, d.uid, d.seq);
        if (cached != acct.body_cache.end()) {
            m_loading_seq     = -1;
            m_current_message = cached->second;
            m_has_message     = true;
            m_rendered_seq    = seq;
            m_expanded_addrs.clear();
            m_reply_btn->set_enabled(true);
            if (m_fwd_btn) m_fwd_btn->set_enabled(true);
            if (m_save_btn) m_save_btn->set_enabled(true);
            render_current();
            arm_read_timer(seq);
            return;
        }
        m_loading_seq = seq;
        show_preview_stub_with_loading(d);
        acct.worker->fetch_body(acct.current_folder, seq, d.uid);
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
        if (m_config.accounts.empty()) {
            show_preferences();
            return;
        }
        /* Explicit refresh means "forget everything I know" -- for the
         * account currently on screen. */
        AccountSession &acct = current_acct();
        acct.summary_cache.clear();
        acct.body_cache.clear();
        acct.worker->cancel_seen();   // seqs may renumber; don't flag a stranger
        if (!acct.wanted_folder.empty())
            set_folder_busy(true, "Refreshing " + acct.wanted_folder + "..." + compressSuffix());
        else
            set_folder_busy(true, "Refreshing folders..." + compressSuffix());
        acct.worker->refresh();
        update_compress_badge();
    }

    /* Active filter glyphs turn red. The arrow stays on the right and
     * only tracks direction. */
    void sync_list_filter_bar() {
        auto tint = [](Button *b, bool on) {
            if (!b) return;
            b->set_text_color(on ? Color(200, 40, 40, 255) : Color(0, 0, 0, 0));
        };
        tint(m_sort_date_btn, !m_sort_by_size);
        tint(m_sort_unread_btn, m_filter_unread);
        tint(m_sort_attach_btn, m_sort_by_size);
        if (!m_sort_dir_btn) return;
        m_sort_dir_btn->set_icon(m_sort_desc ? FA_ARROW_DOWN : FA_ARROW_UP);
        if (m_sort_by_size)
            m_sort_dir_btn->set_tooltip(m_sort_desc ? "Largest first" : "Smallest first");
        else
            m_sort_dir_btn->set_tooltip(m_sort_desc ? "Newest first" : "Oldest first");
    }

    EmailData summary_row(const MailSummary &s) const {
        EmailData d;
        d.seq            = s.seq;
        d.uid            = s.uid;
        d.modseq         = s.modseq;
        d.sender         = s.from;
        d.subject        = s.subject;
        d.preview        = s.preview;
        d.date           = s.date;
        d.date_utc       = s.date_utc;
        d.bytes          = s.bytes;
        d.has_attachment = s.has_attachment;
        d.seen           = s.seen;
        return d;
    }

    /* needle is already lowercased. Empty needle matches everything. */
    bool summary_visible(const MailSummary &s, const std::string &needle) const {
        if (!needle.empty()) {
            std::string hay = s.from + "\n" + s.subject;
            for (char &c : hay) c = (char)std::tolower((unsigned char)c);
            if (hay.find(needle) == std::string::npos) return false;
        }
        if (m_filter_unread && s.seen) return false;
        if (m_filter_attach && !s.has_attachment) return false;
        return true;
    }

    void sort_visible(std::vector<EmailData> &rows) const {
        const bool desc = m_sort_desc;
        const bool by_size = m_sort_by_size;
        std::stable_sort(rows.begin(), rows.end(),
            [&](const EmailData &a, const EmailData &b) {
                if (by_size) {
                    if (a.bytes != b.bytes)
                        return desc ? a.bytes > b.bytes : a.bytes < b.bytes;
                } else if (a.date_utc != b.date_utc && (a.date_utc || b.date_utc)) {
                    return desc ? a.date_utc > b.date_utc : a.date_utc < b.date_utc;
                }
                if (a.uid && b.uid && a.uid != b.uid)
                    return desc ? a.uid > b.uid : a.uid < b.uid;
                return desc ? a.seq > b.seq : a.seq < b.seq;
            });
    }

    /* Filter and sort the current folder's summaries into the list.
     * keep_place retains scroll and the selected message when it still
     * passes; folder changes pass false and jump back to the top. */
    void apply_filter(bool keep_place = false) {
        std::string needle = m_filter;
        for (char &c : needle) c = (char)std::tolower((unsigned char)c);

        std::vector<EmailData> rows;
        rows.reserve(m_summaries.size());
        for (const MailSummary &s : m_summaries)
            if (summary_visible(s, needle))
                rows.push_back(summary_row(s));
        sort_visible(rows);
        if (m_email_list)
            m_email_list->set_emails(std::move(rows), keep_place);
        update_move_buttons();
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

        /* Gallery: att:<index> is a file attachment, not a cid part. */
        if (src.rfind("att:", 0) == 0) {
            size_t n = (size_t)std::atoi(src.c_str() + 4);
            const auto &atts = m_current_message.attachments;
            if (n < atts.size() && image_bytes_decodable(atts[n].data))
                return make_image_info(create_image_texture(src, atts[n].data));
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
        set_status(buf);
    }

    /* (Re-)render the current message — on select, body arrival, theme
       change.  Image bytes bind in place via bind_loaded_images(). */
    /* Re-render the message with the current set of revealed addresses.
     * Cheap enough: this is the same path a message switch already takes. */
    void toggle_expanded_addr(const std::string &addr) {
        std::string low = addr;
        for (char &c : low) c = (char)std::tolower((unsigned char)c);
        if (!m_expanded_addrs.erase(low))
            m_expanded_addrs.insert(low);
        /* render_current() resets the scroll to the top; the user clicked a
         * name, they did not ask to be sent back to the start of the mail. */
        Vector2f keep = m_view_scroll ? m_view_scroll->scroll() : Vector2f(0.f, 0.f);
        render_current();
        if (m_view_scroll) m_view_scroll->set_scroll(keep);
    }

    void render_current() {
        if (!m_has_message) return;
        m_att_preview = false;
        m_gallery.clear();
        m_gallery_sel = -1;
        hide_att_popup();
        clear_image_textures();
        m_has_remote_images = false;
        m_doc_remotes.clear();
        const MailMessage &msg = m_current_message;
        /* HTML and plain/Markdown both go through HtmlDocument so the
         * parchment header card is the same chrome on every message. */
        std::string html = header_html(msg, m_expanded_addrs);
        html += msg.html.empty() ? body_as_html(msg) : msg.html;
        m_view->set_html(with_attachment_slots(html, msg));
        m_has_remote_images = m_view->has_remote_images();
        /* Enabled when this message has remote images, or whenever loading
         * is on so it can always be switched back off.  The pushed state
         * follows the global opt-in, not the message. */
        m_images_btn->set_pushed(m_show_remote_images);
        m_images_btn->set_enabled(m_has_remote_images || m_show_remote_images);
        m_view_scroll->set_scroll(0.0f);
        redraw();
    }

    Widget *make_attachment_strip(Widget *parent) {
        return make_attachment_strip(parent, m_current_message);
    }

    Widget *make_attachment_strip(Widget *parent, const MailMessage &msg) {
        auto vis = visible_attachments(msg);
        if (vis.empty() || !parent) return nullptr;
        auto *strip = new AttachmentStrip(parent);
        for (size_t i = 0; i < vis.size(); ++i) {
            const MailAttachment &a = *vis[i];
            size_t idx = (size_t)(vis[i] - msg.attachments.data());
            int thumb = 0;
            if (attachment_is_photo(a) && image_bytes_decodable(a.data)) {
                std::string key = "attthumb:" + std::to_string(idx) + ":" + a.filename;
                thumb = create_image_texture(key, a.data);
            }
            auto *chip = new AttachmentChip(strip, a, thumb);
            auto alive = m_alive;
            chip->on_open = [this, idx, alive] {
                nanogui::async([this, idx, alive] {
                    if (*alive) open_photo_at(idx);
                });
            };
            if (attachment_is_photo(a))
                chip->on_activate = chip->on_open;
            chip->on_save = [this, chip] { save_attachment(chip->attachment()); };
            chip->on_menu = [this, chip](const Vector2i &p) {
                show_attachment_menu(chip, p);
            };
        }
        return strip;
    }

    void hide_att_popup() {
        if (!m_att_popup) return;
        m_att_popup->set_visible(false);
        if (Screen *s = screen()) {
            /* Leave it parented; next right-click rebuilds the rows. */
            (void)s;
        }
    }

    Window *root_window() {
        for (Widget *c : children())
            if (auto *w = dynamic_cast<Window *>(c))
                if (w->is_root()) return w;
        return nullptr;
    }

    void show_attachment_menu(AttachmentChip *chip, const Vector2i &screen_pos) {
        if (!chip) return;
        Screen *s = screen();
        /* Always hang the menu off the root window. A floating compose
         * dialog as PopupMenu's parent_window makes click-to-close call
         * move_window_to_front() while Screen is still iterating children. */
        Window *w = root_window();
        if (!w) w = chip->window();
        if (!s || !w) return;
        if (m_att_popup && m_att_popup->parent_window() != w) {
            m_att_popup->set_visible(false);
            m_att_popup->dispose();
            m_att_popup = nullptr;
        }
        if (!m_att_popup)
            m_att_popup = new PopupMenu(s, w, nullptr, false);
        while (m_att_popup->child_count() > 0)
            m_att_popup->remove_child_at(m_att_popup->child_count() - 1);

        const MailAttachment att = chip->attachment();
        const bool can_view = attachment_is_inpane_preview(att);
        const bool can_open = can_view ||
            (!attachment_is_exec(att) && !attachment_is_archive(att) &&
             is_open_allowlisted(attachment_ext(att)));
        auto alive = m_alive;
        auto *open_item = new MenuItem(m_att_popup, "Open");
        open_item->set_enabled(can_open);
        open_item->set_callback([this, att, alive] {
            hide_att_popup();
            nanogui::async([this, att, alive] {
                if (*alive) open_attachment(att);
            });
        });
        auto *browser_item = new MenuItem(m_att_popup, "Open in browser");
        browser_item->set_enabled(attachment_opens_in_browser(att));
        browser_item->set_callback([this, att, alive] {
            hide_att_popup();
            nanogui::async([this, att, alive] {
                if (*alive) open_attachment_in_browser(att);
            });
        });
        auto *save_item = new MenuItem(m_att_popup, "Save As\u2026");
        save_item->set_callback([this, att, alive] {
            hide_att_popup();
            nanogui::async([this, att, alive] {
                if (*alive) save_attachment(att);
            });
        });
        auto *dl_item = new MenuItem(m_att_popup, "Save to Downloads");
        dl_item->set_enabled(!att.data.empty());
        dl_item->set_callback([this, att, alive] {
            hide_att_popup();
            nanogui::async([this, att, alive] {
                if (*alive) save_attachment_to_downloads(att);
            });
        });
        if (chip->on_remove) {
            auto rmfn = chip->on_remove;
            auto *rm = new MenuItem(m_att_popup, "Remove");
            rm->set_callback([this, rmfn, alive] {
                hide_att_popup();
                nanogui::async([rmfn, alive] {
                    if (*alive && rmfn) rmfn();
                });
            });
        }

        NVGcontext *ctx = s->nvg_context();
        Vector2i pref = m_att_popup->preferred_size(ctx);
        m_att_popup->set_size(pref);
        m_att_popup->perform_layout(ctx);
        Vector2i pos = visual_screen_pos(chip, screen_pos);
        pos.x() = std::min(pos.x(), std::max(0, s->width() - pref.x()));
        if (pos.y() + pref.y() > s->height())
            pos.y() = std::max(0, pos.y() - pref.y());
        m_att_popup->set_position(pos);
        m_att_popup->set_visible(true);
        s->set_popup_visible(m_att_popup);
        int first = 0;
        for (int i = 0; i < m_att_popup->child_count(); ++i) {
            if (auto *mi = m_att_popup->item(i))
                if (mi->visible() && mi->enabled()) { first = i; break; }
        }
        m_att_popup->set_highlighted_index(first);
        m_att_popup->request_focus();
        redraw();
    }

    void cleanup_att_temps() {
        for (const AttTemp &t : m_att_temps) {
#ifndef _WIN32
            ::unlink(t.file.c_str());
            if (!t.dir.empty()) ::rmdir(t.dir.c_str());
#else
            DeleteFileA(t.file.c_str());
            if (!t.dir.empty()) RemoveDirectoryA(t.dir.c_str());
#endif
        }
        m_att_temps.clear();
    }

    bool write_temp_attachment(const MailAttachment &att, std::string &path_out,
                               std::string &err) {
        const std::string ext = attachment_ext(att);
        const std::string name = sanitize_filename(att.filename, ext);
#ifdef _WIN32
        char tmp[MAX_PATH];
        if (!GetTempPathA(MAX_PATH, tmp)) {
            err = "no temp directory";
            return false;
        }
        char dir[MAX_PATH];
        std::snprintf(dir, sizeof(dir), "%snmail-att-%u", tmp, (unsigned)GetTickCount());
        if (!CreateDirectoryA(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = "could not create temp folder";
            return false;
        }
        std::string path = std::string(dir) + "\\" + name;
#else
        char tmpl[] = "/tmp/nmail-att-XXXXXX";
        if (!mkdtemp(tmpl)) {
            err = "could not create temp folder";
            return false;
        }
        std::string path = std::string(tmpl) + "/" + name;
#endif
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "could not write " + path;
            return false;
        }
        out.write(att.data.data(), (std::streamsize)att.data.size());
        out.close();
        if (!out) {
            err = "could not write " + path;
            return false;
        }
#ifndef _WIN32
        ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
        m_att_temps.push_back({ path,
#ifdef _WIN32
            dir
#else
            tmpl
#endif
        });
        path_out = path;
        return true;
    }

    bool desktop_open_file(const std::string &path) {
        if (path.empty()) return false;
        for (unsigned char c : path)
            if (c < 32) return false;
#ifndef _WIN32
        if (path.front() != '/') return false;
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
#ifdef __APPLE__
            execlp("open", "open", path.c_str(), (char *)nullptr);
#else
            execlp("xdg-open", "xdg-open", path.c_str(), (char *)nullptr);
            execlp("gio", "gio", "open", path.c_str(), (char *)nullptr);
#endif
            _exit(127);
        }
        return pid > 0;
#else
        return (int)(intptr_t)ShellExecuteA(NULL, "open", path.c_str(),
                                            NULL, NULL, SW_SHOWNORMAL) > 32;
#endif
    }

    Widget *make_att_preview_bar(Widget *parent) {
        Widget *bar = new Widget(parent);
        auto *fl = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                  AlignItems::Center, 0, 10);
        fl->set_padding(2, 4);
        bar->set_layout(fl);
        bar->set_live(true);
        Button *back = new Button(bar, "Back", FA_ARROW_LEFT);
        back->set_callback([this] { close_attachment_preview(); });
        new Label(bar, m_att_preview_name, "sans-bold", 18);
        if (m_gallery_sel >= 0 && m_gallery_sel < (int)m_gallery.size() &&
            !m_gallery[(size_t)m_gallery_sel].decodable) {
            Button *ext = new Button(bar, "Open externally");
            ext->set_callback([this] { open_gallery_external(); });
        }
        return bar;
    }

    void close_attachment_preview() {
        Vector2f keep = m_att_preview_scroll;
        m_att_preview = false;
        m_gallery.clear();
        m_gallery_sel = -1;
        if (m_has_message)
            render_current();
        if (m_view_scroll)
            m_view_scroll->set_scroll(keep);
    }

    /* stb_image (via nvgCreateImageMem) decodes JPEG/PNG/GIF/BMP, not HEIC. */
    bool image_bytes_decodable(const std::string &data) const {
        if (data.size() < 16) return false;
        int w = 0, h = 0, n = 0;
        return stbi_info_from_memory(
            (const unsigned char *)data.data(), (int)data.size(),
            &w, &h, &n) != 0;
    }

    /* Photos that are not already shown inline in the HTML. False when
     * none of them can be decoded — caller opens the file externally. */
    bool show_image_gallery(size_t att_index) {
        const auto &atts = m_current_message.attachments;
        if (att_index >= atts.size()) return false;
        auto vis = visible_attachments(m_current_message);
        std::vector<GalleryItem> items;
        int sel = -1;
        bool any = false;
        for (const MailAttachment *p : vis) {
            if (!attachment_is_photo(*p)) continue;
            size_t idx = (size_t)(p - atts.data());
            bool dec = image_bytes_decodable(p->data);
            if (dec) any = true;
            if (idx == att_index) sel = (int)items.size();
            items.push_back({idx, dec});
        }
        if (!any || sel < 0) return false;

        if (!m_att_preview) {
            m_att_preview_scroll = m_view_scroll ? m_view_scroll->scroll()
                                                 : Vector2f(0.f, 0.f);
        }
        m_gallery = std::move(items);
        m_gallery_sel = sel;
        m_att_preview = true;
        const MailAttachment &att = atts[m_gallery[(size_t)sel].att_index];
        m_att_preview_name = att_display_name(att);
        hide_att_popup();
        clear_image_textures();
        m_has_remote_images = false;
        m_doc_remotes.clear();

        std::string html =
            "<nmail-widget id=\"nmail-att-preview-bar\"></nmail-widget>";
        if (m_gallery[(size_t)sel].decodable) {
            html += "<img src=\"att:" + std::to_string(att_index) + "\" alt=\"\">";
        } else {
            std::string ext = attachment_ext(att);
            for (char &c : ext) c = (char)std::toupper((unsigned char)c);
            if (ext.empty()) ext = "This image";
            html += "<p>" + att_html_escape(ext) +
                    " can't be previewed in the app. "
                    "Use Open externally.</p>";
        }
        html += "<div style=\"height:12px\"></div>"
                "<nmail-widget id=\"nmail-att-gallery\"></nmail-widget>";
        m_view->set_html(html);
        m_images_btn->set_enabled(m_show_remote_images);
        m_images_btn->set_pushed(m_show_remote_images);
        if (m_view_scroll) m_view_scroll->set_scroll(0.0f);
        set_status(m_att_preview_name);
        redraw();
        return true;
    }

    void open_photo_at(size_t idx) {
        if (idx >= m_current_message.attachments.size()) return;
        const MailAttachment &att = m_current_message.attachments[idx];
        if (attachment_is_photo(att) && show_image_gallery(idx))
            return;
        open_attachment(att);
    }

    void open_gallery_external() {
        if (m_gallery_sel < 0 || m_gallery_sel >= (int)m_gallery.size()) return;
        size_t idx = m_gallery[(size_t)m_gallery_sel].att_index;
        if (idx >= m_current_message.attachments.size()) return;
        open_attachment_external(m_current_message.attachments[idx]);
    }

    Widget *make_gallery_strip(Widget *parent) {
        if (m_gallery.empty() || !parent) return nullptr;
        auto *strip = new AttachmentStrip(parent);
        const auto &atts = m_current_message.attachments;
        for (int i = 0; i < (int)m_gallery.size(); ++i) {
            const GalleryItem &g = m_gallery[(size_t)i];
            if (g.att_index >= atts.size()) continue;
            const MailAttachment &a = atts[g.att_index];
            int thumb = 0;
            if (g.decodable) {
                std::string key = "galthumb:" + std::to_string(g.att_index);
                thumb = create_image_texture(key, a.data);
            }
            auto *chip = new AttachmentChip(strip, a, thumb, 0.55f);
            if (i == m_gallery_sel) chip->set_selected(true);
            auto alive = m_alive;
            size_t idx = g.att_index;
            chip->on_activate = [this, idx, alive] {
                nanogui::async([this, idx, alive] {
                    if (*alive) show_image_gallery(idx);
                });
            };
            chip->on_menu = [this, chip](const Vector2i &p) {
                show_attachment_menu(chip, p);
            };
        }
        return strip;
    }

    void show_attachment_preview(const MailAttachment &att) {
        hide_att_popup();
        m_gallery.clear();
        m_gallery_sel = -1;
        m_att_preview_scroll = m_view_scroll ? m_view_scroll->scroll()
                                             : Vector2f(0.f, 0.f);
        m_att_preview = true;
        m_att_preview_name = att_display_name(att);
        clear_image_textures();
        m_has_remote_images = false;
        m_doc_remotes.clear();
        std::string html = attachment_is_html(att)
                               ? with_preview_bar(att.data)
                               : text_attachment_html(att);
        m_view->set_html(html);
        m_has_remote_images = m_view->has_remote_images();
        m_images_btn->set_pushed(m_show_remote_images);
        m_images_btn->set_enabled(m_has_remote_images || m_show_remote_images);
        if (m_view_scroll)
            m_view_scroll->set_scroll(0.0f);
        set_status(m_att_preview_name);
        redraw();
    }

    std::string downloads_dir() {
#ifdef _WIN32
        const char *up = std::getenv("USERPROFILE");
        std::string d = (up && up[0]) ? std::string(up) + "\\Downloads"
                                      : std::string("Downloads");
        _mkdir(d.c_str());
        return d;
#else
        const char *home = std::getenv("HOME");
        std::string d = (home && home[0]) ? std::string(home) + "/Downloads"
                                          : std::string("Downloads");
        ::mkdir(d.c_str(), 0755);
        return d;
#endif
    }

    std::string unique_path_in(const std::string &dir, const std::string &name) {
#ifdef _WIN32
        const char sep = '\\';
#else
        const char sep = '/';
#endif
        auto exists = [](const std::string &p) {
            std::ifstream in(p, std::ios::binary);
            return (bool)in;
        };
        std::string path = dir + sep + name;
        if (!exists(path)) return path;
        std::string stem = name, ext;
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0) {
            stem = name.substr(0, dot);
            ext = name.substr(dot);
        }
        for (int i = 1; i < 1000; ++i) {
            path = dir + sep + stem + "-" + std::to_string(i) + ext;
            if (!exists(path)) return path;
        }
        return path;
    }

    void open_attachment_in_browser(const MailAttachment &att) {
        if (attachment_is_exec(att) || !attachment_opens_in_browser(att)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Blocked",
                "This file cannot be opened in a browser.",
                "OK", "", false);
            dlg->center();
            return;
        }
        std::string path, err;
        if (!write_temp_attachment(att, path, err)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Open failed", err, "OK", "", false);
            dlg->center();
            return;
        }
        if (!desktop_open_file(path)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Open failed", "Could not open " + att_display_name(att),
                "OK", "", false);
            dlg->center();
        }
    }

    void save_attachment_to_downloads(const MailAttachment &att) {
        if (att.data.empty()) return;
        std::string ext = attachment_ext(att);
        std::string name = sanitize_filename(att.filename, ext);
        if (attachment_is_exec(att)) {
            if (name.size() < 9 ||
                name.compare(name.size() - 9, 9, ".download") != 0)
                name += ".download";
        }
        std::string path = unique_path_in(downloads_dir(), name);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        out.write(att.data.data(), (std::streamsize)att.data.size());
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        set_status("Saved " + path);
    }

    void open_attachment_external(const MailAttachment &att) {
        std::string path, err;
        if (!write_temp_attachment(att, path, err)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Open failed", err, "OK", "", false);
            dlg->center();
            return;
        }
        if (!desktop_open_file(path)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Open failed", "Could not open " + att_display_name(att),
                "OK", "", false);
            dlg->center();
        }
    }

    void open_attachment(const MailAttachment &att) {
        if (attachment_is_inpane_preview(att)) {
            show_attachment_preview(att);
            return;
        }
        if (attachment_is_photo(att)) {
            int idx = -1;
            const auto &atts = m_current_message.attachments;
            for (size_t i = 0; i < atts.size(); ++i) {
                if (atts[i].mime == att.mime && atts[i].filename == att.filename &&
                    atts[i].cid == att.cid && atts[i].data.size() == att.data.size()) {
                    idx = (int)i;
                    break;
                }
            }
            if (idx >= 0 && show_image_gallery((size_t)idx))
                return;
        }
        if (attachment_is_exec(att)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Blocked",
                "This file looks like a program or script and will not be opened.",
                "OK", "", false);
            dlg->center();
            return;
        }
        if (attachment_is_archive(att)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Archive",
                "Archive files are not opened automatically. Save it instead?",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att](int i) {
                if (i == 0) save_attachment(att);
            });
            dlg->center();
            return;
        }
        const std::string ext = attachment_ext(att);
        if (!is_open_allowlisted(ext)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Can't open",
                "This file type is not opened automatically. Save it instead?",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att](int i) {
                if (i == 0) save_attachment(att);
            });
            dlg->center();
            return;
        }
        open_attachment_external(att);
    }

    void save_attachment(const MailAttachment &att) {
        const std::string ext = attachment_ext(att);
        std::string name = sanitize_filename(att.filename, ext);
        if (attachment_is_exec(att)) {
            if (name.size() < 9 || name.compare(name.size() - 9, 9, ".download") != 0)
                name += ".download";
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Executable",
                "This file looks like a program or script. It will be saved with a "
                ".download suffix so it is not run by accident.",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att, name, ext](int i) {
                if (i == 0) save_attachment_to(att, name, ext.empty() ? "bin" : ext);
            });
            dlg->center();
            return;
        }
        if (attachment_is_archive(att)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Question,
                "Save archive",
                "Save " + att_display_name(att) + " to disk?",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att, name, ext](int i) {
                if (i == 0) save_attachment_to(att, name, ext.empty() ? "zip" : ext);
            });
            dlg->center();
            return;
        }
        save_attachment_to(att, name, ext.empty() ? "dat" : ext);
    }

    void save_attachment_to(const MailAttachment &att, const std::string &name,
                            const std::string &ext) {
        auto paths = file_dialog(
            { { ext, att.mime.empty() ? ext : att.mime } },
            true, false, "");
        if (paths.empty() || paths[0].empty())
            return;
        std::string path = paths[0];
        std::string low = att_lower(path);
        std::string suffix = "." + att_lower(ext);
        if (low.size() < suffix.size() ||
            low.compare(low.size() - suffix.size(), suffix.size(), suffix) != 0)
            path += suffix;
        (void)name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        out.write(att.data.data(), (std::streamsize)att.data.size());
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
        }
    }

    void save_current_email() {
        if (!m_has_message) return;
        const MailMessage &msg = m_current_message;
        if (msg.raw.empty()) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "No original message data to save.",
                "OK", "", false);
            dlg->center();
            return;
        }
        auto paths = file_dialog(
            { {"eml", "Email message (RFC 822)"}, {"msg", "Email message"} },
            true, false, "");
        if (paths.empty() || paths[0].empty())
            return;
        std::string path = paths[0];
        std::string low = att_lower(path);
        bool has_ext = low.size() >= 4 &&
            (low.rfind(".eml") == low.size() - 4 ||
             low.rfind(".msg") == low.size() - 4);
        if (!has_ext)
            path += ".eml";

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        out.write(msg.raw.data(), (std::streamsize)msg.raw.size());
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        set_status("Saved " + path);
    }

    /* ---- Preferences window ---- */
    /* Accounts on the left, the selected account's settings on the right --
     * a master/detail dialog rather than one flat form, since any number of
     * accounts can be configured (every one connects simultaneously; see
     * AccountSession / add_account_session). */
    void show_preferences() {
        Window *win = new Window(this, "Accounts", false);
        win->set_id("nmail-prefs");
        win->set_close_callback([this, win] { close_dialog(win); });
        win->set_traffic_lights_mask(0x1);   // close (red) button only
        win->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill,
                                      12, 10));
        win->set_min_width(680);

        // Working copy: only committed to m_config / live sessions on Save.
        auto accounts = std::make_shared<std::vector<MailAccount>>(m_config.accounts);
        auto selected = std::make_shared<int>(accounts->empty() ? -1 : 0);

        Split *split = new Split(win, Split::Orientation::Horizontal);
        // Split::set_min_size floors EACH pane, not the widget.
        split->set_min_size(100);
        split->set_max_size({2048, 2048});
        split->set_keep_size_on_resize(true);
        split->set_fixed_first_size(200);
        split->set_min_width(200 + 6 + 380);

        // ---- Left: account list ----
        Widget *left = new Widget(split);
        left->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 6));
        left->set_min_width(170);

        Widget *list = new Widget(left);
        list->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 2));

        Button *add_btn = new Button(left, "Add Account", FA_PLUS);

        // ---- Right: settings form for the selected account ----
        Widget *right = new Widget(split);
        right->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 10));
        right->set_min_width(380);

        Widget *form = new Widget(right);
        auto *form_layout = new GridLayout(Orientation::Horizontal, 2,
                                           Alignment::Middle, 0, 8);
        form_layout->set_col_alignment(
            std::vector<Alignment>{Alignment::Middle, Alignment::Fill});
        form->set_layout(form_layout);

        new Label(form, "Name:", "sans-bold");
        TextBox *name = new TextBox(form);
        name->set_placeholder("e.g. iCloud, Work Gmail");
        name->set_editable(true);
        name->set_alignment(TextBox::Alignment::Right);

        new Label(form, "IMAP server:", "sans-bold");
        TextBox *host = new TextBox(form);
        host->set_placeholder("imap.example.com");
        host->set_editable(true);
        host->set_alignment(TextBox::Alignment::Right);

        new Label(form, "Port:", "sans-bold");
        IntBox<int> *port = new IntBox<int>(form);
        port->set_editable(true);

        new Label(form, "Username:", "sans-bold");
        TextBox *user = new TextBox(form);
        user->set_placeholder("you@example.com");
        user->set_editable(true);
        user->set_alignment(TextBox::Alignment::Right);

        new Label(form, "Password:", "sans-bold");
        TextBox *pass = new TextBox(form);
        pass->set_editable(true);
        pass->set_alignment(TextBox::Alignment::Right);

        new Label(form, "SMTP server:", "sans-bold");
        TextBox *smtp_host = new TextBox(form);
        smtp_host->set_placeholder("(same as IMAP server)");
        smtp_host->set_editable(true);
        smtp_host->set_alignment(TextBox::Alignment::Right);

        new Label(form, "SMTP port:", "sans-bold");
        IntBox<int> *smtp_port = new IntBox<int>(form);
        smtp_port->set_editable(true);

        Widget *remove_row = new Widget(right);
        remove_row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
        Button *remove_btn = new Button(remove_row, "Remove Account", FA_TRASH);

        /* Flush the visible form into the model for whichever account is
         * currently selected -- called right before switching rows and
         * right before Save, so in-progress edits are never lost. */
        auto commit_current = [=]() {
            if (*selected < 0 || *selected >= (int)accounts->size()) return;
            MailAccount &a = (*accounts)[*selected];
            a.name       = name->value();
            a.host       = host->value();
            a.port       = port->value();
            a.username   = user->value();
            a.password   = pass->value();
            a.smtp_host  = smtp_host->value();
            a.smtp_port  = smtp_port->value();
        };

        /* Reflect account `idx` (or blank/disabled fields when idx < 0) in
         * the form. */
        auto populate_fields = [=](int idx) {
            bool have = idx >= 0 && idx < (int)accounts->size();
            MailAccount blank;
            const MailAccount &a = have ? (*accounts)[idx] : blank;
            name->set_value(a.name);
            host->set_value(a.host);
            port->set_value(a.port);
            user->set_value(a.username);
            pass->set_value(a.password);
            smtp_host->set_value(a.smtp_host);
            smtp_port->set_value(a.smtp_port);
            for (Widget *w : {(Widget*)name, (Widget*)host, (Widget*)port,
                             (Widget*)user, (Widget*)pass, (Widget*)smtp_host,
                             (Widget*)smtp_port})
                w->set_enabled(have);
            remove_btn->set_enabled(have);
        };

        /* Rebuild the row buttons from `*accounts` -- only needed when
         * accounts are added/removed, not on an ordinary selection click. */
        std::function<void()> rebuild_list = [=]() {
            while (!list->children().empty())
                list->remove_child_at(0);
            for (int i = 0; i < (int)accounts->size(); ++i) {
                const MailAccount &a = (*accounts)[i];
                Button *row = new Button(list, a.display_name(), FA_USER);
                row->set_flags(Button::RadioButton);
                row->set_pushed(i == *selected);
                row->set_callback([=]() {
                    commit_current();
                    *selected = i;
                    populate_fields(i);
                });
            }
            if (screen()) screen()->perform_layout();
        };
        rebuild_list();
        populate_fields(*selected);

        add_btn->set_callback([=]() {
            commit_current();
            MailAccount a;
            a.name = "New Account";
            accounts->push_back(a);
            *selected = (int)accounts->size() - 1;
            rebuild_list();
            populate_fields(*selected);
        });

        remove_btn->set_callback([=]() {
            if (*selected < 0 || *selected >= (int)accounts->size()) return;
            accounts->erase(accounts->begin() + *selected);
            if (*selected >= (int)accounts->size()) *selected = (int)accounts->size() - 1;
            rebuild_list();
            populate_fields(*selected);
        });

        // ---- General (shared, app-wide) settings ----
        Widget *general = new Widget(win);
        auto *general_layout = new GridLayout(Orientation::Horizontal, 2,
                                              Alignment::Middle, 0, 8);
        general_layout->set_col_alignment(
            std::vector<Alignment>{Alignment::Middle, Alignment::Fill});
        general->set_layout(general_layout);

        new Label(general, "Check for mail:", "sans-bold");
        Dropdown *check_interval = new Dropdown(general, Dropdown::ComboBox,
                                                "Check for mail");
        static const int kIntervalMinutes[] = {5, 15, 30, 60};
        check_interval->add_item({"Every 5 minutes", "check_5"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        check_interval->add_item({"Every 15 minutes", "check_15"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        check_interval->add_item({"Every 30 minutes", "check_30"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        check_interval->add_item({"Hourly", "check_60"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        {
            int idx = 1;   // default: 15 minutes
            for (int i = 0; i < 4; ++i)
                if (kIntervalMinutes[i] == m_config.check_interval_min) idx = i;
            check_interval->set_selected_index(idx);
        }

        new Label(general, "Contacts:", "sans-bold");
        CheckBox *save_contacts = new CheckBox(general, "Remember on disk");
        save_contacts->set_checked(m_config.save_contacts);
        save_contacts->set_tooltip(
            "Keep addresses harvested from your mail in " +
            contacts_path() + " so completions survive a restart. "
            "When off they are kept only for this session.");

        Widget *buttons = new Widget(win);
        buttons->set_layout(new BoxLayout(Orientation::Horizontal,
                                          Alignment::Middle, 0, 8));

        Button *save = new Button(buttons, "Save && Connect", FA_CHECK);
        save->set_callback([this, win, accounts, commit_current, check_interval,
                           save_contacts]() {
            commit_current();
            m_config.accounts = *accounts;
            int idx = check_interval->selected_index();
            m_config.check_interval_min =
                kIntervalMinutes[(idx >= 0 && idx < 4) ? idx : 1];
            m_config.save_contacts = save_contacts->checked();
            if (!save_config(m_config)) {
                auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                    "Save failed",
                    "Could not write " + config_path(), "OK", "", false);
                dlg->center();
                return;
            }
            m_config_loaded = true;
            /* Write straight away so enabling the option survives a crash. */
            if (m_config.save_contacts && m_contacts.dirty())
                m_contacts.save(contacts_path());
            if (PopupMenu *pop = check_interval->popup())
                pop->set_visible(false);

            /* Reconcile live sessions against the edited account list:
             * accounts identified by MailAccount::id() (username@host). An
             * edit to host/username changes the id, which this treats as
             * "old account removed, new one added" -- a clean reconnect
             * that only costs that one account's QRESYNC/body-cache warmth,
             * same tradeoff the id() doc comment already calls out. */
            std::vector<std::string> new_ids;
            for (const MailAccount &ma : m_config.accounts)
                if (!ma.host.empty()) new_ids.push_back(ma.id());

            std::vector<std::string> to_remove;
            for (auto &sess : m_accounts)
                if (std::find(new_ids.begin(), new_ids.end(), sess->config.id()) == new_ids.end())
                    to_remove.push_back(sess->config.id());
            for (const std::string &id : to_remove) remove_account_session(id);

            for (const MailAccount &ma : m_config.accounts) {
                if (ma.host.empty()) continue;   // still being filled in
                AccountSession *acct = account(ma.id());
                if (acct) {
                    acct->config = ma;
                    m_folder_view->update_account(ma.id(), ma.display_name(),
                                                  acct->folders, acct->current_folder);
                    acct->worker->set_config(ma);
                    acct->worker->set_check_interval_min(m_config.check_interval_min);
                    acct->worker->connect();
                } else {
                    add_account_session(ma);
                }
            }
            /* Destroy the prefs window after this callback returns so we
               do not free the Save button while it is still running. */
            close_dialog(win);
            glfwPostEmptyEvent();
        });

        Button *cancel = new Button(buttons, "Cancel", FA_TIMES);
        cancel->set_callback([this, win]() { close_dialog(win); });

        win->center();
        win->request_focus();
    }

    /* ---- Reply / Forward / New compose window ---- */
    /* reply: quote original, To = sender.  forward: quote original, empty To,
     * Fwd: subject, original attachments.  neither: blank new message. */
    void show_compose(bool reply = true, bool forward = false) {
        if ((reply || forward) && !m_has_message) return;
        const MailMessage orig = (reply || forward) ? m_current_message
                                                    : MailMessage{};

        Window *win = new Window(this,
            forward ? "Forward" : reply ? "Reply" : "New Message", true);
        win->set_id(forward ? "nmail-forward"
                  : reply   ? "nmail-reply"
                            : "nmail-compose");
        win->set_close_callback([this, win] { close_dialog(win); });
        /* A single-column AdvancedGridLayout instead of a Vertical BoxLayout:
         * BoxLayout never grows children past their preferred size on the
         * main axis, so the message body would stay a fixed height no
         * matter how tall the window got. Row 4 (the body) is the only row
         * with stretch, so it alone absorbs extra height on resize.  Row 6
         * is the attachment chip well.  Row 8 is the send-status row
         * (hidden until a send starts). */
        auto *win_layout = new AdvancedGridLayout(
            {0}, {0, 10, 0, 10, 0, 4, 0, 4, 0, 8, 0}, 12);
        win_layout->set_col_stretch(0, 1.0f);
        win_layout->set_row_stretch(4, 1.0f);
        win->set_layout(win_layout);
        /* Width only: a floor smaller than the layout's own natural size is
         * safe (there's a single column, so any slack just goes to it).
         * Deliberately no set_min_height() — the fixed chrome rows (form,
         * toolbar, buttons) can't shrink below their natural size, and the
         * body row is already floored at its own min_height(300), so the
         * layout's natural/intrinsic height *is* the right minimum; forcing
         * a smaller one would just make something else get clipped. */
        win->set_min_width(760);

        /* Which account this message goes out under.  Defaults to whichever
         * account is currently on screen (the one owning the message being
         * replied to/forwarded), falling back to the first configured
         * account for a brand-new compose with nothing open yet. */
        const bool multi_account = m_accounts.size() > 1;
        std::string from_account_id = m_current_account_id;
        if (from_account_id.empty() && !m_accounts.empty())
            from_account_id = m_accounts.front()->config.id();

        Widget *form = new Widget(win);
        win_layout->set_anchor(form, AdvancedGridLayout::Anchor(0, 0));
        /* Advanced grid so the entry column (stretch=1) absorbs all extra
         * width when the window is resized, while the label column stays
         * pinned to its natural width, flush left. */
        auto *form_layout = new AdvancedGridLayout({0, 8, 0}, {0, 8, 0}, 0);
        form_layout->set_col_stretch(2, 1.0f);
        if (multi_account) {
            // One extra content+gap row pair for the From: field below.
            form_layout->append_row(8);
            form_layout->append_row(0);
        }
        form->set_layout(form_layout);

        int form_row = 0;
        Dropdown *from_box = nullptr;
        if (multi_account) {
            Label *from_lbl = new Label(form, "From:", "sans-bold");
            from_box = new Dropdown(form, Dropdown::ComboBox, "From");
            int sel_idx = 0;
            for (size_t i = 0; i < m_accounts.size(); ++i) {
                const MailAccount &ma = m_accounts[i]->config;
                if (ma.id() == from_account_id) sel_idx = (int)i;
                from_box->add_item({ma.display_name(), ma.id()}, FA_USER,
                                   [] {}, {{0, 0}}, true);
            }
            from_box->set_selected_index(sel_idx);
            form_layout->set_anchor(from_lbl,
                AdvancedGridLayout::Anchor(0, form_row, Alignment::Minimum, Alignment::Middle));
            form_layout->set_anchor(from_box,
                AdvancedGridLayout::Anchor(2, form_row, Alignment::Fill, Alignment::Middle));
            form_row += 2;
        }

        Label *to_lbl = new Label(form, "To:", "sans-bold");
        AutoCompleteBox *to = new AutoCompleteBox(form);
        to->set_value(reply ? (orig.from_addr.empty() ? orig.from
                                                        : orig.from_addr)
                            : "");
        to->set_editable(true);
        /* Complete one recipient at a time so "a@x.com, ja" offers Jane. */
        to->set_token_separator(',');
        to->set_provider([this](const std::string &q) {
            std::vector<AutoCompleteBox::Item> out;
            for (const Contact &c : m_contacts.search(q, 8)) {
                AutoCompleteBox::Item it;
                it.label  = c.name.empty() ? c.address : c.name;
                it.detail = c.name.empty() ? "" : c.address;
                it.value  = format_address(c);
                out.push_back(it);
            }
            return out;
        });
        form_layout->set_anchor(to_lbl,
            AdvancedGridLayout::Anchor(0, form_row, Alignment::Minimum, Alignment::Middle));
        form_layout->set_anchor(to,
            AdvancedGridLayout::Anchor(2, form_row, Alignment::Fill, Alignment::Middle));
        form_row += 2;

        Label *subj_lbl = new Label(form, "Subject:", "sans-bold");
        TextBox *subj = new TextBox(form);
        std::string s = (reply || forward) ? orig.subject : "";
        if (reply && (s.size() < 3 || (s[0] != 'R' && s[0] != 'r') ||
            (s[1] != 'e' && s[1] != 'E') || s[2] != ':'))
            s = "Re: " + s;
        if (forward) {
            bool has_fwd = s.size() >= 4 &&
                (s[0] == 'F' || s[0] == 'f') &&
                (s[1] == 'W' || s[1] == 'w') &&
                (s[2] == 'D' || s[2] == 'd') && s[3] == ':';
            if (!has_fwd)
                s = "Fwd: " + s;
        }
        subj->set_value(s);
        subj->set_editable(true);
        form_layout->set_anchor(subj_lbl,
            AdvancedGridLayout::Anchor(0, form_row, Alignment::Minimum, Alignment::Middle));
        form_layout->set_anchor(subj,
            AdvancedGridLayout::Anchor(2, form_row, Alignment::Fill, Alignment::Middle));

        /* Format toolbar: WYSIWYG style toggles (Ctrl+B/I/U also work). */
        Widget *fmt = new Widget(win);
        fmt->set_layout(new BoxLayout(Orientation::Horizontal,
                                      Alignment::Middle, 0, 4));
        win_layout->set_anchor(fmt, AdvancedGridLayout::Anchor(0, 2));

        TextEditor *body = new TextEditor(win, TextEditor::Mode::RichText);
        body->set_read_only(false);
        body->set_background_color(m_dark ? Color(30, 31, 38, 255)
                                          : Color(250, 250, 252, 255));
        Style bs;
        bs.fgColor = text_color();
        bs.fontSize = (float)m_config.compose_font_size;
        body->set_default_style(bs);
        body->set_min_height(300);
        body->set_padding(10);
        win_layout->set_anchor(body, AdvancedGridLayout::Anchor(0, 4));

        /* Busy overlay shown over the body while a send is in flight:
         * same grid cell as the body, added after it so it draws on top
         * and gets input events first (children are hit-tested in reverse
         * order).  Hidden until Spinner::start(). */
        Spinner *spinner = new Spinner(win, "Sending...");
        win_layout->set_anchor(spinner, AdvancedGridLayout::Anchor(0, 4));

        /* Attachment well: compact chips + an Add tile.  Reply/Forward
         * start with the original's files (user can Remove). */
        auto compose_atts = std::make_shared<std::vector<MailAttachment>>();
        if (reply || forward) {
            for (const MailAttachment *a : visible_attachments(orig))
                compose_atts->push_back(*a);
        }
        auto *att_strip = new AttachmentStrip(win);
        win_layout->set_anchor(att_strip, AdvancedGridLayout::Anchor(0, 6));
        auto rebuild_atts = std::make_shared<std::function<void()>>();
        *rebuild_atts = [this, att_strip, compose_atts, rebuild_atts, win]() {
            hide_att_popup();
            while (att_strip->child_count() > 0)
                att_strip->remove_child_at(att_strip->child_count() - 1);
            for (size_t i = 0; i < compose_atts->size(); ++i) {
                const MailAttachment &a = (*compose_atts)[i];
                int thumb = 0;
                if (a.mime.rfind("image/", 0) == 0 && !a.data.empty())
                    thumb = create_image_texture(
                        "compose-att:" + std::to_string(i) + ":" + a.filename,
                        a.data);
                auto *chip = new AttachmentChip(att_strip, a, thumb, 0.5f);
                chip->on_open = [this, chip] { open_attachment(chip->attachment()); };
                chip->on_save = [this, chip] { save_attachment(chip->attachment()); };
                chip->on_menu = [this, chip](const Vector2i &p) {
                    show_attachment_menu(chip, p);
                };
                chip->on_remove = [compose_atts, rebuild_atts, i]() {
                    if (i < compose_atts->size())
                        compose_atts->erase(compose_atts->begin() +
                                            (std::ptrdiff_t)i);
                    (*rebuild_atts)();
                };
            }
            auto *add = new AddAttachmentChip(att_strip, 0.5f);
            add->on_click = [this, compose_atts, rebuild_atts]() {
                auto paths = file_dialog(
                    { {"pdf", "PDF"}, {"png", "PNG image"}, {"jpg", "JPEG image"},
                      {"txt", "Text"}, {"html", "HTML"}, {"zip", "Zip archive"},
                      {"*", "All files"} },
                    false, true, "");
                for (const std::string &path : paths) {
                    if (path.empty()) continue;
                    std::ifstream in(path, std::ios::binary);
                    if (!in) continue;
                    MailAttachment a;
                    size_t slash = path.find_last_of("/\\");
                    a.filename = slash == std::string::npos ? path
                                 : path.substr(slash + 1);
                    a.data.assign((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
                    std::string ext;
                    size_t dot = a.filename.find_last_of('.');
                    if (dot != std::string::npos)
                        ext = att_lower(a.filename.substr(dot + 1));
                    a.mime = mime_from_ext(ext);
                    if (!a.data.empty())
                        compose_atts->push_back(std::move(a));
                }
                (*rebuild_atts)();
            };
            if (Screen *s = screen()) {
                s->perform_layout();
                s->redraw();
            }
            (void)win;
        };
        (*rebuild_atts)();

        /* Send-status row: label + indeterminate bar, hidden until needed. */
        Widget *status_row = new Widget(win);
        status_row->set_layout(new BoxLayout(Orientation::Horizontal,
                                             Alignment::Middle, 0, 8));
        win_layout->set_anchor(status_row,
                               AdvancedGridLayout::Anchor(0, 8));
        new Label(status_row, "Sending...", "sans", 14);
        IndeterminateBar *send_bar = new IndeterminateBar(status_row);
        send_bar->set_fixed_size(Vector2i(140, 10));
        status_row->set_visible(false);

        auto make_fmt = [&](int icon, TextEditor::StyleFlag f,
                            const std::string &tip) {
            Button *b = new Button(fmt, "", icon);
            b->set_flags(Button::Flags::ToggleButton);
            b->set_font_size(20);
            b->set_tooltip(tip);
            b->set_callback([body, f]() {
                body->toggle_style(f);
                /* Widget::mouse_button_event() hands this button focus on
                 * mouse-down (any unfocused widget gets it on click); give
                 * it back so the caret stays live and the pending typing
                 * style toggle_style() just set for an empty selection is
                 * not wiped by having to click back into the editor. */
                body->request_focus();
            });
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
            b->set_callback([fn, body]() {
                fn();
                body->request_focus();   // see make_fmt's callback for why
            });
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

        /* Base font size for the whole document.  Headings/code scale off
         * this (TextEditor::set_base_font_size), so raising it grows H1 etc.
         * too instead of leaving them frozen at whatever size they were
         * originally applied at. */
        Widget *fmt_spacer = new Widget(fmt);
        fmt_spacer->set_min_width(10);
        fmt_spacer->set_width(10);
        new Label(fmt, "Size:", "sans-bold");
        IntBox<int> *font_size = new IntBox<int>(fmt);
        font_size->set_editable(true);
        font_size->set_spinnable(true);
        font_size->set_min_max_values(8, 36);
        font_size->set_value_increment(1);
        font_size->set_fixed_size(Vector2i(56, 0));
        font_size->set_value(m_config.compose_font_size);
        auto apply_font_size = [this, body, font_size](int v) {
            font_size->set_value(v);   // clamps to [8, 36]
            v = font_size->value();
            m_config.compose_font_size = v;
            save_config(m_config);
            body->set_base_font_size((float)v);
        };
        font_size->set_callback(apply_font_size);

        /* Ctrl/Cmd +/- bumps the compose font size, mirroring the main
         * window's viewer zoom shortcut. key_filter runs before
         * TextEditor::keyboard_event's own handling and, if it returns
         * true, before MailApp::keyboard_event's fallback chain ever sees
         * the key -- so this keeps the reply window's +/- from being
         * stolen by the HTML viewer zoom behind it. */
        body->key_filter = [font_size, apply_font_size](int key, int /*scancode*/,
                                                        int action, int mods) {
            if (!(mods & SYSTEM_COMMAND_MOD) ||
                (action != GLFW_PRESS && action != GLFW_REPEAT))
                return false;
            if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
                apply_font_size(font_size->value() + 1);
                return true;
            }
            if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
                apply_font_size(font_size->value() - 1);
                return true;
            }
            return false;
        };

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

        /* Prefill (reply only): empty paragraph for the reply, then the
         * quoted original as indented paragraphs (serialized back to
         * "> " lines). */
        {
            Document *doc = body->document().get();
            doc->paragraphs.clear();
            doc->addParagraph();   // reply goes here

            if (reply || forward) {
                doc->addParagraph();   // spacer

                Style meta_s = bs; meta_s.fgColor = meta_color();
                if (forward) {
                    doc->addParagraph("---------- Forwarded message ----------",
                                      meta_s);
                    if (!orig.from.empty())
                        doc->addParagraph("From: " + orig.from, meta_s);
                    if (!orig.date.empty())
                        doc->addParagraph("Date: " + orig.date, meta_s);
                    if (!orig.subject.empty())
                        doc->addParagraph("Subject: " + orig.subject, meta_s);
                    doc->addParagraph();
                } else {
                    doc->addParagraph("On " + orig.date + ", " + orig.from +
                                      " wrote:", meta_s);
                }

                std::istringstream iss(orig.body);
                std::string qline;
                while (std::getline(iss, qline)) {
                    if (!qline.empty() && qline.back() == '\r') qline.pop_back();
                    if (qline.empty()) continue;
                    Paragraph *qp = doc->addParagraph(qline, bs);
                    qp->leftIndent = 16.0f;
                }
            }
            doc->markLayoutDirty();
        }
        body->set_caret({0, 0});
        refresh_fmt();

        Widget *buttons = new Widget(win);
        /* Middle column has all the stretch, so it swallows the extra
         * width and pushes the action group flush against the right edge
         * of the window while the format group stays flush left. */
        auto *buttons_layout = new AdvancedGridLayout({0, 0, 0}, {0}, 0);
        buttons_layout->set_col_stretch(1, 1.0f);
        buttons->set_layout(buttons_layout);
        win_layout->set_anchor(buttons, AdvancedGridLayout::Anchor(0, 10));

        Widget *fmt_group = new Widget(buttons);
        fmt_group->set_layout(new BoxLayout(Orientation::Horizontal,
                                            Alignment::Middle, 0, 8));
        buttons_layout->set_anchor(fmt_group,
            AdvancedGridLayout::Anchor(0, 0, Alignment::Minimum, Alignment::Middle));

        /* Send format: plain text, Markdown (MailMate-style markup=
         * markdown), or a generated HTML body. */
        new Label(fmt_group, "Format:", "sans-bold");
        Dropdown *fmt_box = new Dropdown(fmt_group, Dropdown::ComboBox,
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

        Widget *action_group = new Widget(buttons);
        action_group->set_layout(new BoxLayout(Orientation::Horizontal,
                                               Alignment::Middle, 0, 8));
        buttons_layout->set_anchor(action_group,
            AdvancedGridLayout::Anchor(2, 0, Alignment::Maximum, Alignment::Middle));

        Button *send = new Button(action_group, "Send", FA_PAPER_PLANE);
        send->set_callback([this, win, send, to, subj, body, fmt_box, spinner,
                           status_row, send_bar, compose_atts, from_box,
                           from_account_id,
                           irt = reply ? orig.message_id : ""]() {
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
            /* Lock the composer and show busy feedback while SMTP runs. */
            to->set_editable(false);
            subj->set_editable(false);
            body->set_read_only(true);
            status_row->set_visible(true);
            send_bar->start();
            perform_layout();
            spinner->start();
            int fmt = fmt_box->selected_index();
            if (fmt < 0) fmt = 1;   // default to Markdown
            MailFormat format = fmt == 0 ? MailFormat::Plain
                              : fmt == 2 ? MailFormat::Html
                                         : MailFormat::Markdown;
            std::string text = fmt == 0 ? body->plain_text()
                             : fmt == 2 ? document_to_html(*body->document())
                                        : document_to_markdown(*body->document());
            std::string chosen_account_id = from_account_id;
            if (from_box) {
                int idx = from_box->selected_index();
                if (idx >= 0 && idx < (int)m_accounts.size())
                    chosen_account_id = m_accounts[idx]->config.id();
            }
            send_reply(win, send, spinner, status_row, send_bar, to, subj, body,
                       chosen_account_id, to_s, sub_s, text, irt, format,
                       compose_atts ? *compose_atts
                                    : std::vector<MailAttachment>{});
        });

        Button *cancel = new Button(action_group, "Cancel", FA_TIMES);
        cancel->set_callback([this, win]() { close_dialog(win); });

        win->center();
        win->request_focus();
    }

    /* Send on a one-shot thread (SMTP is a separate connection from the
     * IMAP worker); the result is marshalled back with nanogui::async. */
    void send_reply(Window *win, Button *send_btn, Spinner *spinner,
                    Widget *status_row, IndeterminateBar *send_bar,
                    TextBox *to_box, TextBox *subj_box,
                    TextEditor *editor,
                    const std::string &account_id,
                    const std::string &to, const std::string &subject,
                    const std::string &body, const std::string &irt,
                    MailFormat format,
                    std::vector<MailAttachment> attachments) {
        const AccountSession *acct = account(account_id);
        const MailAccount &ma = acct ? acct->config : current_acct().config;
        SmtpConfig sc;
        sc.host     = ma.smtp_host.empty() ? ma.host : ma.smtp_host;
        sc.port     = ma.smtp_port;
        sc.username = ma.username;
        sc.password = ma.password;
        std::string from = ma.username;

        set_status("Sending...");
        std::thread([this, win, send_btn, spinner, status_row, send_bar, to_box,
                     subj_box, editor, sc, from, to, subject, body,
                     irt, format, attachments]() {
            SmtpClient smtp;
            std::string err;
            bool ok = smtp.send(sc, from, to, subject, body, irt, format,
                                err, attachments);
            nanogui::async(std::function<void()>(
                [this, win, send_btn, spinner, status_row, send_bar, to_box,
                 subj_box, editor, ok, err]() {
                    if (ok) {
                        set_status("Sent");
                        win->dispose();
                        sync_taskbar();     // drop its taskbar button
                        redraw();
                    } else {
                        set_status("Send failed");
                        /* Restore the composer so the user can retry. */
                        spinner->stop();
                        if (send_bar) send_bar->stop();
                        status_row->set_visible(false);
                        send_btn->set_enabled(true);
                        to_box->set_editable(true);
                        subj_box->set_editable(true);
                        editor->set_read_only(false);
                        perform_layout();
                        auto *dlg = new MessageDialog(this,
                            MessageDialog::Type::Warning,
                            "Could not send message", err, "OK", "", false);
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
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && m_att_preview) {
            close_attachment_preview();
            return true;
        }
        // Ctrl/Cmd+S saves the current message as an .eml
        if (key == GLFW_KEY_S && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            save_current_email();
            return true;
        }
        // Ctrl/Cmd +/-/0 zoom (20% per step, visual via ZoomScrollPanel, not reflow)
        if ((modifiers & SYSTEM_COMMAND_MOD) && action == GLFW_PRESS && m_view_scroll) {
            Vector2i anchor = Vector2i(m_view_scroll->width()/2, m_view_scroll->height()/2);
            if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
                m_view_scroll->zoom_by(1.2, anchor);
                char buf[64]; std::snprintf(buf, sizeof(buf), "Zoom %.0f%%", m_view_scroll->zoom()*100.0);
                set_status(buf); redraw(); return true;
            }
            if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
                m_view_scroll->zoom_by(1.0/1.2, anchor);
                char buf[64]; std::snprintf(buf, sizeof(buf), "Zoom %.0f%%", m_view_scroll->zoom()*100.0);
                set_status(buf); redraw(); return true;
            }
            if (key == GLFW_KEY_0 || key == GLFW_KEY_KP_0) {
                m_view_scroll->reset_view();
                set_status("Zoom 100%"); redraw(); return true;
            }
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        pump_preview();
        sync_taskbar();
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
    chdir_to_bundle_resources();
    try {
        /* The IMAP worker writes to a socket the server may have closed;
         * nanoproxy's socket_write handles EPIPE, but only if SIGPIPE
         * doesn't kill the process first.  Windows has no SIGPIPE. */
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
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
#ifdef _WIN32
        /* /SUBSYSTEM:WINDOWS means stderr goes nowhere -- say it in a dialog. */
        MessageBoxA(NULL, error_msg.c_str(), "nmail",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
        return -1;
    }
    return 0;
}
