/*
 * nmail/attachment_widgets.h — Mail-style attachment chips for HtmlDocument.
 */
#pragma once

#include "htmldocument.h"
#include "imap_client.h"

#include <nanogui/widget.h>
#include <nanogui/layout.h>
#include <nanogui/theme.h>
#include <nanogui/screen.h>
#include <nanogui/opengl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

using namespace nanogui;

inline std::string att_lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline bool html_uses_cid(const std::string &html, const std::string &cid) {
    if (html.empty() || cid.empty()) return false;
    const std::string needle = att_lower("cid:" + cid);
    const std::string hay = att_lower(html);
    size_t p = hay.find(needle);
    while (p != std::string::npos) {
        size_t e = p + needle.size();
        char n = e < hay.size() ? hay[e] : '\0';
        if (!n || n == '"' || n == '\'' || n == ' ' || n == '>' ||
            n == '&' || n == '?' || n == '#')
            return true;
        p = hay.find(needle, p + 1);
    }
    return false;
}

inline std::vector<const MailAttachment *>
visible_attachments(const MailMessage &msg) {
    std::vector<const MailAttachment *> out;
    out.reserve(msg.attachments.size());
    for (const MailAttachment &a : msg.attachments) {
        if (a.data.empty()) continue;
        if (!a.cid.empty() && html_uses_cid(msg.html, a.cid))
            continue;
        out.push_back(&a);
    }
    return out;
}

inline std::string mime_ext_guess(const std::string &mime) {
    if (mime == "application/pdf") return "pdf";
    if (mime == "image/jpeg" || mime == "image/jpg") return "jpg";
    if (mime == "image/png")  return "png";
    if (mime == "image/gif")  return "gif";
    if (mime == "image/webp") return "webp";
    if (mime == "image/tiff") return "tiff";
    if (mime == "text/plain") return "txt";
    if (mime == "text/html")  return "html";
    if (mime == "text/csv")   return "csv";
    if (mime == "text/calendar") return "ics";
    if (mime == "application/rtf" || mime == "text/rtf") return "rtf";
    if (mime == "application/zip") return "zip";
    if (mime == "application/msword") return "doc";
    if (mime.find("wordprocessingml") != std::string::npos) return "docx";
    if (mime.find("spreadsheetml") != std::string::npos) return "xlsx";
    if (mime.find("presentationml") != std::string::npos) return "pptx";
    if (mime == "application/vnd.ms-excel") return "xls";
    if (mime == "application/vnd.ms-powerpoint") return "ppt";
    if (mime.find("opendocument.text") != std::string::npos) return "odt";
    if (mime.find("opendocument.spreadsheet") != std::string::npos) return "ods";
    if (mime.find("opendocument.presentation") != std::string::npos) return "odp";
    if (mime.rfind("audio/", 0) == 0) return "audio";
    if (mime.rfind("video/", 0) == 0) return "video";
    return "";
}

inline std::string attachment_ext(const MailAttachment &a) {
    std::string fn = a.filename;
    size_t dot = fn.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < fn.size() &&
        fn.find('/', dot) == std::string::npos &&
        fn.find('\\', dot) == std::string::npos) {
        std::string e = att_lower(fn.substr(dot + 1));
        if (e.size() <= 8 && e.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos)
            return e;
    }
    return mime_ext_guess(a.mime);
}

inline std::string format_bytes(size_t n) {
    char buf[32];
    if (n < 1024)
        std::snprintf(buf, sizeof(buf), "%zu bytes", n);
    else if (n < 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), n < 10 * 1024 ? "%.1f KB" : "%.0f KB",
                      n / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
    return buf;
}

inline std::string sanitize_filename(const std::string &raw, const std::string &ext) {
    std::string fn = raw;
    size_t slash = fn.find_last_of("/\\");
    if (slash != std::string::npos) fn = fn.substr(slash + 1);
    std::string out;
    out.reserve(fn.size());
    for (unsigned char c : fn) {
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c == '\\' || c == '/')
            out += '_';
        else
            out += (char)c;
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' '))
        out.erase(out.begin());
    if (out.size() > 120) out.resize(120);
    if (out.empty()) out = "attachment";
    if (!ext.empty()) {
        std::string have;
        size_t dot = out.find_last_of('.');
        if (dot != std::string::npos)
            have = att_lower(out.substr(dot + 1));
        if (have != ext)
            out += "." + ext;
    }
    return out;
}

inline bool ext_in(const std::string &ext, std::initializer_list<const char *> list) {
    for (const char *s : list)
        if (ext == s) return true;
    return false;
}

