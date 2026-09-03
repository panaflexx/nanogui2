/*
 * nmail_view — standalone HtmlDocument viewer for emails saved from nmail.
 *
 *   nmail_view [file.eml | file.html]
 *
 * Opens the same renderer the reading pane uses. Drop a saved .eml (RFC 822
 * from nmail's Save button), a legacy nmail HTML dump, or any .html.
 *
 * Remote <img> URLs load in the background: the document paints immediately
 * with placeholders, then each texture is bound as it arrives.
 */
#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanogui/layout.h>
#include <nanogui/icons.h>
#include <nanogui/menu.h>
#include <nanogui/messagedialog.h>
#include <GLFW/glfw3.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "htmldocument.h"
#include "imap_client.h"
#include "attachment_widgets.h"
#include "saved_email.h"
#include "http_fetch.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../ext/glfw/deps/stb_image_write.h"

using namespace nanogui;

class MailViewApp : public Screen {
public:
    MailViewApp(const std::string &path)
        : Screen(Vector2i(900, 800), "nmail_view") {
        inc_ref();
        set_theme_mode(ThemeMode::Light);
        m_theme->m_split_divider_width = 2;

        auto *root_flex = new FlexLayout(FlexDirection::Column,
                                         JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        RootWindow *window = new RootWindow(this, root_flex);

        Widget *toolbar = new Widget(window);
        toolbar->set_min_height(52);
        toolbar->set_height(52);
        toolbar->set_height_flex(SizeMode::Fixed);
        toolbar->set_layout(
            new BoxLayout(Orientation::Horizontal, Alignment::Middle, 8, 4));

        auto make_btn = [&](int icon, const std::string &tip) {
            Button *btn = new Button(toolbar, "", icon);
            btn->set_font_size(40);
            btn->set_transparent(true);
            btn->set_tooltip(tip);
            return btn;
        };

        Button *open_btn = make_btn(FA_FOLDER_OPEN, "Open HTML email (Ctrl+O)");
        open_btn->set_callback([this]() { open_dialog(); });

        Button *save_btn = make_btn(FA_CAMERA, "Save to PNG (Ctrl+S)");
        save_btn->set_callback([this]() { save_dialog(); });

        m_theme_btn = make_btn(FA_SUN, "Toggle light/dark (Ctrl+T)");
        m_theme_btn->set_callback([this]() {
            apply_theme(m_dark ? ThemeMode::Light : ThemeMode::Dark);
        });

        m_title = new Label(toolbar, "nmail_view", "sans-bold", 18);
        m_title->set_width(400);

        m_scroll = new ZoomScrollPanel(window, ZoomScrollPanel::ScrollTypes::Both);
        m_scroll->set_reflow_on_zoom(false);
        m_scroll->set_zoom_range(0.5, 3.0);
        m_scroll->set_zoom_enabled(true);
        m_scroll->set_height_flex(SizeMode::Expanding);
        root_flex->set_flex_item(m_scroll, FlexLayout::FlexItem(1.0f));

        m_view = new HtmlDocument(m_scroll);
        m_view->image_resolver = [this](const std::string &src) {
            return resolve_image(src);
        };
        m_view->embed_widget = [this](Widget *parent, const HtmlEmbedSpec &spec)
                -> Widget * {
            if (spec.id == "nmail-att-preview-bar")
                return make_att_preview_bar(parent);
            if (spec.id != "nmail-attachments" || m_att_preview)
                return nullptr;
            return make_attachment_strip(parent);
        };
        apply_theme(ThemeMode::Light);

        m_status = new Label(window, "Open an .eml or HTML email, or pass a path",
                             "sans", 16);
        m_status->set_min_height(24);
        m_status->set_height(24);
        m_status->set_height_flex(SizeMode::Fixed);

        show_help();
        perform_layout();

        if (!path.empty())
            load_path(path);
    }

    ~MailViewApp() override {
        *m_alive = false;
        clear_image_textures();
    }

    void apply_theme(ThemeMode mode) {
        m_dark = (mode == ThemeMode::Dark);
        set_theme_mode(mode);
        m_theme_btn->set_icon(m_dark ? FA_SUN : FA_MOON);
        m_view->set_background(m_dark ? nvgRGBA(30, 31, 38, 255)
                                      : nvgRGBA(250, 250, 252, 255));
        m_view->set_colors(
            m_dark ? nvgRGBA(226, 227, 233, 255) : nvgRGBA(20, 20, 25, 255),
            m_dark ? nvgRGBA(150, 152, 166, 255) : nvgRGBA(110, 110, 125, 255));
        if (!m_last_raw.empty())
            apply_message(m_last, /*reset_scroll=*/false);
        else
            show_help();
        perform_layout();
        redraw();
    }

    void show_help() {
        m_view->set_html(
            "<h1>nmail_view</h1>"
            "<p>Open an <b>.eml</b> saved from nmail (Save / Ctrl+S), "
            "a legacy nmail HTML dump, or any .html fragment.</p>"
            "<ul>"
            "<li><b>Ctrl+O</b> — open file</li>"
            "<li><b>Ctrl+T</b> — light / dark</li>"
            "<li><b>Ctrl +/-</b> — zoom 20% step, <b>Ctrl+0</b> reset; pinch / Ctrl+wheel also</li>"
            "</ul>"
            "<p>Remote images load in the background.</p>");
        m_title->set_caption("nmail_view");
    }

    void open_dialog() {
        auto paths = file_dialog(
            { {"eml", "Email message (RFC 822)"},
              {"html", "HTML email"}, {"txt", "Plain text"} },
            false, false, "");
        if (!paths.empty() && !paths[0].empty())
            load_path(paths[0]);
    }

    static bool looks_like_rfc822(const std::string &s) {
        if (s.compare(0, 16, "<!-- nmail-saved") == 0)
            return false;
        size_t i = 0;
        if (s.compare(0, 5, "From ") == 0) {
            size_t nl = s.find('\n');
            if (nl == std::string::npos) return true;
            i = nl + 1;
        }
        size_t colon = s.find(':', i);
        size_t nl = s.find('\n', i);
        if (colon == std::string::npos || (nl != std::string::npos && colon > nl))
            return false;
        bool name = false;
        for (size_t k = i; k < colon; ++k) {
            unsigned char c = (unsigned char)s[k];
            if (std::isalnum(c) || c == '-') name = true;
            else if (c == ' ' || c == '\t') break;
            else return false;
        }
        return name;
    }

    static std::string html_esc(const std::string &s) {
        std::string o;
        for (char c : s) {
            if (c == '&') o += "&amp;";
            else if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else o += c;
        }
        return o;
    }

    void load_path(const std::string &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            m_status->set_caption("Could not open " + path);
            return;
        }
        std::string raw((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
        m_last_raw = raw;
        m_path = path;
        m_last = MailMessage{};

        if (raw.compare(0, 16, "<!-- nmail-saved") == 0) {
            SavedEmail e;
            std::string err;
            if (!nmail_parse_email(raw, e)) {
                m_status->set_caption("Could not parse " + path);
                return;
            }
            m_last.from = e.from;
            m_last.to = e.to;
            m_last.subject = e.subject;
            m_last.date = e.date;
            m_last.html = e.html;
            m_last.body = e.body;
        } else if (looks_like_rfc822(raw)) {
            if (!parse_rfc822_message(raw, m_last)) {
                m_status->set_caption("Could not parse RFC 822 " + path);
                return;
            }
        } else {
            m_last.html = raw;
        }

        apply_message(m_last, /*reset_scroll=*/true);
        glfwSetWindowTitle(glfw_window(), ("nmail_view — " + path).c_str());
        update_image_status();
    }

    Widget *make_attachment_strip(Widget *parent) {
        auto vis = visible_attachments(m_last);
        if (vis.empty() || !parent) return nullptr;
        auto *strip = new AttachmentStrip(parent);
        for (size_t i = 0; i < vis.size(); ++i) {
            const MailAttachment &a = *vis[i];
            int thumb = 0;
            if (a.mime.rfind("image/", 0) == 0 && !a.data.empty()) {
                std::string key = "att:" + std::to_string(i) + ":" + a.filename;
                thumb = create_texture(key, a.data);
            }
            auto *chip = new AttachmentChip(strip, a, thumb);
            chip->on_open = [this, chip] { open_attachment(chip->attachment()); };
            chip->on_save = [this, chip] { save_attachment(chip->attachment()); };
            chip->on_menu = [this, chip](const Vector2i &p) {
                show_attachment_menu(chip, p);
            };
        }
        return strip;
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
        return bar;
    }

    void close_attachment_preview() {
        Vector2f keep = m_att_preview_scroll;
        m_att_preview = false;
        apply_message(m_last, /*reset_scroll=*/false);
        if (m_scroll) m_scroll->set_scroll(keep);
    }

    void show_attachment_preview(const MailAttachment &att) {
        m_att_preview_scroll = m_scroll ? m_scroll->scroll() : Vector2f(0.f, 0.f);
        m_att_preview = true;
        m_att_preview_name = att_display_name(att);
        clear_image_textures();
        std::string html = attachment_is_html(att)
                               ? with_preview_bar(att.data)
                               : text_attachment_html(att);
        m_view->set_html(html);
        if (m_scroll) m_scroll->set_scroll(0.0f);
        perform_layout();
        redraw();
    }

    void open_attachment(const MailAttachment &att) {
        if (attachment_is_inpane_preview(att)) {
            show_attachment_preview(att);
            return;
        }
        if (attachment_is_exec(att) || attachment_is_archive(att) ||
            !is_open_allowlisted(attachment_ext(att)))
            return;
        std::string path, err;
        if (write_temp_attachment(att, path, err))
            desktop_open_file(path);
    }

    void save_attachment(const MailAttachment &att) {
        const std::string ext = attachment_ext(att);
        auto paths = file_dialog(
            { { ext.empty() ? "dat" : ext, att.mime.empty() ? "File" : att.mime } },
            true, false, "");
        if (paths.empty() || paths[0].empty()) return;
        std::string path = paths[0];
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out)
            out.write(att.data.data(), (std::streamsize)att.data.size());
    }

    static Vector2i visual_screen_pos(const Widget *w, const Vector2i &logical_abs) {
        const ZoomScrollPanel *zsp = nullptr;
        for (const Widget *p = w; p && !zsp; p = p->parent())
            zsp = dynamic_cast<const ZoomScrollPanel *>(p);
        if (!zsp) return logical_abs;
        Vector2i zsp_abs = zsp->absolute_position();
        Vector2i rel = logical_abs - zsp_abs;
        double z = zsp->zoom();
        auto pan = zsp->pan_offset();
        return zsp_abs + Vector2i(
            (int)std::lround(pan.x() + rel.x() * z),
            (int)std::lround(pan.y() + rel.y() * z));
    }

    void show_attachment_menu(AttachmentChip *chip, const Vector2i &screen_pos) {
        if (!chip) return;
        Screen *s = screen();
        Window *w = chip->window();
        if (!s || !w) return;
        if (!m_att_popup)
            m_att_popup = new PopupMenu(s, w, nullptr, false);
        while (m_att_popup->child_count() > 0)
            m_att_popup->remove_child_at(m_att_popup->child_count() - 1);
        const MailAttachment att = chip->attachment();
        auto *open_item = new MenuItem(m_att_popup, "Open");
        open_item->set_enabled(attachment_is_inpane_preview(att) ||
            (!attachment_is_exec(att) && !attachment_is_archive(att) &&
             is_open_allowlisted(attachment_ext(att))));
        open_item->set_callback([this, att] {
            if (m_att_popup) m_att_popup->set_visible(false);
            open_attachment(att);
        });
        auto *browser_item = new MenuItem(m_att_popup, "Open in browser");
        browser_item->set_enabled(attachment_opens_in_browser(att));
        browser_item->set_callback([this, att] {
            if (m_att_popup) m_att_popup->set_visible(false);
            std::string path, err;
            if (write_temp_attachment(att, path, err))
                desktop_open_file(path);
        });
        auto *save_item = new MenuItem(m_att_popup, "Save As\u2026");
        save_item->set_callback([this, att] {
            if (m_att_popup) m_att_popup->set_visible(false);
            save_attachment(att);
        });
        auto *dl_item = new MenuItem(m_att_popup, "Save to Downloads");
        dl_item->set_enabled(!att.data.empty());
        dl_item->set_callback([this, att] {
            if (m_att_popup) m_att_popup->set_visible(false);
            save_to_downloads(att);
        });
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
        redraw();
    }

    bool write_temp_attachment(const MailAttachment &att, std::string &path_out,
                               std::string &err) {
        const std::string ext = attachment_ext(att);
        const std::string name = sanitize_filename(att.filename, ext);
#ifndef _WIN32
        char tmpl[] = "/tmp/nmail-att-XXXXXX";
        if (!mkdtemp(tmpl)) { err = "temp dir"; return false; }
        std::string path = std::string(tmpl) + "/" + name;
#else
        (void)err;
        std::string path = name;
#endif
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { err = "write failed"; return false; }
        out.write(att.data.data(), (std::streamsize)att.data.size());
#ifndef _WIN32
        ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
        path_out = path;
        return true;
    }

    bool desktop_open_file(const std::string &path) {
#ifndef _WIN32
        if (path.empty() || path.front() != '/') return false;
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
#ifdef __APPLE__
            execlp("open", "open", path.c_str(), (char *)nullptr);
#else
            execlp("xdg-open", "xdg-open", path.c_str(), (char *)nullptr);
#endif
            _exit(127);
        }
        return pid > 0;
#else
        return false;
#endif
    }