inline bool is_exec_ext(const std::string &ext) {
    return ext_in(ext, {
        "exe", "com", "bat", "cmd", "msi", "scr", "pif", "dll", "so",
        "dylib", "app", "bin", "run", "out", "elf", "sh", "bash", "zsh",
        "ps1", "py", "rb", "pl", "js", "jsx", "vbs", "jse", "wsf", "php",
        "lua", "desktop", "lnk", "url", "jar", "apk", "command", "cgi"
    });
}

inline bool is_archive_ext(const std::string &ext) {
    return ext_in(ext, {
        "zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "cab",
        "iso", "dmg", "pkg", "zst", "lz", "lzma"
    });
}

inline bool attachment_is_exec(const MailAttachment &a);
inline bool attachment_is_archive(const MailAttachment &a);

inline bool is_open_allowlisted(const std::string &ext) {
    return ext_in(ext, {
        "pdf", "txt", "rtf", "csv", "html", "htm",
        "png", "jpg", "jpeg", "gif", "webp", "tif", "tiff", "bmp", "heic", "heif",
        "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "ods", "odp",
        "mp3", "mp4", "wav", "aac", "m4a", "mov", "webm", "ogg",
        "json", "xml", "vcf", "ics", "svg", "pages", "numbers", "key"
    });
}

inline bool attachment_is_html(const MailAttachment &a) {
    if (a.data.empty()) return false;
    const std::string ext = attachment_ext(a);
    if (ext == "html" || ext == "htm") return true;
    return a.mime == "text/html";
}

/* Text or HTML: Open shows these in the reading pane, not a helper app. */
inline bool attachment_is_inpane_preview(const MailAttachment &a) {
    if (a.data.empty()) return false;
    if (attachment_is_exec(a) || attachment_is_archive(a)) return false;
    if (attachment_is_html(a)) return true;
    const std::string ext = attachment_ext(a);
    if (ext_in(ext, { "txt", "csv", "json", "xml", "md", "log", "ics",
                      "vcf", "css", "yml", "yaml", "ini", "conf" }))
        return true;
    if (a.mime.rfind("text/", 0) == 0) return true;
    if (a.mime == "application/json" || a.mime == "application/xml")
        return true;
    return false;
}

inline bool attachment_opens_in_browser(const MailAttachment &a) {
    if (a.data.empty() || attachment_is_exec(a) || attachment_is_archive(a))
        return false;
    if (attachment_is_inpane_preview(a) || attachment_is_html(a))
        return true;
    const std::string ext = attachment_ext(a);
    return ext_in(ext, { "pdf", "svg", "png", "jpg", "jpeg", "gif", "webp" });
}

inline std::string att_html_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == 0) continue;
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;";  break;
            case '>':  out += "&gt;";  break;
            case '"':  out += "&quot;"; break;
            default:   out += (char)c; break;
        }
    }
    return out;
}

inline std::string with_preview_bar(std::string html) {
    const char *slot = "<nmail-widget id=\"nmail-att-preview-bar\"></nmail-widget>";
    std::string low = att_lower(html);
    size_t p = low.find("<body");
    if (p != std::string::npos) {
        size_t gt = html.find('>', p);
        if (gt != std::string::npos) {
            html.insert(gt + 1, slot);
            return html;
        }
    }
    return std::string(slot) + html;
}

inline std::string text_attachment_html(const MailAttachment &att) {
    std::string data = att.data;
    data.erase(std::remove(data.begin(), data.end(), '\0'), data.end());
    const size_t cap = 1024 * 1024;
    bool trunc = data.size() > cap;
    if (trunc) data.resize(cap);
    std::string html =
        "<nmail-widget id=\"nmail-att-preview-bar\"></nmail-widget>"
        "<pre style=\"font-family:monospace;font-size:15px;"
        "white-space:pre-wrap;word-wrap:break-word\">";
    html += att_html_escape(data);
    if (trunc)
        html += "\n\n[truncated]";
    html += "</pre>";
    return html;
}

enum class AttMagic { Other, Exec, Zip };
inline AttMagic sniff_magic(const std::string &data) {
    if (data.size() < 4) return AttMagic::Other;
    const unsigned char *b = (const unsigned char *)data.data();
    if (b[0] == 'M' && b[1] == 'Z') return AttMagic::Exec;
    if (b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')
        return AttMagic::Exec;
    uint32_t be = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                  ((uint32_t)b[2] << 8) | b[3];
    if (be == 0xFEEDFACEu || be == 0xFEEDFACFu || be == 0xCAFEBABEu ||
        be == 0xCEFAEDFEu || be == 0xCFFAEDFEu)
        return AttMagic::Exec;
    if (b[0] == 'P' && b[1] == 'K') return AttMagic::Zip;
    return AttMagic::Other;
}

inline bool attachment_is_exec(const MailAttachment &a) {
    const std::string ext = attachment_ext(a);
    if (is_exec_ext(ext)) return true;
    return sniff_magic(a.data) == AttMagic::Exec;
}

inline bool attachment_is_archive(const MailAttachment &a) {
    const std::string ext = attachment_ext(a);
    if (is_archive_ext(ext)) return true;
    if (sniff_magic(a.data) == AttMagic::Zip &&
        !ext_in(ext, { "docx", "xlsx", "pptx", "odt", "ods", "odp",
                       "pages", "numbers", "key", "epub" }))
        return true;
    return false;
}

inline NVGcolor att_type_color(const std::string &ext) {
    if (ext == "pdf") return nvgRGB(196, 52, 48);
    if (ext == "doc" || ext == "docx" || ext == "odt" || ext == "pages")
        return nvgRGB(42, 92, 178);
    if (ext == "xls" || ext == "xlsx" || ext == "ods" || ext == "numbers" ||
        ext == "csv")
        return nvgRGB(36, 138, 68);
    if (ext == "ppt" || ext == "pptx" || ext == "odp" || ext == "key")
        return nvgRGB(208, 108, 36);
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
        ext == "webp" || ext == "tif" || ext == "tiff" || ext == "bmp" ||
        ext == "heic" || ext == "heif" || ext == "svg")
        return nvgRGB(42, 138, 148);
    if (is_archive_ext(ext)) return nvgRGB(140, 102, 48);
    if (ext == "txt" || ext == "rtf") return nvgRGB(96, 98, 110);
    return nvgRGB(88, 112, 156);
}

inline std::string att_badge(const std::string &ext) {
    if (ext.empty()) return "";
    std::string b = ext;
    if (b.size() > 4) b.resize(4);
    for (char &c : b) c = (char)std::toupper((unsigned char)c);
    return b;
}

inline std::string att_display_name(const MailAttachment &a) {
    if (!a.filename.empty()) {
        std::string fn = a.filename;
        size_t slash = fn.find_last_of("/\\");
        if (slash != std::string::npos) fn = fn.substr(slash + 1);
        if (!fn.empty()) return fn;
    }
    std::string ext = attachment_ext(a);
    return ext.empty() ? "Attachment" : "Attachment." + ext;
}

inline std::string ellipsize(NVGcontext *ctx, const std::string &s, float max_w) {
    if (nvgTextBounds(ctx, 0, 0, s.c_str(), nullptr, nullptr) <= max_w)
        return s;
    std::string out = s;
    while (out.size() > 1) {
        out.pop_back();
        std::string t = out + "\xE2\x80\xA6";
        if (nvgTextBounds(ctx, 0, 0, t.c_str(), nullptr, nullptr) <= max_w)
            return t;
    }
    return "\xE2\x80\xA6";
}

class AttachmentChip : public Widget {
public:
    std::function<void()> on_open;
    std::function<void()> on_save;
    std::function<void(const Vector2i &screen_pos)> on_menu;

    AttachmentChip(Widget *parent, const MailAttachment &att, int thumb)
        : Widget(parent), m_att(att), m_thumb(thumb) {
        set_live(true);
        set_cursor(Cursor::Hand);
        std::string tip = att_display_name(att) + "\n" +
                          format_bytes(att.data.size());
        if (!att.mime.empty()) tip += "\n" + att.mime;
        set_tooltip(tip);
        m_ext = attachment_ext(att);
        m_name = att_display_name(att);
        m_size_label = format_bytes(att.data.size());
        m_badge = att_badge(m_ext);
        m_accent = att_type_color(m_ext);
    }

    const MailAttachment &attachment() const { return m_att; }

    void set_selected(bool sel) {
        if (m_selected == sel) return;
        m_selected = sel;
        if (Screen *s = screen()) s->redraw();
    }
    bool selected() const { return m_selected; }