    void save_to_downloads(const MailAttachment &att) {
        const char *home = std::getenv("HOME");
        std::string dir = (home && home[0]) ? std::string(home) + "/Downloads"
                                            : std::string("Downloads");
#ifndef _WIN32
        ::mkdir(dir.c_str(), 0755);
#endif
        std::string name = sanitize_filename(att.filename, attachment_ext(att));
        std::string path = dir + "/" + name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out)
            out.write(att.data.data(), (std::streamsize)att.data.size());
    }

    void apply_message(const MailMessage &msg, bool reset_scroll) {
        std::string caption = msg.subject.empty() ? m_path : msg.subject;
        m_title->set_caption(caption);
        m_doc_remotes.clear();
        clear_image_textures();

        auto is_full_doc = [](const std::string &s) {
            auto has = [&](const char *a, const char *b) {
                return s.find(a) != std::string::npos ||
                       s.find(b) != std::string::npos;
            };
            return has("<html", "<HTML") || has("<body", "<BODY");
        };

        std::string h;
        const bool fragment = msg.html.empty() || !is_full_doc(msg.html);
        if (fragment && (!msg.subject.empty() || !msg.from.empty())) {
            h += "<p><span style=\"font-size:24px\"><b>" +
                 html_esc(msg.subject) + "</b></span></p>";
            if (!msg.from.empty())
                h += "<p><b>From: </b>" + html_esc(msg.from) + "</p>";
            if (!msg.to.empty())
                h += "<p><b>To: </b>" + html_esc(msg.to) + "</p>";
            if (!msg.date.empty())
                h += "<p><b>Date: </b>" + html_esc(msg.date) + "</p>";
            h += "<hr>";
        }

        if (!msg.html.empty()) {
            m_view->set_html(with_attachment_slots(h + msg.html, msg));
        } else if (!msg.body.empty() || !visible_attachments(msg).empty()) {
            std::string body_html = h;
            if (!msg.body.empty()) {
                std::string p;
                for (char c : html_esc(msg.body)) {
                    if (c == '\n') p += "<br>";
                    else p += c;
                }
                body_html += "<p>" + p + "</p>";
            }
            m_view->set_html(with_attachment_slots(body_html, msg));
        } else {
            m_view->set_plain("");
        }
        if (reset_scroll)
            m_scroll->set_scroll(0.0f);
        perform_layout();
        redraw();
        update_image_status();
    }

    virtual bool keyboard_event(int key, int scancode,
                                int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        if (action != GLFW_PRESS)
            return false;
        if (key == GLFW_KEY_ESCAPE && m_att_preview) {
            close_attachment_preview();
            return true;
        }
        if (key == GLFW_KEY_O && (modifiers & SYSTEM_COMMAND_MOD)) {
            open_dialog();
            return true;
        }
        if (key == GLFW_KEY_S && (modifiers & SYSTEM_COMMAND_MOD)) {
            save_dialog();
            return true;
        }
        if (key == GLFW_KEY_T && (modifiers & SYSTEM_COMMAND_MOD)) {
            apply_theme(m_dark ? ThemeMode::Light : ThemeMode::Dark);
            return true;
        }
        if ((modifiers & SYSTEM_COMMAND_MOD) && m_scroll) {
            Vector2i anchor = Vector2i(m_scroll->width()/2, m_scroll->height()/2);
            if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) { m_scroll->zoom_by(1.2, anchor); redraw(); return true; }
            if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) { m_scroll->zoom_by(1.0/1.2, anchor); redraw(); return true; }
            if (key == GLFW_KEY_0 || key == GLFW_KEY_KP_0) { m_scroll->reset_view(); redraw(); return true; }
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, m_size.x(), m_size.y());
        nvgFillColor(ctx, m_dark ? nvgRGBA(30, 31, 38, 255)
                                 : nvgRGBA(235, 237, 242, 255));
        nvgFill(ctx);
        nvgRestore(ctx);
        Screen::draw(ctx);
        maybe_save_pending();
    }

    bool has_pending_images() const {
        return !m_remote_pending.empty() || m_fetch_inflight > 0;
    }

    bool save_png(const std::string &path) {
        std::string out = path;
        if (out.size() < 4 || out.substr(out.size()-4) != ".png")
            out += ".png";
        // Ensure layout reflects any just-bound images; auto-size to document
        for (int k=0;k<2;++k) { perform_layout(); }
        int dh = m_view ? m_view->size().y() : 0;
        int needH = std::min(8000, std::max(size().y(), dh + 120));
        if (needH != size().y()) {
            set_size(Vector2i(size().x(), needH));
            for (int k=0;k<2;++k) { perform_layout(); draw_all(); }
        } else {
            draw_all();
        }
        int W = size().x(), H = size().y();
        std::vector<unsigned char> buf((size_t)W * (size_t)H * 4);
        draw_setup();
        draw_contents();
        draw_widgets();
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        stbi_flip_vertically_on_write(1);
        int ok = stbi_write_png(out.c_str(), W, H, 4, buf.data(), W*4);
        if (ok) {
            m_status->set_caption("Saved " + out);
            printf("Saved PNG %s %dx%d\n", out.c_str(), W, H);
        } else {
            m_status->set_caption("Failed to save " + out);
            fprintf(stderr, "stbi_write_png failed for %s\n", out.c_str());
        }
        return ok != 0;
    }

    void save_dialog() {
        auto paths = file_dialog({{"png", "PNG image"}}, true, false, "");
        if (paths.empty() || paths[0].empty()) return;
        std::string p = paths[0];
        if (has_pending_images()) {
            m_pending_save_path = p;
            m_status->set_caption("Waiting for " + std::to_string(m_remote_pending.size() + m_fetch_inflight) + " image(s)… will save to " + p);
            // also arm timeout: if images stall, save anyway after 30s
            m_pending_save_deadline = glfwGetTime() + 30.0;
            return;
        }
        save_png(p);
    }

    void set_screenshot_path(const std::string &p) {
        m_screenshot_path = p;
        m_screenshot_deadline = glfwGetTime() + 30.0;
        m_screenshot_done = false;
        if (!p.empty()) m_status->set_caption("Screenshot pending: " + p);
    }

    void maybe_save_pending() {
        double now = glfwGetTime();
        // CLI --screenshot path: save after images settle, then exit mainloop
        if (!m_screenshot_path.empty() && !m_screenshot_done) {
            bool pending = has_pending_images();
            bool timeout = m_screenshot_deadline > 0 && now >= m_screenshot_deadline;
            // grace: once pending clears, wait 250ms for final relayout
            if (!pending || timeout) {
                if (!pending) {
                    if (m_screenshot_grace < 0) m_screenshot_grace = now + 0.25;
                    if (now < m_screenshot_grace && !timeout) return;
                }
                m_screenshot_done = true;
                if (getenv("NMAIL_DEBUG_SUMMARY") && m_view)
                    fprintf(stderr, "%s", m_view->debug_summary().c_str());
                bool ok = save_png(m_screenshot_path);
                printf("screenshot %s %s\n", ok ? "saved" : "FAILED", m_screenshot_path.c_str());
                // exit after one more frame so status is visible if interactive
                nanogui::async([this]() { nanogui::leave(); glfwPostEmptyEvent(); });
                glfwPostEmptyEvent();
            }
        }
        // toolbar Save button deferred save
        if (!m_pending_save_path.empty()) {
            bool pending = has_pending_images();
            bool timeout = m_pending_save_deadline > 0 && now >= m_pending_save_deadline;
            if (!pending || timeout) {
                std::string p = m_pending_save_path;
                m_pending_save_path.clear();
                m_pending_save_deadline = 0;
                save_png(p);
            }
        }
    }