    virtual Vector2i preferred_size(NVGcontext *) const override {
        return Vector2i(kChipW, kChipH);
    }

    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        m_hover = enter;
        if (Screen *s = screen()) s->redraw();
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool focus_event(bool focused) override {
        if (!focused)
            set_selected(false);
        return Widget::focus_event(focused);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down,
                                    int mods) override {
        (void)mods;
        if (!contains(p)) return false;
        if (button == GLFW_MOUSE_BUTTON_RIGHT && down) {
            select_only();
            request_focus();
            if (on_menu) on_menu(absolute_position() + (p - m_pos));
            return true;
        }
        if (button == GLFW_MOUSE_BUTTON_1 && down) {
            double now = glfwGetTime();
            bool dbl = m_last_click > 0.0 && (now - m_last_click) < 0.35;
            m_last_click = dbl ? 0.0 : now;
            select_only();
            request_focus();
            if (dbl && on_open) on_open();
            return true;
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        const float x = (float)m_pos.x(), y = (float)m_pos.y();
        const float w = (float)m_size.x(), h = (float)m_size.y();
        bool dark = false;
        if (Theme *t = theme())
            dark = (t->m_text_color.r() + t->m_text_color.g() +
                    t->m_text_color.b()) > 1.5f;

        if (m_hover || m_selected) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y, w, h, 8.0f);
            nvgFillColor(ctx, m_selected
                ? (dark ? nvgRGBA(80, 110, 180, 50) : nvgRGBA(40, 90, 180, 28))
                : (dark ? nvgRGBA(255, 255, 255, 18) : nvgRGBA(0, 0, 0, 12)));
            nvgFill(ctx);
        }

        const float pw = 56.0f, ph = 72.0f, fold = 13.0f, rad = 3.5f;
        const float px = x + (w - pw) * 0.5f;
        const float py = y + 8.0f;
        NVGcolor paper = dark ? nvgRGB(58, 59, 68) : nvgRGB(248, 246, 240);
        NVGcolor edge  = dark ? nvgRGB(110, 112, 124) : nvgRGB(196, 188, 172);
        NVGcolor foldc = dark ? nvgRGB(72, 74, 84) : nvgRGB(232, 226, 214);

        nvgBeginPath(ctx);
        nvgMoveTo(ctx, px + rad, py);
        nvgLineTo(ctx, px + pw - fold, py);
        nvgLineTo(ctx, px + pw, py + fold);
        nvgLineTo(ctx, px + pw, py + ph - rad);
        nvgQuadTo(ctx, px + pw, py + ph, px + pw - rad, py + ph);
        nvgLineTo(ctx, px + rad, py + ph);
        nvgQuadTo(ctx, px, py + ph, px, py + ph - rad);
        nvgLineTo(ctx, px, py + rad);
        nvgQuadTo(ctx, px, py, px + rad, py);
        nvgClosePath(ctx);
        nvgFillColor(ctx, paper);
        nvgFill(ctx);
        nvgStrokeWidth(ctx, 1.15f);
        nvgStrokeColor(ctx, edge);
        nvgStroke(ctx);

        nvgBeginPath(ctx);
        nvgMoveTo(ctx, px + pw - fold, py);
        nvgLineTo(ctx, px + pw, py + fold);
        nvgLineTo(ctx, px + pw - fold, py + fold);
        nvgClosePath(ctx);
        nvgFillColor(ctx, foldc);
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, px + pw - fold, py);
        nvgLineTo(ctx, px + pw, py + fold);
        nvgLineTo(ctx, px + pw - fold, py + fold);
        nvgStrokeColor(ctx, edge);
        nvgStroke(ctx);

        if (m_thumb > 0) {
            nvgSave(ctx);
            nvgIntersectScissor(ctx, px + 1, py + fold + 1,
                                pw - 2, ph - fold - 7);
            int iw = 0, ih = 0;
            nvgImageSize(ctx, m_thumb, &iw, &ih);
            float tw = (float)std::max(iw, 1), th = (float)std::max(ih, 1);
            float scale = std::max((pw - 2) / tw, (ph - fold - 7) / th);
            float dw = tw * scale, dh = th * scale;
            float ox = px + 1 + ((pw - 2) - dw) * 0.5f;
            float oy = py + fold + 1 + ((ph - fold - 7) - dh) * 0.5f;
            NVGpaint paint = nvgImagePattern(ctx, ox, oy, dw, dh, 0.0f,
                                             m_thumb, 1.0f);
            nvgBeginPath(ctx);
            nvgRect(ctx, px + 1, py + fold + 1, pw - 2, ph - fold - 7);
            nvgFillPaint(ctx, paint);
            nvgFill(ctx);
            nvgRestore(ctx);
        }

        nvgBeginPath(ctx);
        nvgRect(ctx, px, py + ph - 5.0f, pw, 5.0f);
        nvgFillColor(ctx, m_accent);
        nvgFill(ctx);

        if (!m_badge.empty() && m_thumb <= 0) {
            nvgFontFace(ctx, "sans-bold");
            nvgFontSize(ctx, 9.0f);
            float bw = nvgTextBounds(ctx, 0, 0, m_badge.c_str(), nullptr, nullptr);
            float bh = 13.0f, pad = 5.0f;
            float bx = px + 5.0f, by = py + ph - 22.0f;
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, bx, by, bw + pad * 2, bh, 2.5f);
            nvgFillColor(ctx, m_accent);
            nvgFill(ctx);
            nvgFillColor(ctx, nvgRGB(255, 255, 255));
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(ctx, bx + pad, by + bh * 0.5f, m_badge.c_str(), nullptr);
        }

        NVGcolor ink = dark ? nvgRGB(226, 227, 233) : nvgRGB(32, 32, 38);
        NVGcolor meta = dark ? nvgRGB(150, 152, 166) : nvgRGB(110, 110, 125);
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 11.5f);
        nvgFillColor(ctx, ink);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        std::string shown = ellipsize(ctx, m_name, w - 8.0f);
        nvgText(ctx, x + w * 0.5f, py + ph + 6.0f, shown.c_str(), nullptr);
        nvgFontSize(ctx, 10.0f);
        nvgFillColor(ctx, meta);
        nvgText(ctx, x + w * 0.5f, py + ph + 20.0f, m_size_label.c_str(), nullptr);
    }

private:
    static constexpr int kChipW = 88;
    static constexpr int kChipH = 118;
    MailAttachment m_att;
    int            m_thumb = 0;
    std::string    m_ext, m_name, m_size_label, m_badge;
    NVGcolor       m_accent{};
    bool           m_hover = false;
    bool           m_selected = false;
    double         m_last_click = 0.0;

    void select_only() {
        if (Widget *par = parent()) {
            for (Widget *c : par->children())
                if (auto *ch = dynamic_cast<AttachmentChip *>(c))
                    ch->set_selected(ch == this);
        } else {
            set_selected(true);
        }
    }
};

class AttachmentStrip : public Widget {
public:
    explicit AttachmentStrip(Widget *parent) : Widget(parent) {
        set_live(true);
        set_height_flex(SizeMode::Preferred);
        set_width_flex(SizeMode::Expanding);
    }

    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        int inner = m_size.x() > 40 ? m_size.x()
                  : (parent() && parent()->width() > 40 ? parent()->width() : 400);
        return layout_chips(ctx, inner, nullptr);
    }

    virtual void perform_layout(NVGcontext *ctx) override {
        layout_chips(ctx, std::max(40, m_size.x()), this);
        int need = preferred_size(ctx).y();
        if (need > 0 && std::abs(need - m_size.y()) > 2) {
            for (Widget *p = parent(); p; p = p->parent())
                if (auto *hd = dynamic_cast<HtmlDocument *>(p)) {
                    hd->request_reflow();
                    break;
                }
        }
    }

private:
    static constexpr int kGap = 10, kPad = 8;
    Vector2i layout_chips(NVGcontext *ctx, int inner, Widget *place) const {
        int x = kPad, y = kPad, row_h = 0, max_x = kPad;
        for (Widget *c : m_children) {
            if (!c->visible()) continue;
            Vector2i ps = c->preferred_size(ctx);
            if (x > kPad && x + ps.x() + kPad > inner) {
                y += row_h + kGap;
                x = kPad;
                row_h = 0;
            }
            if (place) {
                c->set_position(Vector2i(x, y));
                c->set_size(ps);
                c->perform_layout(ctx);
            }
            x += ps.x() + kGap;
            row_h = std::max(row_h, ps.y());
            max_x = std::max(max_x, x);
        }
        return Vector2i(std::max(inner, max_x + kPad - kGap),
                        y + row_h + kPad);
    }
};

inline std::string with_attachment_slots(std::string html, const MailMessage &msg) {
    if (visible_attachments(msg).empty()) return html;
    const std::string slot =
        "<div style=\"height:16px\"></div>"
        "<nmail-widget id=\"nmail-attachments\"></nmail-widget>";
    std::string low = att_lower(html);
    size_t p = low.rfind("</body>");
    if (p != std::string::npos)
        html.insert(p, slot);
    else
        html += slot;
    return html;
}