private:
    static const int kMaxInflight = 4;

    void clear_image_textures() {
        for (auto &kv : m_img_tex)
            nvgDeleteImage(nvg_context(), kv.second);
        m_img_tex.clear();
    }

    static int b64_val(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    static bool decode_base64(const std::string &in, std::string &out) {
        out.clear();
        out.reserve(in.size() * 3 / 4);
        /* Unsigned + masked: see the matching decoder in imap_client.cpp.
         * A signed accumulator overflows after a few symbols. */
        unsigned val = 0;
        int valb = -8;
        for (unsigned char c : in) {
            if (c == '=' || std::isspace(c))
                continue;
            int d = b64_val((char)c);
            if (d < 0)
                return false;
            val = ((val << 6) | (unsigned)d) & 0xFFFFFFu;
            valb += 6;
            if (valb >= 0) {
                out.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return !out.empty();
    }

    static bool decode_data_url(const std::string &src, std::string &bytes) {
        if (src.rfind("data:", 0) != 0)
            return false;
        size_t comma = src.find(',');
        if (comma == std::string::npos)
            return false;
        std::string meta = src.substr(5, comma - 5);
        std::string data = src.substr(comma + 1);
        bool b64 = meta.find("base64") != std::string::npos;
        if (b64)
            return decode_base64(data, bytes);
        bytes.clear();
        for (size_t i = 0; i < data.size(); ++i) {
            if (data[i] == '%' && i + 2 < data.size()) {
                unsigned h = 0;
                if (std::sscanf(data.c_str() + i + 1, "%2x", &h) == 1) {
                    bytes.push_back((char)h);
                    i += 2;
                    continue;
                }
            }
            bytes.push_back(data[i] == '+' ? ' ' : data[i]);
        }
        return !bytes.empty();
    }

    bool load_local_bytes(const std::string &src, std::string &bytes) const {
        std::string path;
        if (src.rfind("file://", 0) == 0)
            path = src.substr(7);
        else if (src.find("://") != std::string::npos)
            return false;
        else if (!src.empty() && src[0] == '/')
            return false; /* site-root relative, no document base */
        else {
            size_t slash = m_path.rfind('/');
            std::string dir = (slash == std::string::npos)
                                  ? std::string(".")
                                  : m_path.substr(0, slash);
            path = dir + "/" + src;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        bytes.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
        return !bytes.empty();
    }

    HtmlImageInfo make_info(int id) {
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

    int create_texture(const std::string &src, const std::string &bytes) {
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

    HtmlImageInfo resolve_image(const std::string &src) {
        auto cached = m_img_tex.find(src);
        if (cached != m_img_tex.end())
            return make_info(cached->second);

        std::string bytes;
        if (src.rfind("cid:", 0) == 0) {
            std::string cid = src.substr(4);
            for (const MailImage &img : m_last.images) {
                if (img.cid == cid)
                    return make_info(create_texture(src, img.data));
            }
            return HtmlImageInfo{};
        }

        if (decode_data_url(src, bytes)) {
            int id = create_texture(src, bytes);
            return make_info(id);
        }

        if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0) {
            m_doc_remotes.insert(src);
            auto it = m_remote_bytes.find(src);
            if (it != m_remote_bytes.end()) {
                int id = create_texture(src, it->second);
                if (id <= 0)
                    m_remote_failed.insert(src);
                return make_info(id);
            }
            if (!m_remote_failed.count(src))
                queue_remote_fetch(src);
            return HtmlImageInfo{};
        }

        if (load_local_bytes(src, bytes)) {
            int id = create_texture(src, bytes);
            return make_info(id);
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
                int id = create_texture(url, m_remote_bytes[url]);
                if (id <= 0)
                    m_remote_failed.insert(url);
            }
        } else {
            m_remote_failed.insert(url);
        }

        Vector2f sc = m_scroll->scroll();
        m_view->bind_loaded_images();
        m_scroll->set_scroll(sc.y());
        update_image_status();
        pump_fetches();
    }

    void update_image_status() {
        if (m_path.empty())
            return;
        if (m_doc_remotes.empty()) {
            m_status->set_caption(m_path);
            return;
        }
        int have = 0, fail = 0;
        int n = (int)m_doc_remotes.size();
        for (const auto &u : m_doc_remotes) {
            if (m_img_tex.count(u))
                ++have;
            else if (m_remote_failed.count(u))
                ++fail;
        }
        char buf[768];
        if (have + fail >= n) {
            if (fail)
                std::snprintf(buf, sizeof(buf),
                              "%s — %d/%d images (%d failed)",
                              m_path.c_str(), have, n, fail);
            else
                std::snprintf(buf, sizeof(buf), "%s — %d images",
                              m_path.c_str(), have);
        } else {
            std::snprintf(buf, sizeof(buf), "%s — loading images %d/%d",
                          m_path.c_str(), have, n);
        }
        m_status->set_caption(buf);
    }

    ZoomScrollPanel *m_scroll = nullptr;
public:
    HtmlDocument *m_view   = nullptr;
private:
    Label        *m_title  = nullptr;
    Label        *m_status = nullptr;
    Button       *m_theme_btn = nullptr;
    bool          m_dark = false;
    std::string   m_path;
    std::string   m_last_raw;
    MailMessage   m_last;
    PopupMenu    *m_att_popup = nullptr;
    bool          m_att_preview = false;
    Vector2f      m_att_preview_scroll{0.f, 0.f};
    std::string   m_att_preview_name;
    // Save PNG (toolbar Ctrl+S or --screenshot)
    std::string   m_pending_save_path;
    double        m_pending_save_deadline = 0;
    std::string   m_screenshot_path;
    double        m_screenshot_deadline = 0;
    double        m_screenshot_grace = -1;
    bool          m_screenshot_done = false;

    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::unordered_map<std::string, int>         m_img_tex;
    std::unordered_map<std::string, std::string> m_remote_bytes;
    std::unordered_set<std::string>              m_remote_pending;
    std::unordered_set<std::string>              m_remote_failed;
    std::unordered_set<std::string>              m_doc_remotes;
    std::deque<std::string>                      m_fetch_queue;
    int                                          m_fetch_inflight = 0;
};

int main(int argc, char **argv) {
    // nmail_view [html] [--dump] [--gold path] [--screenshot out.png]
    // --screenshot opens, waits for remote images (or 30s), saves PNG, exits.
    // --dump legacy raw dump retained; --screenshot is the new image path API.
    bool dump = false; std::string gold;
    std::string screenshot;
    std::string path;
    for (int i=1;i<argc;++i) {
        std::string a=argv[i];
        if (a=="--dump") dump=true;
        else if (a=="--screenshot" && i+1<argc) screenshot=argv[++i];
        else if (a=="--gold" && i+1<argc) gold=argv[++i];
        else if (!a.empty() && a[0]!='-') path=a;
    }
    if (!screenshot.empty()) {
        // --screenshot supersedes --dump: interactive window that auto-saves+exits
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        nanogui::init();
        {
            // Resolve path relative to screenshot flag order: first positional
            std::string p = path;
            if (p.empty()) { for(int i=1;i<argc;++i){ std::string a=argv[i]; if(!a.empty()&&a[0]!='-'&&a.find(".html")!=std::string::npos){ p=a; break; }} }
            ref<MailViewApp> app = new MailViewApp(p);
            app->dec_ref();
            app->set_visible(true);
            for(int k=0;k<4;++k){ app->perform_layout(); app->draw_all(); }
            if (!gold.empty() && gold.find("dark")!=std::string::npos) app->apply_theme(ThemeMode::Dark);
            app->set_screenshot_path(screenshot);
            app->draw_all();
            nanogui::mainloop(-1);
        }
        nanogui::shutdown(); return 0;
    }
    if (dump) {
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        nanogui::init();
        // Size screen to fit full document + toolbar/status, capped.
        // This lets dump capture full email, not just 800px viewport.
        ref<MailViewApp> app = new MailViewApp(path);
        app->dec_ref();
        app->set_visible(true);
        // Allow measuring without viewport clip: resize screen to document height.
        for(int k=0;k<4;++k){ app->perform_layout(); app->draw_all(); }
        if (!gold.empty() && gold.find("dark")!=std::string::npos) app->apply_theme(ThemeMode::Dark);
        for(int k=0;k<3;++k){ app->perform_layout(); app->draw_all(); }
        // Auto-size to document height for full dump (cap 6000)
        if (auto *v = app->m_view) {
            int dh = v->size().y();
            int needH = std::min(6000, std::max(800, dh + 120));
            if (needH > app->size().y()) {
                app->set_size(Vector2i(app->size().x(), needH));
                for(int k=0;k<2;++k){ app->perform_layout(); app->draw_all(); }
            }
        }
        if (auto *v = app->m_view) printf("%s", v->debug_summary().c_str());
        app->draw_all();
        {
            app->draw_setup(); app->draw_contents(); app->draw_widgets();
            int W = app->size().x(), H = app->size().y();
            std::vector<unsigned char> buf(W*H*4);
            glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE, buf.data());
            const char *out="/tmp/nmail_view_dump.raw";
            FILE*f=fopen(out,"wb"); if(f){ fwrite(buf.data(),1,buf.size(),f); fclose(f); printf("dumped %s %dx%d\n",out,W,H); }
            if(!gold.empty()) printf("gold: %s (manual diff: compare %s vs gold)\n", gold.c_str(), out);
            // also write per-input named copy
            if (!path.empty()) {
                std::string base = path; size_t sl=base.rfind('/'); if(sl!=std::string::npos) base=base.substr(sl+1); size_t dot=base.rfind('.'); if(dot!=std::string::npos) base=base.substr(0,dot);
                char named[256]; snprintf(named,sizeof(named),"/tmp/nmail_%s_dump.raw", base.c_str());
                FILE*g=fopen(named,"wb"); if(g){ fwrite(buf.data(),1,buf.size(),g); fclose(g); printf("dumped %s\n",named); }
            }
        }
        printf("SURVIVED dump\n");
        nanogui::shutdown(); return 0;
    }
    try {
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        nanogui::init();
        {
            std::string p2;
            if (argc > 1) p2 = argv[1];
            // --dump already handled above; interactive path filters flags
            if (!p2.empty() && p2.rfind("--",0)==0) p2.clear();
            // prefer positional path from dump-parse
            if (!path.empty()) p2 = path;
            ref<MailViewApp> app = new MailViewApp(p2);
            app->dec_ref();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop(-1);
        }
        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::cerr << "nmail_view: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
