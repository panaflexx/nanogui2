/*
 * nmail/htmldocument.cpp — see htmldocument.h.
 *
 * The Gumbo walk below keeps the block structure of the source HTML:
 * container elements (div/table/tr/td/...) become Widgets with a
 * FlexLayout, while runs of inline content are accumulated into a
 * nanogui::Document and rendered by a single HtmlText leaf widget.
 * Inline style propagation (bold/italic/underline/mono/colors/sizes)
 * mirrors the old linear html_walk renderer this replaces.
 */
#include "htmldocument.h"

#include <nanogui/layout.h>
#include <nanogui/screen.h>
#include "gumbo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <typeinfo>
#include <vector>

using namespace nanogui;

namespace {

#ifdef DEBUG
/* NMAIL_DEBUG_DRAW=1: trace widget draw positions + the active NVG
 * transform, to catch content painting in the wrong coordinate space. */
bool debug_draw() {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("NMAIL_DEBUG_DRAW");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v == 1;
}

void trace_draw(const char *what, const Widget *w, const char *text = nullptr) {
    if (!debug_draw()) return;
    NVGcontext *ctx = nullptr;
    if (const Screen *s = w->screen()) ctx = s->nvg_context();
    float xf[6] = {0};
    int depth = -1, recording = -1;
    if (ctx) {
        nvgCurrentTransform(ctx, xf);
        depth = nvgStateDepth(ctx);
        recording = nvgIsRecordingDisplayList(ctx) ? 1 : 0;
    }
    fprintf(stderr, "[trace] %s %p pos=(%d,%d) size=(%d,%d) "
            "xform=[%.2f %.2f %.2f %.2f %.1f %.1f] depth=%d rec=%d",
            what, (const void *)w, w->position().x(), w->position().y(),
            w->size().x(), w->size().y(),
            xf[0], xf[1], xf[2], xf[3], xf[4], xf[5], depth, recording);
    const Widget *p = w->parent();
    for (int i = 0; p && i < 8; ++i, p = p->parent())
        fprintf(stderr, " <- %s(%d,%d)%s%s", typeid(*p).name(),
                p->position().x(), p->position().y(),
                p->cached() ? ":CACHED" : "",
                p->live() ? ":live" : "");
    if (!p) fprintf(stderr, " <- ROOT(null)");
    if (text) fprintf(stderr, " text=\"%.30s\"", text);
    fprintf(stderr, "\n");
}
#else
inline bool debug_draw() { return false; }
inline void trace_draw(const char *, const Widget *, const char * = nullptr) {}
#endif

// ---------------------------------------------------------------------------
// Color parsing: "#rgb" / "#rrggbb" / "rgb(r,g,b)" / common color names.
// ---------------------------------------------------------------------------
NVGcolor parse_html_color(const char *s, bool &ok) {
    ok = true;
    if (s && s[0] == '#') {
        unsigned r = 0, g = 0, b = 0;
        size_t len = strlen(s + 1);
        if (len == 6 && std::sscanf(s + 1, "%02x%02x%02x", &r, &g, &b) == 3)
            return nvgRGB(r, g, b);
        if (len == 3 && std::sscanf(s + 1, "%01x%01x%01x", &r, &g, &b) == 3)
            return nvgRGB(r * 17, g * 17, b * 17);
    } else if (s) {
        std::string l;
        for (const char *p = s; *p; ++p)
            l += (char)std::tolower((unsigned char)*p);
        /* "rgb(12, 34, 56)" / "rgba(12, 34, 56, 0.5)" — whitespace after
         * each comma is optional and inconsistent in the wild, so walk
         * the number list instead of matching a fixed sscanf pattern
         * (which silently dropped every rgba() as "unknown": "rgba"
         * matches the old rfind("rgb",0)==0 check, but its 3-argument
         * sscanf pattern can never match the trailing alpha). */
        if (l.rfind("rgb", 0) == 0) {
            bool has_alpha = l.rfind("rgba", 0) == 0;
            size_t p = l.find('(');
            if (p != std::string::npos) {
                float comp[4] = { 0, 0, 0, 255 };
                int n = 0;
                size_t i = p + 1;
                while (i < l.size() && n < 4) {
                    while (i < l.size() && (l[i] == ' ' || l[i] == ','))
                        ++i;
                    char *end = nullptr;
                    float v = std::strtof(l.c_str() + i, &end);
                    if (end == l.c_str() + i)
                        break;
                    comp[n++] = v;
                    i = end - l.c_str();
                }
                if (n >= 3) {
                    float a = has_alpha && n >= 4
                                  ? std::clamp(comp[3], 0.0f, 1.0f) * 255.0f
                                  : 255.0f;
                    return nvgRGBA((unsigned char)comp[0], (unsigned char)comp[1],
                                   (unsigned char)comp[2], (unsigned char)a);
                }
            }
        }
        if (l == "black")  return nvgRGB(0, 0, 0);
        if (l == "white")  return nvgRGB(255, 255, 255);
        if (l == "red")    return nvgRGB(200, 30, 30);
        if (l == "green")  return nvgRGB(30, 140, 50);
        if (l == "blue")   return nvgRGB(40, 80, 200);
        if (l == "yellow") return nvgRGB(190, 170, 20);
        if (l == "orange") return nvgRGB(220, 130, 20);
        if (l == "purple") return nvgRGB(130, 60, 170);
        if (l == "gray" || l == "grey") return nvgRGB(128, 128, 128);
    }
    ok = false;
    return nvgRGB(0, 0, 0);
}

std::string trim_lower(const std::string &in) {
    size_t b = in.find_first_not_of(" \t\r\n");
    size_t e = in.find_last_not_of(" \t\r\n");
    std::string out = (b == std::string::npos) ? "" : in.substr(b, e - b + 1);
    for (char &c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

/* Whitespace-trim without lower-casing — image URLs are case-sensitive,
 * unlike every other CSS value apply_style_attr() deals with. */
std::string trim(const std::string &in) {
    size_t b = in.find_first_not_of(" \t\r\n");
    size_t e = in.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : in.substr(b, e - b + 1);
}

enum class ApplyMode { All, NonImportant, Important };

/* Block/replaced-element metrics from CSS + style="" (not typography). */
struct BoxProps {
    float width_pct     = 0.0f;
    float width_px      = 0.0f;
    float max_width_px  = 0.0f;
    float max_height_px = 0.0f;
    float height_px     = 0.0f;
    float radius_px     = 0.0f;
    /* display:table-cell (CSS-table email layout, e.g. Unlayer's .u-col):
     * siblings sharing a table-cell parent are side-by-side columns even
     * though their width is often px, not the % that MJML uses.  Also
     * marks max_width_px as a per-cell fallback bound rather than a
     * "center this as a column" hint (see build_children). */
    bool  table_cell    = false;
    bool  center        = false;  /* margin: auto / align=center wrapper */
    /* CSS padding on the box (not inherited onto text runs).  Applied as
     * FlexLayout per-axis padding so a chip like Instagram's notification
     * pill (`padding: 8px 16px`) actually insets the heart+"1". */
    float pad_x         = 0.0f;
    float pad_y         = 0.0f;
    /* display:inline-flex / inline-block / inline — shrink-to-content
     * instead of stretching as a block. */
    bool  inline_flex   = false;
    /* vertical-align:middle on a flex/painted box — center children. */
    bool  align_middle  = false;
    /* float:left — email nav bars (PennyMac header-menu LIs). */
    bool  float_left    = false;
    /* CSS `background-image:url(...)` on a plain <div> — common in
     * marketing/notification email (Meta digests use it for every real
     * photo, never <img src>).  Resolved and painted like an <img>. */
    std::string bg_image_url;
};

/* font-size value: Npx / Npt / Nem / N% (em and % are relative to the
 * inherited size). */
void apply_font_size(const std::string &v, float &size) {
    char *end = nullptr;
    float n = std::strtof(v.c_str(), &end);
    if (end == v.c_str() || n <= 0.0f)
        return;
    std::string unit = trim_lower(end);
    if (unit == "pt")      size = n * (96.0f / 72.0f);
    else if (unit == "em") size = size * n;
    else if (unit == "%")  size = size * (n / 100.0f);
    else                   size = n;   // px (or unitless)
    size = std::min(std::max(size, 6.0f), 72.0f);
}

/* CSS length: px / pt / % / unitless.  "auto"/"none" are ignored. */
bool parse_css_len(const std::string &val, float &px, float &pct) {
    px = 0.0f;
    pct = 0.0f;
    if (val.empty() || val == "auto" || val == "none" || val == "initial")
        return false;
    char *end = nullptr;
    float n = std::strtof(val.c_str(), &end);
    if (end == val.c_str())
        return false;
    std::string unit = trim_lower(end);
    if (unit == "%") {
        pct = n;
        return true;
    }
    if (unit == "pt")
        px = n * (96.0f / 72.0f);
    else
        px = n;
    return px > 0.0f || pct > 0.0f;
}

/* Minimal inline style="" support: color, background, font-*, text-*,
 * display, width/max-width/height, padding, vertical-align. */
void apply_style_attr(const char *css, Style &st,
                      TextAlignment &align, bool &has_align,
                      BoxProps *box = nullptr,
                      ApplyMode mode = ApplyMode::All) {
    if (!css || !*css)
        return;
    std::string str = css;
    size_t pos = 0;
    while (pos < str.size()) {
        size_t end = str.find(';', pos);
        std::string decl = str.substr(pos, end == std::string::npos
                                       ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? str.size() : end + 1;

        size_t colon = decl.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = trim_lower(decl.substr(0, colon));
        std::string val = trim_lower(decl.substr(colon + 1));
        if (val.empty())
            continue;
        bool important = false;
        size_t imp = val.rfind("!important");
        if (imp != std::string::npos) {
            important = true;
            val = trim_lower(val.substr(0, imp));
        }
        if (val.empty())
            continue;
        if (mode == ApplyMode::NonImportant && important)
            continue;
        if (mode == ApplyMode::Important && !important)
            continue;
        /* Outlook mso-* is not CSS.  mso-border-alt:none must not clear
         * a real border:1px solid #FCD200 on the same style="" string. */
        if (key.rfind("mso-", 0) == 0 && key != "mso-hide")
            continue;

        if (key == "color") {
            bool ok = false;
            NVGcolor c = parse_html_color(val.c_str(), ok);
            if (ok) st.fgColor = c;
        } else if (key == "background-color" || key == "background") {
            /* Shorthand: take a leading color and ignore the rest. */
            std::string col = val;
            size_t sp = col.find_first_of(" \t");
            if (sp != std::string::npos) col = col.substr(0, sp);
            bool ok = false;
            NVGcolor c = parse_html_color(col.c_str(), ok);
            if (ok) st.bgColor = c;
        } else if (key == "background-image" && box) {
            /* val is lower-cased; re-slice the original declaration so the
             * URL keeps its case (image hosts are case-sensitive). */
            std::string raw = trim(decl.substr(colon + 1));
            size_t up = raw.find("url(");
            if (up != std::string::npos) {
                size_t ustart = up + 4;
                size_t uend = raw.find(')', ustart);
                if (uend != std::string::npos) {
                    std::string url = trim(raw.substr(ustart, uend - ustart));
                    if (!url.empty() && (url.front() == '\'' || url.front() == '"'))
                        url.erase(url.begin());
                    if (!url.empty() && (url.back() == '\'' || url.back() == '"'))
                        url.pop_back();
                    if (!url.empty())
                        box->bg_image_url = url;
                }
            }
        } else if (key == "font-size") {
            apply_font_size(val, st.fontSize);
        } else if (key == "font-weight") {
            if (val == "bold" || val == "bolder" || atoi(val.c_str()) >= 600)
                st.bold = true;
            else if (val == "normal" || val == "lighter" || atoi(val.c_str()) == 400)
                st.bold = false;
        } else if (key == "font-style") {
            if (val == "italic" || val == "oblique")
                st.italic = true;
            else if (val == "normal")
                st.italic = false;
        } else if (key == "text-decoration" ||
                   key == "text-decoration-line") {
            if (val.find("none") != std::string::npos)
                st.underline = false;
            else if (val.find("underline") != std::string::npos)
                st.underline = true;
        } else if (key == "font-family") {
            if (val.find("mono") != std::string::npos ||
                val.find("courier") != std::string::npos ||
                val.find("consol") != std::string::npos ||
                val.find("menlo") != std::string::npos)
                st.monospace = true;
        } else if (key == "display") {
            if (val == "none")
                st.displayNone = true;
            else
                st.displayNone = false;
            if (box && val == "table-cell")
                box->table_cell = true;
            if (box && (val == "inline-flex" || val == "inline-block" ||
                        val == "inline"))
                box->inline_flex = true;
        } else if (key == "mso-hide") {
            if (val == "all")
                st.displayNone = true;
        } else if (key == "text-align") {
            if (val == "center")      { align = TextAlignment::Center;  has_align = true; }
            else if (val == "right")  { align = TextAlignment::Right;   has_align = true; }
            else if (val == "justify"){ align = TextAlignment::Justify; has_align = true; }
            else if (val == "left")   { align = TextAlignment::Left;    has_align = true; }
        } else if (key == "vertical-align") {
            /* "top" on a TD is table layout, not superscript.  Inline
             * spans (price cents) still set it via walk_inline. */
            if (val == "super")
                st.superscript = true;
            else if (val == "baseline" || val == "middle" || val == "bottom")
                st.superscript = false;
            if (box && (val == "middle" || val == "center"))
                box->align_middle = true;
        } else if (key == "float" && box) {
            box->float_left = (val == "left");
        } else if (key == "padding" || key == "padding-left" ||
                   key == "padding-right" || key == "padding-top" ||
                   key == "padding-bottom") {
            /* "6px 16px" → padY, padX.  Single value applies to both. */
            float px = 0, pct = 0;
            size_t sp = val.find_first_of(" \t");
            std::string a = (sp == std::string::npos) ? val : val.substr(0, sp);
            std::string b = (sp == std::string::npos) ? val
                            : trim_lower(val.substr(sp));
            size_t sp2 = b.find_first_of(" \t");
            if (sp2 != std::string::npos) b = b.substr(0, sp2);
            if (parse_css_len(a, px, pct) && pct == 0.0f) {
                if (key == "padding" || key == "padding-top" ||
                    key == "padding-bottom") {
                    st.padY = std::max(st.padY, px);
                    if (box) box->pad_y = std::max(box->pad_y, px);
                }
                if (key == "padding" || key == "padding-left" ||
                    key == "padding-right") {
                    st.padX = std::max(st.padX, px);
                    if (box) box->pad_x = std::max(box->pad_x, px);
                }
            }
            if (key == "padding" && b != a &&
                parse_css_len(b, px, pct) && pct == 0.0f) {
                st.padX = std::max(st.padX, px);
                if (box) box->pad_x = std::max(box->pad_x, px);
            }
        } else if (box && (key == "width" || key == "max-width" ||
                           key == "height" || key == "max-height")) {
            float px = 0, pct = 0;
            if (!parse_css_len(val, px, pct))
                continue;
            if (key == "width") {
                if (pct > 0.0f) box->width_pct = pct;
                else            box->width_px  = px;
            } else if (key == "max-width") {
                if (pct <= 0.0f) box->max_width_px = px;
                else if (box->width_pct <= 0.0f) box->width_pct = pct;
            } else if (key == "height") {
                if (pct <= 0.0f) box->height_px = px;
            } else if (key == "max-height") {
                if (pct <= 0.0f) box->max_height_px = px;
            }
        } else if (key == "border" || key == "border-width" ||
                   key == "border-color" || key == "border-style") {
            if (val == "none" || val == "0" || val == "0px" ||
                val.find("none") == 0) {
                if (key != "border-color")
                    st.borderWidth = 0.0f;
                continue;
            }
            std::string rest = val;
            while (!rest.empty()) {
                while (!rest.empty() && std::isspace((unsigned char)rest[0]))
                    rest.erase(0, 1);
                if (rest.empty()) break;
                size_t sp = rest.find_first_of(" \t");
                std::string tok = (sp == std::string::npos) ? rest
                                  : rest.substr(0, sp);
                rest = (sp == std::string::npos) ? "" : rest.substr(sp);
                if (tok == "solid" || tok == "dashed" || tok == "dotted" ||
                    tok == "double" || tok == "groove" || tok == "ridge")
                    continue;
                float px = 0, pct = 0;
                if (parse_css_len(tok, px, pct) && pct == 0.0f) {
                    st.borderWidth = px;
                    continue;
                }
                bool ok = false;
                NVGcolor c = parse_html_color(tok.c_str(), ok);
                if (ok) {
                    st.borderColor = c;
                    if (st.borderWidth <= 0.0f && key == "border-color")
                        st.borderWidth = 1.0f;
                }
            }
        } else if (box && key == "border-radius") {
            float px = 0, pct = 0;
            size_t sp = val.find_first_of(" \t");
            std::string a = (sp == std::string::npos) ? val : val.substr(0, sp);
            if (parse_css_len(a, px, pct) && pct == 0.0f)
                box->radius_px = px;
        } else if (box && (key == "margin" || key == "margin-left" ||
                           key == "margin-right")) {
            if (val.find("auto") != std::string::npos)
                box->center = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Flow: accumulates inline content into one Document (one HtmlText leaf).
// ---------------------------------------------------------------------------
struct Flow {
    Document       doc;
    Paragraph     *cur = nullptr;
    bool           cur_has_text = false;
    bool           pre = false;
    TextAlignment  align = TextAlignment::Left;

    Paragraph *para() {
        if (!cur) {
            cur = doc.addParagraph();
            cur->alignment = align;
            cur_has_text = false;
        }
        return cur;
    }
    /* End the current block: the next text starts a new paragraph. */
    void brk() { cur = nullptr; cur_has_text = false; }

    /* Inline image run (a small icon next to text) — see Text::image. */
    void emitImage(int image, float w, float h, std::string src) {
        Text t;
        t.isImageRun = true;
        t.image      = image;  // resolved id; commonly 0 (not loaded yet)
        t.image_w    = w;
        t.image_h    = h;
        t.image_src  = std::move(src);
        para()->addText(t);
        cur_has_text = true;
    }

    void emit(const std::string &raw, const Style &st) {
        std::string t;
        if (pre) {
            t = raw;
        } else {
            bool ws = false;
            for (char c : raw) {
                if (std::isspace((unsigned char)c)) {
                    if (!ws) t += ' ';
                    ws = true;
                } else {
                    t += c;
                    ws = false;
                }
            }
            /* No leading space at the start of a paragraph. */
            if (!cur_has_text && !t.empty() && t.front() == ' ')
                t.erase(0, 1);
        }
        if (t.empty()) return;
        para()->addText(t, st);
        cur_has_text = true;
    }

    /* Anything worth turning into a widget? (whitespace-only runs between
     * block elements must not create empty leaves.) */
    bool has_content() const {
        for (const auto &p : doc.paragraphs) {
            if (p->isRule || p->isImage)
                return true;
            for (const Text &r : p->runs) {
                if (r.isImageRun)
                    return true;
                for (char c : r.content)
                    if (!std::isspace((unsigned char)c))
                        return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// HtmlText: leaf widget rendering one Document; HtmlBlock: container widget
// with an optional background fill.
// ---------------------------------------------------------------------------
class HtmlText : public Widget {
public:
    Document m_doc;
    NVGcolor m_bg;
    int      m_measured_h = 0;
    int      m_measured_w = -1;
    int      m_natural_w  = -1;  // cached max-content width

    HtmlText(Widget *parent, Document &&doc, NVGcolor bg)
        : Widget(parent), m_doc(std::move(doc)), m_bg(bg) {
        set_live(true);
    }

    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        /* Cap at the parent, not at our last laid-out m_size: using
         * m_size ratchets a shrink-wrapped chip (heart+"1") down until
         * the icon and text wrap onto two lines. */
        int parent_w = (parent() && parent()->size().x() > 10)
                           ? parent()->size().x() : 0;
        int cap = parent_w > 0 ? parent_w : 600;
        HtmlText *self = const_cast<HtmlText *>(this);
        /* Returning 0 for stretched leaves collapsed Amazon's 50/50
         * wish-list grid: those cells sit under AlignItems::Center
         * (max-width + margin:auto) and need a real max-content width
         * so the row doesn't shrink to padding. Cache it so resize
         * does not re-run nvgTextBounds on every leaf. */
        if (self->m_natural_w < 0) {
            int n = ctx ? (int)std::ceil(self->m_doc.measure_natural_width(ctx)) : 0;
            if (n > 0)
                n += 4;
            self->m_natural_w = std::max(n, 1);
        }
        bool stretch_col = false;
        if (parent()) {
            if (auto *fl = dynamic_cast<const FlexLayout *>(parent()->layout())) {
                FlexDirection d = fl->direction();
                stretch_col = (d == FlexDirection::Column ||
                               d == FlexDirection::ColumnReverse) &&
                              fl->align_items() == AlignItems::Stretch;
            }
        }
        int w = (stretch_col && parent_w > 0)
                    ? cap
                    : std::min(std::max(self->m_natural_w, 10), cap);
        self->measure(ctx, w);
        return Vector2i(std::min(self->m_natural_w, cap),
                        std::max(m_measured_h, 1));
    }

    /* Lay out at width w without painting (Document::layout_only suppresses
     * all paint calls; issuing nvg paint here — typically outside an NVG
     * frame — would leak packets into the next frame's draw list). */
    void measure(NVGcontext *ctx, int w) {
        w = std::max(w, 10);
        if (w == m_measured_w && m_measured_h > 0 && !m_doc.layoutDirty())
            return;
        m_doc.contentWidth = (float)w;
        m_doc.markLayoutDirty();
        m_doc.layout_only = true;
        m_doc.draw(ctx, 0.f, 0.f);
        m_doc.layout_only = false;
        m_measured_h = (int)std::ceil(m_doc.last_drawn_height);
        m_measured_w = w;
    }

    virtual void draw(NVGcontext *ctx) override {
        if (nvgIsRecordingDisplayList(ctx))
            return;
        float x = (float)m_pos.x(), y = (float)m_pos.y();
        if (debug_draw()) {
            std::string t;
            for (const auto &p : m_doc.paragraphs) { t += p->plain_text(); if (t.size() > 30) break; }
            trace_draw("HtmlText", this, t.substr(0, 30).c_str());
        }
        if (m_bg.a > 0.0f) {
            nvgBeginPath(ctx);
            nvgRect(ctx, x, y, (float)m_size.x(), (float)m_size.y());
            nvgFillColor(ctx, m_bg);
            nvgFill(ctx);
        }
        int w = std::max(m_size.x(), 10);
        if (w != m_measured_w)
            m_doc.markLayoutDirty();
        m_doc.contentWidth = (float)w;
        m_doc.draw(ctx, x, y);
        int h = (int)std::ceil(m_doc.last_drawn_height);
        if (w != m_measured_w || h != m_measured_h) {
            /* Our reported preferred height was stale: ask the enclosing
             * HtmlDocument for one clean relayout next frame instead of
             * re-entering layout mid-paint. */
            m_measured_w = w;
            m_measured_h = h;
            for (Widget *p = parent(); p; p = p->parent())
                if (auto *hd = dynamic_cast<HtmlDocument *>(p)) {
                    hd->request_reflow();
                    break;
                }
        }
    }
};

class HtmlBlock : public Widget {
public:
    NVGcolor m_bg = NVGcolor{ { { 0.f, 0.f, 0.f, 0.f } } };
    float    m_radius = 0.0f;
    /* CSS background-image (see BoxProps::bg_image_url).  m_bg_image is
     * an nvg image handle, 0 until resolved; m_bg_image_src lets
     * HtmlDocument::bind_loaded_images() re-resolve it once bytes land. */
    int         m_bg_image   = 0;
    float       m_bg_image_w = 0.0f;
    float       m_bg_image_h = 0.0f;
    std::string m_bg_image_src;

    explicit HtmlBlock(Widget *parent) : Widget(parent) { set_live(true); }

    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        Vector2i p = Widget::preferred_size(ctx);
        if (m_max_size.x() > 0) {
            /* Block boxes with max-width want to fill that width (CSS
             * width:100%; max-width:N).  HtmlDocument::preferred_size
             * still reports 0 so the split can shrink. */
            p.x() = m_max_size.x();
        }
        return p;
    }

    virtual void draw(NVGcontext *ctx) override {
        if (nvgIsRecordingDisplayList(ctx))
            return;
        trace_draw("HtmlBlock", this);
        /* Identity-position wrappers (the NMAIL_DEBUG_DRAW (0,0) chain) must
         * not nvgSave: each save is a stack slot, and overflowing pops the
         * ScrollPanel scissor/transform so later cells paint at window x=0. */
        const bool shifted = m_pos.x() != 0 || m_pos.y() != 0;
        if (shifted) {
            int depth0 = nvgStateDepth(ctx);
            nvgSave(ctx);
            if (nvgStateDepth(ctx) <= depth0)
                return;
        }
        if (m_bg.a > 0.0f || m_bg_image > 0) {
            float x = (float)m_pos.x(), y = (float)m_pos.y();
            float w = (float)m_size.x(), h = (float)m_size.y();
            float rad = m_radius;
            if (rad > 0.5f)
                rad = std::min(rad, std::min(w, h) * 0.5f);
            if (m_bg.a > 0.0f) {
                nvgBeginPath(ctx);
                if (rad > 0.5f)
                    nvgRoundedRect(ctx, x, y, w, h, rad);
                else
                    nvgRect(ctx, x, y, w, h);
                nvgFillColor(ctx, m_bg);
                nvgFill(ctx);
            }
            if (m_bg_image > 0 && w > 0.0f && h > 0.0f) {
                /* CSS background-size:cover: uniform scale so the image
                 * fills the box on both axes, centered and cropped. */
                float iw = m_bg_image_w > 0.0f ? m_bg_image_w : w;
                float ih = m_bg_image_h > 0.0f ? m_bg_image_h : h;
                float scale = std::max(w / iw, h / ih);
                float dw = iw * scale, dh = ih * scale;
                float ox = x + (w - dw) * 0.5f;
                float oy = y + (h - dh) * 0.5f;
                NVGpaint paint = nvgImagePattern(ctx, ox, oy, dw, dh, 0.0f,
                                                  m_bg_image, 1.0f);
                nvgBeginPath(ctx);
                if (rad > 0.5f)
                    nvgRoundedRect(ctx, x, y, w, h, rad);
                else
                    nvgRect(ctx, x, y, w, h);
                nvgFillPaint(ctx, paint);
                nvgFill(ctx);
            }
        }
        if (shifted)
            nvgTranslate(ctx, (float)m_pos.x(), (float)m_pos.y());
        for (Widget *c : children()) {
            if (c->visible())
                c->draw(ctx);
        }
        if (shifted)
            nvgRestore(ctx);
    }
};

// ---------------------------------------------------------------------------
// Builder: shared render state for one set_html() pass.
// ---------------------------------------------------------------------------
enum class CssMedia { Any, Dark, Light, Skip };

struct CssSel {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::string ancestor_class; /* ".foo bar" — some parent has class foo */
    std::string ancestor_tag;
    int spec = 0;
};

struct CssRule {
    CssSel      sel;
    std::string decls;
    int         order = 0;
    CssMedia    media = CssMedia::Any;
};

struct Stylesheet {
    std::vector<CssRule> rules;
};

struct Builder {
    HtmlDocument *view;
    NVGcolor      accent;
    NVGcolor      meta;
    NVGcolor      code_bg;
    bool          dark = false;
    Stylesheet    sheet;
    /* Nested <table> without width=100% shrinks to content (CTA chips).
     * Flattened 1-cell TDs must not stretch a bgcolor across the column. */
    bool          in_full_width = true;
    /* Column flex-grow weights for the current table, taken from the first
     * multi-cell row (HTML width="N%" or CSS width:N%).  Later rows reuse
     * them so a Benchmark-style grid lines up instead of each <tr> sizing
     * itself from that row's text. Nested tables save/restore this. */
    std::vector<float> table_col_grow;
};

const char *attr(GumboElement *el, const char *name) {
    GumboAttribute *a = gumbo_get_attribute(&el->attributes, name);
    return (a && a->value) ? a->value : nullptr;
}

// ---------------------------------------------------------------------------
// Stylesheet: <style> blocks from the mail (class / id / element selectors,
// prefers-color-scheme media). Inline style="" still wins.
// ---------------------------------------------------------------------------
void collect_style_text(GumboNode *node, std::string &out) {
    if (!node) return;
    if (node->type == GUMBO_NODE_DOCUMENT) {
        GumboVector *kids = &node->v.document.children;
        for (unsigned i = 0; i < kids->length; ++i)
            collect_style_text((GumboNode *)kids->data[i], out);
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) return;
    GumboElement *el = &node->v.element;
    if (el->tag == GUMBO_TAG_STYLE) {
        for (unsigned i = 0; i < el->children.length; ++i) {
            GumboNode *c = (GumboNode *)el->children.data[i];
            if (c->type == GUMBO_NODE_TEXT || c->type == GUMBO_NODE_CDATA)
                out.append(c->v.text.text);
        }
        out += '\n';
        return;
    }
    for (unsigned i = 0; i < el->children.length; ++i)
        collect_style_text((GumboNode *)el->children.data[i], out);
}

std::string strip_css_comments(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        if (i + 1 < in.size() && in[i] == '/' && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/'))
                ++i;
            if (i + 1 < in.size()) i += 2;
            out += ' ';
        } else {
            out += in[i++];
        }
    }
    return out;
}

size_t css_match_brace(const std::string &s, size_t open) {
    int d = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') ++d;
        else if (s[i] == '}') {
            if (--d == 0) return i;
        }
    }
    return std::string::npos;
}

void css_skip_ws(const std::string &s, size_t &i, size_t end) {
    while (i < end && std::isspace((unsigned char)s[i])) ++i;
}

bool parse_simple_selector(const std::string &raw, CssSel &out) {
    std::string s = trim_lower(raw);
    if (s.empty()) return false;
    /* One descendant step: ".st-LinkTextBlock a" */
    size_t sp = s.find_first_of(" \t");
    if (sp != std::string::npos && s.find_first_of(">+~[:") == std::string::npos) {
        std::string anc = trim_lower(s.substr(0, sp));
        std::string rest = trim_lower(s.substr(sp));
        if (rest.find_first_of(" \t") != std::string::npos)
            return false;
        CssSel a, b;
        if (!parse_simple_selector(anc, a) || !parse_simple_selector(rest, b))
            return false;
        out = b;
        if (!a.classes.empty())
            out.ancestor_class = a.classes[0];
        else if (!a.tag.empty())
            out.ancestor_tag = a.tag;
        else
            return false;
        out.spec += 10;
        return true;
    }
    if (s.find_first_of(" >+~[:") != std::string::npos)
        return false;
    out = CssSel{};
    size_t i = 0;
    if (s[0] != '.' && s[0] != '#') {
        size_t j = i;
        while (j < s.size() && (std::isalnum((unsigned char)s[j]) ||
                                s[j] == '-' || s[j] == '_'))
            ++j;
        out.tag = s.substr(0, j);
        if (out.tag == "*") out.tag.clear();
        i = j;
    }
    while (i < s.size()) {
        if (s[i] == '.' || s[i] == '#') {
            char kind = s[i++];
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) ||
                                    s[j] == '-' || s[j] == '_'))
                ++j;
            if (j == i) return false;
            std::string ident = s.substr(i, j - i);
            if (kind == '.') out.classes.push_back(ident);
            else            out.id = ident;
            i = j;
        } else {
            return false;
        }
    }
    if (out.tag.empty() && out.id.empty() && out.classes.empty())
        return false;
    out.spec = (out.id.empty() ? 0 : 100) +
               (int)out.classes.size() * 10 +
               (out.tag.empty() ? 0 : 1);
    return true;
}

CssMedia parse_css_media(const std::string &q) {
    std::string l = trim_lower(q);
    if (l.find("prefers-color-scheme") != std::string::npos) {
        if (l.find("dark") != std::string::npos)  return CssMedia::Dark;
        if (l.find("light") != std::string::npos) return CssMedia::Light;
    }
    /* Email pane is treated as a desktop-width reader, matching Firefox
     * at a normal window size — skip phone-only breakpoints.  Real
     * templates commonly use 600/620/640/650 as their "mobile" cutoff
     * (not just 480), so the assumed viewport needs headroom past 600
     * or their max-width query stays applied (forcing single-column
     * mobile layout) while the matching min-width query — which carries
     * the real desktop column widths — gets skipped instead. */
    constexpr float kAssumedViewportPx = 660.0f;
    /* Hardware device width, not the pane.  PennyMac's
     * `@media (max-device-width: 768px) { #main_container { width:375px } }`
     * must not match here — Firefox on a PC ignores it, and applying it
     * locked the whole letter to 375px while inner 600px wrappers
     * overflowed (negative x).  Probe the longer keys first. */
    constexpr float kAssumedDevicePx = 1280.0f;
    auto media_px = [&](const char *key) -> float {
        size_t p = l.find(key);
        if (p == std::string::npos) return -1.0f;
        p += std::strlen(key);
        while (p < l.size() && (l[p] == ':' || std::isspace((unsigned char)l[p])))
            ++p;
        char *end = nullptr;
        float n = std::strtof(l.c_str() + p, &end);
        return (end == l.c_str() + p) ? -1.0f : n;
    };
    float maxdw = media_px("max-device-width");
    if (maxdw > 0.0f && maxdw < kAssumedDevicePx)
        return CssMedia::Skip;
    float mindw = media_px("min-device-width");
    if (mindw > kAssumedDevicePx)
        return CssMedia::Skip;
    float maxw = media_px("max-width");
    if (maxw > 0.0f && maxw < kAssumedViewportPx)
        return CssMedia::Skip;
    float minw = media_px("min-width");
    if (minw > kAssumedViewportPx)
        return CssMedia::Skip;
    return CssMedia::Any;
}

void parse_css_rules(const std::string &s, size_t &i, size_t end,
                     CssMedia media, Stylesheet &ss, int &order);

void parse_css_at_rule(const std::string &s, size_t &i, size_t end,
                       Stylesheet &ss, int &order) {
    size_t start = i;
    while (i < end && s[i] != '{' && s[i] != ';') ++i;
    std::string head = s.substr(start, i - start);
    if (i >= end) return;
    if (s[i] == ';') { ++i; return; }
    size_t close = css_match_brace(s, i);
    if (close == std::string::npos) { i = end; return; }
    std::string q = trim_lower(head);
    if (q.rfind("@media", 0) == 0) {
        CssMedia inner = parse_css_media(head);
        if (inner != CssMedia::Skip) {
            size_t j = i + 1;
            parse_css_rules(s, j, close, inner, ss, order);
        }
    }
    i = close + 1;
}

void parse_css_rules(const std::string &s, size_t &i, size_t end,
                     CssMedia media, Stylesheet &ss, int &order) {
    while (i < end) {
        css_skip_ws(s, i, end);
        if (i >= end) break;
        if (s[i] == '}') { ++i; break; }
        if (s[i] == '@') {
            parse_css_at_rule(s, i, end, ss, order);
            continue;
        }
        size_t sel_b = i;
        while (i < end && s[i] != '{') ++i;
        if (i >= end) break;
        std::string selectors = s.substr(sel_b, i - sel_b);
        size_t close = css_match_brace(s, i);
        if (close == std::string::npos) { i = end; break; }
        std::string decls = s.substr(i + 1, close - (i + 1));
        i = close + 1;
        size_t p = 0;
        while (p < selectors.size()) {
            size_t c = selectors.find(',', p);
            std::string one = selectors.substr(
                p, c == std::string::npos ? std::string::npos : c - p);
            p = (c == std::string::npos) ? selectors.size() : c + 1;
            CssSel sel;
            if (!parse_simple_selector(one, sel))
                continue;
            CssRule r;
            r.sel = std::move(sel);
            r.decls = decls;
            r.order = order++;
            r.media = media;
            ss.rules.push_back(std::move(r));
        }
    }
}

void parse_stylesheet(const std::string &css, Stylesheet &ss) {
    std::string s = strip_css_comments(css);
    size_t i = 0;
    int order = 0;
    parse_css_rules(s, i, s.size(), CssMedia::Any, ss, order);
}

std::string elem_tag_name(GumboElement *el) {
    const char *n = gumbo_normalized_tagname(el->tag);
    if (n && *n) return trim_lower(n);
    return {};
}

std::vector<std::string> elem_classes(GumboElement *el) {
    std::vector<std::string> out;
    const char *c = attr(el, "class");
    if (!c) return out;
    std::string s = trim_lower(c);
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool css_sel_matches(const CssSel &sel, GumboElement *el, GumboNode *node) {
    if (!sel.tag.empty() && sel.tag != elem_tag_name(el))
        return false;
    if (!sel.id.empty()) {
        const char *id = attr(el, "id");
        if (!id || trim_lower(id) != sel.id)
            return false;
    }
    if (!sel.classes.empty()) {
        auto have = elem_classes(el);
        for (const auto &need : sel.classes) {
            if (std::find(have.begin(), have.end(), need) == have.end())
                return false;
        }
    }
    if (!sel.ancestor_class.empty() || !sel.ancestor_tag.empty()) {
        bool ok = false;
        for (GumboNode *p = node ? node->parent : nullptr; p; p = p->parent) {
            if (p->type != GUMBO_NODE_ELEMENT) continue;
            GumboElement *pe = &p->v.element;
            if (!sel.ancestor_class.empty()) {
                auto have = elem_classes(pe);
                if (std::find(have.begin(), have.end(), sel.ancestor_class)
                        != have.end()) {
                    ok = true;
                    break;
                }
            }
            if (!sel.ancestor_tag.empty() &&
                elem_tag_name(pe) == sel.ancestor_tag) {
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }
    return true;
}

void apply_css(GumboElement *el, Style &st, TextAlignment &align,
               const Stylesheet &sheet, bool dark, BoxProps *box,
               ApplyMode mode, GumboNode *node = nullptr) {
    struct Hit { int spec, order; const CssRule *r; };
    std::vector<Hit> hits;
    for (const auto &r : sheet.rules) {
        if (r.media == CssMedia::Skip) continue;
        if (r.media == CssMedia::Dark && !dark) continue;
        if (r.media == CssMedia::Light && dark) continue;
        if (!css_sel_matches(r.sel, el, node)) continue;
        hits.push_back({ r.sel.spec, r.order, &r });
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.spec != b.spec) return a.spec < b.spec;
        return a.order < b.order;
    });
    for (const Hit &h : hits) {
        bool dummy = false;
        apply_style_attr(h.r->decls.c_str(), st, align, dummy, box, mode);
    }
}

/* padding / border / background are not inherited.  They paint on the
 * widget (HtmlBlock) or on the inline run that set them.  Copying a
 * TD's padY onto every descendant run made 63px lines and clipped the
 * Learn More pill (fill was recorded only on a later paint that the
 * fast path then skipped). */
void reset_box_style(Style &st) {
    st.padX = 0.f;
    st.padY = 0.f;
    st.borderWidth = 0.f;
    st.bgColor = nvgRGBA(0, 0, 0, 0);
    st.borderColor = nvgRGBA(0, 0, 0, 0);
}

/* Non-important stylesheet, presentational, non-important inline,
 * then !important stylesheet, then !important inline — so MJML's
 * `.mj-column-per-33 { width:33% !important }` beats inline width:100%. */
void apply_cascade(GumboElement *el, Style &st, TextAlignment &align,
                   const Builder &B, BoxProps *box = nullptr,
                   GumboNode *node = nullptr) {
    apply_css(el, st, align, B.sheet, B.dark, box, ApplyMode::NonImportant, node);
    const char *align_attr = attr(el, "align");
    if (align_attr) {
        bool dummy = true;
        apply_style_attr(("text-align:" + trim_lower(align_attr)).c_str(),
                         st, align, dummy, box, ApplyMode::NonImportant);
    }
    const char *style_attr = attr(el, "style");
    if (style_attr) {
        bool has_align = false;
        apply_style_attr(style_attr, st, align, has_align, box,
                         ApplyMode::NonImportant);
    }
    apply_css(el, st, align, B.sheet, B.dark, box, ApplyMode::Important, node);
    if (style_attr) {
        bool has_align = false;
        apply_style_attr(style_attr, st, align, has_align, box,
                         ApplyMode::Important);
    }
}

/* Column weight for grouping consecutive siblings into one flex row, or
 * 0 if `el` isn't a column at all:
 *  - MJML/CSS `width:N%` with N<100 — weight is the percentage.
 *  - `.mj-column-per-NN` class — weight is NN.
 *  - `display:table-cell` (CSS-table email layout, e.g. Unlayer's
 *    `.u-col-*`) — weight is the cascaded pixel width, which is usually
 *    set !important by a `@media (min-width:...)` block and can exceed
 *    99, unlike the percentage cases above.  A missing width still
 *    counts as a column (equal share) since the display alone is a
 *    deliberate side-by-side signal. */
float flex_column_pct(GumboElement *el, const Builder &B) {
    BoxProps box;
    Style dummy;
    TextAlignment a = TextAlignment::Left;
    apply_cascade(el, dummy, a, B, &box);
    if (box.width_pct > 0.0f && box.width_pct < 99.0f)
        return box.width_pct;
    if (box.table_cell)
        return box.width_px > 0.0f ? box.width_px : 1.0f;
    const char *cls = attr(el, "class");
    if (cls) {
        std::string s = trim_lower(cls);
        size_t p = s.find("mj-column-per-");
        if (p != std::string::npos) {
            int n = atoi(s.c_str() + p + 14);
            if (n > 0 && n < 100)
                return (float)n;
        }
    }
    return 0.0f;
}

/* PennyMac-style header nav: <ul><li style="float:left; width:N%">.
 * Two or more floated or %-width items belong on one row, not as a
 * vertical bullet list. */
bool list_is_nav_row(GumboElement *ul, const Builder &B) {
    int n = 0, side = 0;
    for (unsigned i = 0; i < ul->children.length; ++i) {
        GumboNode *cn = (GumboNode *)ul->children.data[i];
        if (cn->type != GUMBO_NODE_ELEMENT) continue;
        if (cn->v.element.tag != GUMBO_TAG_LI) continue;
        Style dummy;
        TextAlignment a = TextAlignment::Left;
        BoxProps box;
        apply_cascade(&cn->v.element, dummy, a, B, &box);
        if (dummy.displayNone) continue;
        ++n;
        if (box.float_left || (box.width_pct > 0.0f && box.width_pct < 99.0f))
            ++side;
    }
    return n >= 2 && side >= 2;
}

/* Marketing emails commonly ship each image twice: an Outlook-only copy
 * inside a plain "<!--[if mso]>...<![endif]-->" comment (invisible to
 * every real HTML parser, since a standard comment ends at the first
 * "-->") carrying real width/height, paired with a "downlevel-revealed"
 * "<!--[if !mso]><!-->...<!--<![endif]-->" wrapper around the <img> that
 * actually renders — whose own width/height rely on `height:auto` plus
 * the browser fetching the image to learn its natural size.  Mining the
 * mso-only copy's attributes as a size hint lets layout reserve the
 * right space before (or without) fetching the real bytes. */
bool mso_comment_img_size(const std::string &comment, float &w, float &h) {
    if (comment.find("mso") == std::string::npos)
        return false;
    size_t img = comment.find("<img");
    if (img == std::string::npos)
        return false;
    size_t tag_end = comment.find('>', img);
    std::string tag = comment.substr(img, (tag_end == std::string::npos
                                            ? comment.size() : tag_end) - img);
    auto find_num = [&](const char *key) -> float {
        size_t p = tag.find(key);
        if (p == std::string::npos) return 0.0f;
        p += std::strlen(key);
        while (p < tag.size() && (tag[p] == '"' || tag[p] == '\''))
            ++p;
        return strtof(tag.c_str() + p, nullptr);
    };
    w = find_num("width=");
    h = find_num("height=");
    return w > 0.0f && h > 0.0f;
}

/* Scan the (few) siblings immediately before `node` for the mso-only
 * comment described above.  The downlevel-reveal trick puts an empty
 * "[if !mso]><!" comment directly before the real <img>, so the actual
 * "[if mso]" comment sits one step further back. */
bool mso_size_hint(GumboNode *node, float &w, float &h) {
    GumboNode *parent = node->parent;
    if (!parent || parent->type != GUMBO_NODE_ELEMENT)
        return false;
    GumboVector *kids = &parent->v.element.children;
    size_t idx = node->index_within_parent;
    for (size_t back = 1; back <= 3 && back <= idx; ++back) {
        GumboNode *sib = (GumboNode *)kids->data[idx - back];
        if (sib->type == GUMBO_NODE_WHITESPACE)
            continue;
        if (sib->type != GUMBO_NODE_COMMENT)
            break;
        if (mso_comment_img_size(sib->v.text.text, w, h))
            return true;
    }
    return false;
}

/* hint_w/hint_h: intrinsic size mined from a paired Outlook-only
 * "[if mso]" comment (see mso_comment_img_size) when the real <img> only
 * gives a width and leans on `height:auto` + the browser's own fetch to
 * learn the rest.  Used only as a last resort, below the real attributes
 * and any loaded pixels. */
void size_html_image(GumboElement *el, const Builder &B,
                     const HtmlImageInfo &ri, float &pw, float &ph,
                     float hint_w = 0.0f, float hint_h = 0.0f) {
    BoxProps box;
    Style dummy;
    TextAlignment a = TextAlignment::Left;
    apply_cascade(el, dummy, a, B, &box);

    const char *wattr = attr(el, "width");
    const char *hattr = attr(el, "height");
    float aw = wattr ? strtof(wattr, nullptr) : 0.0f;
    float ah = hattr ? strtof(hattr, nullptr) : 0.0f;
    if (box.width_px > 0.0f)  aw = box.width_px;
    if (box.height_px > 0.0f) ah = box.height_px;

    float aspect = 0.0f;
    if (ri.w > 0.0f && ri.h > 0.0f)
        aspect = ri.h / ri.w;
    else if (aw > 0.0f && ah > 0.0f)
        aspect = ah / aw;
    else if (hint_w > 0.0f && hint_h > 0.0f)
        aspect = hint_h / hint_w;

    float maxw = box.max_width_px;
    float maxh = box.max_height_px;
    if (aw > 0.0f && maxw > 0.0f && aw > maxw) {
        if (aspect > 0.0f) ah = maxw * aspect;
        aw = maxw;
    }
    if (ah > 0.0f && maxh > 0.0f && ah > maxh) {
        if (aspect > 0.0f) aw = maxh / aspect;
        ah = maxh;
    }
    if (aw <= 0.0f && ah > 0.0f && aspect > 0.0f)
        aw = ah / aspect;
    if (ah <= 0.0f && aw > 0.0f && aspect > 0.0f)
        ah = aw * aspect;

    if (aw <= 0.0f && ah <= 0.0f) {
        if (ri.w > 0.0f && ri.h > 0.0f) {
            aw = ri.w;
            ah = ri.h;
            if (aw > 600.0f) {
                ah *= 600.0f / aw;
                aw = 600.0f;
            }
        } else if (hint_w > 0.0f && hint_h > 0.0f) {
            aw = hint_w;
            ah = hint_h;
            if (aw > 600.0f) {
                ah *= 600.0f / aw;
                aw = 600.0f;
            }
        } else {
            aw = 160.0f;
            ah = 100.0f;
        }
    }
    pw = aw;
    ph = ah;
}

bool element_is_hidden(GumboElement *el, const Builder &B) {
    Style tmp;
    TextAlignment a = TextAlignment::Left;
    apply_cascade(el, tmp, a, B);
    return tmp.displayNone;
}

void build_children(Widget *container, GumboVector *kids, Style st,
                    Builder &B, int list_depth, TextAlignment align);
void walk_inline(GumboNode *node, Style st, Flow &F, Builder &B,
                 int list_depth);

void walk_inline_children(GumboVector *kids, Style st, Flow &F, Builder &B,
                          int list_depth) {
    for (unsigned i = 0; i < kids->length; ++i)
        walk_inline((GumboNode *)kids->data[i], st, F, B, list_depth);
}

/* Tags whose subtree is a real container widget (handled in
 * build_children, never inside a text flow). */
bool is_container_tag(GumboTag t) {
    switch (t) {
    case GUMBO_TAG_DIV:     case GUMBO_TAG_SECTION: case GUMBO_TAG_ARTICLE:
    case GUMBO_TAG_HEADER:  case GUMBO_TAG_FOOTER:  case GUMBO_TAG_MAIN:
    case GUMBO_TAG_ASIDE:   case GUMBO_TAG_NAV:     case GUMBO_TAG_CENTER:
    case GUMBO_TAG_FIGURE:  case GUMBO_TAG_FIGCAPTION:
    case GUMBO_TAG_FIELDSET: case GUMBO_TAG_FORM:   case GUMBO_TAG_DL:
    case GUMBO_TAG_TABLE:   case GUMBO_TAG_TBODY:   case GUMBO_TAG_THEAD:
    case GUMBO_TAG_TFOOT:   case GUMBO_TAG_TR:      case GUMBO_TAG_TD:
    case GUMBO_TAG_TH:      case GUMBO_TAG_CAPTION:
    case GUMBO_TAG_HTML:    case GUMBO_TAG_BODY:
    case GUMBO_TAG_UL:      case GUMBO_TAG_OL:
        return true;
    default:
        return false;
    }
}

bool is_skipped_tag(GumboTag t) {
    switch (t) {
    case GUMBO_TAG_SCRIPT:   case GUMBO_TAG_STYLE:  case GUMBO_TAG_HEAD:
    case GUMBO_TAG_TITLE:    case GUMBO_TAG_NOSCRIPT: case GUMBO_TAG_TEMPLATE:
    case GUMBO_TAG_IFRAME:   case GUMBO_TAG_OBJECT: case GUMBO_TAG_EMBED:
    case GUMBO_TAG_APPLET:   case GUMBO_TAG_BASE:   case GUMBO_TAG_LINK:
    case GUMBO_TAG_META:
        return true;
    default:
        return false;
    }
}

/* `<a>` is normally inline (a run of styled/underlined text handled by
 * walk_inline), but HTML5 also allows it to wrap block content — email
 * templates use exactly that to make a whole photo tile clickable
 * (`<a href=..><div style="width:...;background-image:..."></div></a>`).
 * walk_inline has no Widget to hang a box on, so a div nested that way
 * would silently lose its size/background and get flattened into the
 * surrounding text flow (only its own inline children survive).  Detect
 * that shape so build_children can route it like a <div> instead. */
bool element_has_block_child(GumboElement *el) {
    for (unsigned i = 0; i < el->children.length; ++i) {
        GumboNode *cn = (GumboNode *)el->children.data[i];
        if (cn->type != GUMBO_NODE_ELEMENT)
            continue;
        GumboTag ct = cn->v.element.tag;
        if (is_container_tag(ct) && !is_skipped_tag(ct))
            return true;
    }
    return false;
}

void walk_inline(GumboNode *node, Style st, Flow &F, Builder &B,
                 int list_depth) {
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_CDATA) {
        F.emit(node->v.text.text, st);
        return;
    }
    if (node->type == GUMBO_NODE_WHITESPACE) {
        F.emit(" ", st);
        return;
    }
    if (node->type == GUMBO_NODE_DOCUMENT) {
        walk_inline_children(&node->v.document.children, st, F, B, list_depth);
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT)
        return;   // comments, doctypes

    GumboElement *el = &node->v.element;
    GumboTag tag = el->tag;

    if (is_skipped_tag(tag))
        return;
    if (element_is_hidden(el, B))
        return;

    switch (tag) {
    case GUMBO_TAG_BR:
        F.brk();
        return;
    case GUMBO_TAG_HR:
        F.brk();
        F.doc.addParagraph()->isRule = true;
        F.brk();
        return;
    case GUMBO_TAG_IMG: {
        const char *src = attr(el, "src");
        const char *alt = attr(el, "alt");
        if (src && src[0]) {
            std::string s = src;
            if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0)
                B.view->note_remote_image();
            HtmlImageInfo ri;
            if (B.view->image_resolver)
                ri = B.view->image_resolver(src);

            float hint_w = 0.0f, hint_h = 0.0f;
            mso_size_hint(node, hint_w, hint_h);
            float pw = 0.0f, ph = 0.0f;
            size_html_image(el, B, ri, pw, ph, hint_w, hint_h);

            /* A small icon (a heart/chevron next to text, e.g. a
             * notification-count "pill") stays inline instead of forcing
             * a block break — otherwise it and its adjacent text land on
             * separate lines.  Anything bigger is presumed to be real
             * content (a photo, an avatar, a banner) and keeps its own
             * block, centered/aligned like a standalone paragraph. An
             * unresolved image with no explicit width/height falls back
             * to size_html_image's 160x100 default, which is well past
             * the threshold — deliberately: we can't yet tell whether
             * it's a small icon, so block is the safe assumption. */
            constexpr float kInlineIconMaxPx = 32.0f;
            if (pw > 0.0f && ph > 0.0f &&
                pw <= kInlineIconMaxPx && ph <= kInlineIconMaxPx) {
                F.emitImage(ri.id, pw, ph, std::move(s));
                return;
            }

            F.brk();
            Paragraph *ip = F.doc.addParagraph();
            ip->isImage   = true;
            ip->image     = ri.id;
            ip->image_w   = pw;
            ip->image_h   = ph;
            ip->image_src = std::move(s);
            F.brk();
            return;
        }
        Style ms = st;
        ms.fgColor = B.meta;
        if (alt && alt[0])
            F.emit(std::string("[image: ") + alt + "]", ms);
        return;
    }
    default:
        break;
    }

    /* Block-level text elements handled inside the flow: paragraph break
     * before and after, style adjustments for the subtree. */
    bool block = false;
    TextAlignment save_align = F.align;
    switch (tag) {
    case GUMBO_TAG_P:    case GUMBO_TAG_ADDRESS:
    case GUMBO_TAG_DT:   case GUMBO_TAG_DD:
    case GUMBO_TAG_H1:   case GUMBO_TAG_H2:  case GUMBO_TAG_H3:
    case GUMBO_TAG_H4:   case GUMBO_TAG_H5:  case GUMBO_TAG_H6:
    case GUMBO_TAG_PRE:  case GUMBO_TAG_BLOCKQUOTE:
    case GUMBO_TAG_UL:   case GUMBO_TAG_OL:  case GUMBO_TAG_LI:
    case GUMBO_TAG_LISTING:
        block = true;
        break;
    default:
        break;
    }
    if (block) F.brk();

    switch (tag) {
    case GUMBO_TAG_B: case GUMBO_TAG_STRONG:
        st.bold = true; break;
    case GUMBO_TAG_I: case GUMBO_TAG_EM: case GUMBO_TAG_CITE: case GUMBO_TAG_DFN:
        st.italic = true; break;
    case GUMBO_TAG_U: case GUMBO_TAG_INS:
        st.underline = true; break;
    case GUMBO_TAG_S: case GUMBO_TAG_STRIKE: case GUMBO_TAG_DEL:
        break;   // no strikethrough in Style; render as normal text
    case GUMBO_TAG_CODE: case GUMBO_TAG_TT: case GUMBO_TAG_KBD:
    case GUMBO_TAG_SAMP: case GUMBO_TAG_VAR:
        st.monospace = true;
        st.bgColor = B.code_bg;
        break;
    case GUMBO_TAG_PRE: case GUMBO_TAG_LISTING: {
        st.monospace = true;
        st.bgColor = B.code_bg;
        apply_cascade(el, st, F.align, B, nullptr, node);
        bool pre_save = F.pre;
        F.pre = true;
        walk_inline_children(&el->children, st, F, B, list_depth);
        F.pre = pre_save;
        F.brk();
        return;
    }
    case GUMBO_TAG_A:
        st.fgColor = B.accent;
        st.underline = true;
        break;
    case GUMBO_TAG_H1: st.bold = true; st.fontSize *= 1.6f;  break;
    case GUMBO_TAG_H2: st.bold = true; st.fontSize *= 1.4f;  break;
    case GUMBO_TAG_H3: st.bold = true; st.fontSize *= 1.2f;  break;
    case GUMBO_TAG_H4: case GUMBO_TAG_H5: case GUMBO_TAG_H6:
        st.bold = true; st.fontSize *= 1.1f; break;
    case GUMBO_TAG_BLOCKQUOTE:
        st.italic = true;
        st.fgColor = B.meta;
        break;
    case GUMBO_TAG_SUP:
        st.superscript = true;
        st.fontSize *= 0.65f;
        break;
    case GUMBO_TAG_SMALL: st.fontSize *= 0.85f; break;
    case GUMBO_TAG_BIG:   st.fontSize *= 1.2f;  break;
    case GUMBO_TAG_UL:
    case GUMBO_TAG_OL:
        list_depth++;
        break;
    case GUMBO_TAG_LI:
        F.para();
        F.cur->isBullet   = true;
        F.cur->leftIndent = 16.0f * (float)std::max(list_depth, 1);
        break;
    case GUMBO_TAG_FONT: {
        const char *col = attr(el, "color");
        if (col) {
            bool ok = false;
            NVGcolor c = parse_html_color(col, ok);
            if (ok) st.fgColor = c;
        }
        const char *sz = attr(el, "size");
        if (sz && sz[0]) {
            /* Legacy 1..7 scale, 3 ~ base size. */
            int n = atoi(sz);
            static const float scale[8] = { 0, 0.65f, 0.8f, 1.0f, 1.2f,
                                            1.45f, 1.8f, 2.2f };
            if (n >= 1 && n <= 7)
                st.fontSize = 17.0f * scale[n];
        }
        break;
    }
    default:
        break;
    }

    apply_cascade(el, st, F.align, B, nullptr, node);
    if (tag == GUMBO_TAG_SPAN || tag == GUMBO_TAG_FONT ||
        tag == GUMBO_TAG_LABEL) {
        const char *css = attr(el, "style");
        if (css) {
            std::string l = trim_lower(css);
            if (l.find("vertical-align:top") != std::string::npos ||
                l.find("vertical-align: top") != std::string::npos ||
                l.find("vertical-align:super") != std::string::npos)
                st.superscript = true;
        }
    }

    walk_inline_children(&el->children, st, F, B, list_depth);

    F.align = save_align;
    if (block) F.brk();
}

/* Create a container widget (column flex by default) under `parent`. */
HtmlBlock *make_block(Widget *parent, FlexDirection dir, int gap,
                      AlignItems align = AlignItems::Stretch) {
    HtmlBlock *w = new HtmlBlock(parent);
    w->set_layout(new FlexLayout(dir, JustifyContent::FlexStart,
                                 align, 0, gap));
    /* set_layout() makes any container a retained display-list cache root.
     * HTML content is far too dynamic for that: baked packets (text, link
     * underlines, bg rects, baked child scissor rects) were being replayed
     * at stale coordinates all over the window.  Live-paint every frame
     * instead, like the TextEditor viewer did. */
    w->set_live(true);
    return w;
}

/* Presentational background for a block: bgcolor attribute or
 * background-color in style="". */
NVGcolor block_background(GumboElement *el, const Builder &B) {
    Style tmp;
    TextAlignment a = TextAlignment::Left;
    apply_css(el, tmp, a, B.sheet, B.dark, nullptr, ApplyMode::All);
    const char *bg = attr(el, "bgcolor");
    if (bg) {
        bool ok = false;
        NVGcolor c = parse_html_color(bg, ok);
        if (ok) tmp.bgColor = c;
    }
    const char *css = attr(el, "style");
    if (css) {
        bool ha = false;
        apply_style_attr(css, tmp, a, ha);
    }
    return tmp.bgColor;
}

int count_row_cells(GumboElement *tr) {
    int n = 0;
    for (unsigned i = 0; i < tr->children.length; ++i) {
        GumboNode *cn = (GumboNode *)tr->children.data[i];
        if (cn->type != GUMBO_NODE_ELEMENT) continue;
        GumboTag t = cn->v.element.tag;
        if (t == GUMBO_TAG_TD || t == GUMBO_TAG_TH)
            ++n;
    }
    return n;
}

/* True if a cell has no element children and no non-whitespace text —
 * an Outlook/table-shim spacer <td></td>.  These must not compete for
 * an equal share of auto_grow: a content cell sitting next to two empty
 * ones (a common "icon | spacer | spacer" pattern) would otherwise get
 * squeezed to a third of the row instead of its own natural size. */
bool cell_is_empty(GumboElement *ce) {
    for (unsigned i = 0; i < ce->children.length; ++i) {
        GumboNode *cn = (GumboNode *)ce->children.data[i];
        if (cn->type == GUMBO_NODE_ELEMENT)
            return false;
        if (cn->type == GUMBO_NODE_TEXT) {
            for (const char *p = cn->v.text.text; *p; ++p)
                if (!std::isspace((unsigned char)*p))
                    return false;
        }
    }
    return true;
}

/* True if `el`'s HTML `width=` attribute explicitly asks for shrink-to-
 * content sizing: a fixed pixel value, or the literal "auto".  This is
 * the author's own signal that the table isn't meant to stretch — e.g.
 * a `width="342px"` avatar wrapper or a `width="auto"` notification
 * "pill" — as opposed to a table with no width attribute at all, which
 * usually holds flowing paragraph text that needs the row's full width
 * to wrap (HtmlText reports 0 preferred width and relies on being
 * *stretched*, so shrink-wrapping it via AlignItems::Center would
 * collapse it to nothing). */
bool has_explicit_shrink_width(GumboElement *el) {
    const char *w = attr(el, "width");
    if (!w || !w[0])
        return false;
    std::string v = trim_lower(w);
    return v.back() != '%' && v != "100";  // bare "100" means 100%, not 100px
}

/* True if any <tr> belonging to THIS table (not a nested <table>, which
 * has its own independent layout context — e.g. the pill's tiny inner
 * icon+"1" table) has more than one real cell: a column grid (a photo
 * row, a caption's own layout table) whose cells are sized by
 * proportional flex-grow at *layout* time, not by summing children
 * bottom-up.  Their reported preferred size is near zero (grow-based
 * cells carry flex-basis 0), so shrink-wrapping the table via
 * AlignItems::Center — fine for a single-column table like an avatar or
 * a notification "pill" — collapses a grid to a sliver. */
bool table_has_multicell_row(GumboNode *node, bool is_root = true) {
    if (node->type != GUMBO_NODE_ELEMENT)
        return false;
    GumboElement *el = &node->v.element;
    if (!is_root && el->tag == GUMBO_TAG_TABLE)
        return false;
    if (el->tag == GUMBO_TAG_TR && count_row_cells(el) > 1)
        return true;
    for (unsigned i = 0; i < el->children.length; ++i)
        if (table_has_multicell_row((GumboNode *)el->children.data[i], false))
            return true;
    return false;
}

bool container_is_column(const Widget *w) {
    if (!w || !w->layout())
        return true;
    if (auto *fl = dynamic_cast<const FlexLayout *>(w->layout())) {
        FlexDirection d = fl->direction();
        return d == FlexDirection::Column || d == FlexDirection::ColumnReverse;
    }
    return true;
}

bool element_is_full_width(GumboElement *el, const Builder &B) {
    const char *w = attr(el, "width");
    if (w) {
        std::string v = trim_lower(w);
        if (v == "100%" || v == "100")
            return true;
    }
    BoxProps box;
    Style dummy;
    TextAlignment a = TextAlignment::Left;
    apply_cascade(el, dummy, a, B, &box);
    return box.width_pct >= 99.0f;
}

void element_box(GumboElement *el, const Builder &B, BoxProps &box) {
    Style dummy;
    TextAlignment a = TextAlignment::Left;
    apply_cascade(el, dummy, a, B, &box);
}

int element_height_px(GumboElement *el, const BoxProps &box) {
    if (box.height_px > 1.0f)
        return (int)box.height_px;
    const char *h = attr(el, "height");
    if (h && h[0]) {
        int n = (int)strtof(h, nullptr);
        if (n > 1)
            return n;
    }
    return 0;
}

/* Pixel width of a table cell.  -1 = auto/%;  0 = collapse (width="0"). */
int cell_px_width(GumboElement *el, const Builder &B) {
    const char *w = attr(el, "width");
    if (w && w[0]) {
        std::string v = trim_lower(w);
        if (!v.empty() && v.back() != '%') {
            int n = (int)strtof(v.c_str(), nullptr);
            if (n >= 0 && n < 2000)
                return n;
        }
        return -1;
    }
    BoxProps box;
    element_box(el, B, box);
    if (box.width_px > 0.0f && box.width_px < 2000.0f && box.width_pct <= 0.0f)
        return (int)box.width_px;
    return -1;
}

/* Percentage width of a cell from width="N%" or CSS width:N%.  0 if the
 * cell is auto/px.  Values >= 99% are treated as "just fill the row"
 * rather than a column weight (same cutoff as the grow assignment). */
float cell_width_pct(GumboElement *el, const Builder &B) {
    const char *w = attr(el, "width");
    if (w && w[0]) {
        std::string v = trim_lower(w);
        if (!v.empty() && v.back() == '%') {
            float pct = strtof(v.c_str(), nullptr);
            if (pct > 0.0f && pct < 99.0f)
                return pct;
        }
    }
    BoxProps box;
    element_box(el, B, box);
    if (box.width_pct > 0.0f && box.width_pct < 99.0f)
        return box.width_pct;
    return 0.0f;
}

/* A row whose extra cells are empty shims with no pixel width (Meta's
 * notification pill: `<td>heart 1</td><td></td><td></td>`).  Treated as
 * a 3-column flex row those empty cells still consume `gap`, so the
 * icon+"1" pack to the left of a too-wide chip.  Flatten to a column
 * instead — but keep a real [20px | content | 20px] spacer row. */
bool row_collapses_to_column(GumboElement *tr, const Builder &B) {
    int content = 0, wide_empty = 0;
    for (unsigned i = 0; i < tr->children.length; ++i) {
        GumboNode *cn = (GumboNode *)tr->children.data[i];
        if (cn->type != GUMBO_NODE_ELEMENT) continue;
        GumboElement *ce = &cn->v.element;
        if (ce->tag != GUMBO_TAG_TD && ce->tag != GUMBO_TAG_TH)
            continue;
        if (cell_is_empty(ce)) {
            if (cell_px_width(ce, B) > 0)
                wide_empty++;
        } else {
            content++;
        }
    }
    return content <= 1 && wide_empty == 0;
}

void decorate_block(HtmlBlock *b, GumboElement *el, const Builder &B) {
    BoxProps box;
    element_box(el, B, box);
    if (box.max_width_px > 80.0f && box.max_width_px < 4000.0f)
        b->set_max_width((int)box.max_width_px);
    if (box.width_px > 0.0f && box.width_px < 4000.0f &&
        box.width_pct <= 0.0f) {
        int w = (int)box.width_px;
        b->set_min_width(w);
        b->set_max_width(w);
    }
    if (box.radius_px > 0.0f)
        b->m_radius = box.radius_px;
    int h = element_height_px(el, box);
    if (h > 1)
        b->set_min_height(h);
    if (auto *fl = dynamic_cast<FlexLayout *>(b->layout())) {
        /* reset_box_style() strips padding from descendant runs; this is
         * where CSS padding actually insets the widget's children. */
        if (box.pad_x > 0.5f || box.pad_y > 0.5f)
            fl->set_padding((int)std::lround(box.pad_x),
                            (int)std::lround(box.pad_y));
        /* Instagram's notification chip is `display:inline-flex;
         * vertical-align:middle; text-align:center` — a painted pill
         * whose heart+"1" must sit in the geometric middle, not at
         * flex-start (top-left) of the padding box. */
        if (box.inline_flex || (box.align_middle && b->m_bg.a > 0.0f)) {
            fl->set_align_items(AlignItems::Center);
            fl->set_justify_content(JustifyContent::Center);
            fl->set_gap(0);
        } else if (h >= 24 && b->m_bg.a > 0.0f) {
            fl->set_justify_content(JustifyContent::Center);
        }
    }
    if (!box.bg_image_url.empty()) {
        /* Unlike an inline <img>, the box's own size comes from CSS
         * width/height (already applied above) — the image is fit into
         * it via draw()'s cover-scale, not the other way around. */
        b->m_bg_image_src = box.bg_image_url;
        if (box.bg_image_url.rfind("http://", 0) == 0 ||
            box.bg_image_url.rfind("https://", 0) == 0)
            B.view->note_remote_image();
        if (B.view->image_resolver) {
            HtmlImageInfo ri = B.view->image_resolver(box.bg_image_url);
            b->m_bg_image   = ri.id;
            b->m_bg_image_w = ri.w;
            b->m_bg_image_h = ri.h;
        }
    }
}

/* Only introduce a widget when the element actually paints a background.
 * Transparent wrappers are the save-stack bomb in the NMAIL_DEBUG_DRAW log. */
Widget *wrap_if_bg(Widget *parent, GumboElement *el, int gap, const Builder &B) {
    NVGcolor bg = block_background(el, B);
    if (bg.a <= 0.0f)
        return parent;
    /* Nested spacer tables often repeat the same bgcolor. A second fill
     * of the same color is a no-op and must not add another save frame. */
    if (auto *hb = dynamic_cast<HtmlBlock *>(parent)) {
        if (hb->m_bg.a > 0.0f &&
            hb->m_bg.r == bg.r && hb->m_bg.g == bg.g && hb->m_bg.b == bg.b)
            return parent;
    }
    HtmlBlock *b = make_block(parent, FlexDirection::Column, gap);
    b->m_bg = bg;
    decorate_block(b, el, B);
    return b;
}

TextAlignment element_align(GumboElement *el, TextAlignment inherited,
                            bool &changed, const Builder &B) {
    TextAlignment a = inherited;
    Style dummy;
    apply_cascade(el, dummy, a, B);
    changed = (a != inherited);
    return a;
}

/* flex-grow hint from width="N%" or colspan. */
float cell_grow(GumboElement *el) {
    const char *w = attr(el, "width");
    if (w) {
        std::string v = trim_lower(w);
        if (!v.empty() && v.back() == '%') {
            float pct = strtof(v.c_str(), nullptr);
            if (pct > 0.0f)
                return pct;
        }
    }
    const char *cs = attr(el, "colspan");
    if (cs) {
        int n = atoi(cs);
        if (n > 1)
            return (float)n;
    }
    return 1.0f;
}

void build_children(Widget *container, GumboVector *kids, Style st,
                    Builder &B, int list_depth, TextAlignment align) {
    Flow F;
    F.align = align;

    auto flush = [&]() {
        if (F.has_content())
            new HtmlText(container, std::move(F.doc), NVGcolor{ { { 0,0,0,0 } } });
        F = Flow();
        F.align = align;
    };

    for (unsigned i = 0; i < kids->length; ++i) {
        GumboNode *node = (GumboNode *)kids->data[i];
        if (node->type != GUMBO_NODE_ELEMENT) {
            walk_inline(node, st, F, B, list_depth);
            continue;
        }
        GumboElement *el = &node->v.element;
        GumboTag tag = el->tag;
        bool block_anchor = tag == GUMBO_TAG_A && element_has_block_child(el);

        if ((!is_container_tag(tag) || is_skipped_tag(tag)) && !block_anchor) {
            walk_inline(node, st, F, B, list_depth);
            continue;
        }

        /* Container-level element: close the current text flow first. */
        flush();
        if (element_is_hidden(el, B))
            continue;

        /* MJML / inline-block columns: consecutive width:N% (<100) siblings
         * become one flex row (cover | details). */
        if (tag != GUMBO_TAG_TABLE && tag != GUMBO_TAG_TBODY &&
            tag != GUMBO_TAG_THEAD && tag != GUMBO_TAG_TFOOT &&
            tag != GUMBO_TAG_TR && tag != GUMBO_TAG_TD &&
            tag != GUMBO_TAG_TH && tag != GUMBO_TAG_HTML &&
            tag != GUMBO_TAG_BODY) {
            float pct0 = flex_column_pct(el, B);
            if (pct0 > 0.0f) {
                std::vector<GumboElement *> cols;
                cols.push_back(el);
                unsigned k = i + 1;
                while (k < kids->length) {
                    GumboNode *n2 = (GumboNode *)kids->data[k];
                    if (n2->type == GUMBO_NODE_WHITESPACE ||
                        n2->type == GUMBO_NODE_COMMENT) {
                        ++k;
                        continue;
                    }
                    if (n2->type != GUMBO_NODE_ELEMENT)
                        break;
                    GumboElement *e2 = &n2->v.element;
                    if (is_skipped_tag(e2->tag) || element_is_hidden(e2, B)) {
                        ++k;
                        continue;
                    }
                    if (!is_container_tag(e2->tag))
                        break;
                    float p2 = flex_column_pct(e2, B);
                    if (p2 <= 0.0f)
                        break;
                    cols.push_back(e2);
                    ++k;
                }
                if (cols.size() >= 2) {
                    HtmlBlock *row = make_block(container, FlexDirection::Row, 8);
                    for (GumboElement *ce : cols) {
                        HtmlBlock *cell = make_block(row, FlexDirection::Column, 2);
                        cell->m_bg = block_background(ce, B);
                        float grow = flex_column_pct(ce, B);
                        if (grow <= 0.0f) grow = 1.0f;
                        build_children(cell, &ce->children, st, B, list_depth,
                                       align);
                        if (auto *fl = dynamic_cast<FlexLayout *>(row->layout()))
                            fl->set_flex_item(cell,
                                              FlexLayout::FlexItem(grow, 1.0f, 0));
                    }
                    i = k - 1;
                    continue;
                }
            }
        }

        switch (tag) {
        case GUMBO_TAG_TABLE:
        case GUMBO_TAG_TBODY:
        case GUMBO_TAG_THEAD:
        case GUMBO_TAG_TFOOT: {
            bool save_fw = B.in_full_width;
            std::vector<float> save_cols;
            if (tag == GUMBO_TAG_TABLE) {
                B.in_full_width = element_is_full_width(el, B);
                save_cols = B.table_col_grow;
                B.table_col_grow.clear();
            }
            Widget *target = container;
            if (tag == GUMBO_TAG_TABLE) {
                BoxProps box;
                element_box(el, B, box);
                NVGcolor bg = block_background(el, B);
                bool cap = box.max_width_px > 80.0f && box.max_width_px < 4000.0f;
                /* Email convention: max-width:600px (even with width:100%)
                 * is a centered column, not a shrink-wrapped align=center
                 * cell — that path collapsed Amazon's 33/67 rows.
                 * `margin:auto` plus an explicit non-% width= is the
                 * other common centering idiom (a lone avatar, a
                 * notification "pill") — shrink-wrap and center it too,
                 * unless it's actually a column grid in disguise. */
                bool center_shrink = !cap && box.center &&
                                     has_explicit_shrink_width(el) &&
                                     !table_has_multicell_row(node);
                if (cap || center_shrink) {
                    HtmlBlock *outer = make_block(container, FlexDirection::Column,
                                                  0, AlignItems::Center);
                    target = outer;
                }
                if (bg.a > 0.0f || cap || box.radius_px > 0.0f) {
                    HtmlBlock *inner = make_block(target, FlexDirection::Column, 2);
                    inner->m_bg = bg;
                    decorate_block(inner, el, B);
                    target = inner;
                }
            }
            build_children(target, &el->children, st, B, list_depth, align);
            B.in_full_width = save_fw;
            if (tag == GUMBO_TAG_TABLE)
                B.table_col_grow = std::move(save_cols);
            break;
        }
        case GUMBO_TAG_TR: {
            if (row_collapses_to_column(el, B) && container_is_column(container)) {
                Widget *target = wrap_if_bg(container, el, 2, B);
                for (unsigned j = 0; j < el->children.length; ++j) {
                    GumboNode *cn = (GumboNode *)el->children.data[j];
                    if (cn->type != GUMBO_NODE_ELEMENT) continue;
                    GumboElement *ce = &cn->v.element;
                    if (ce->tag != GUMBO_TAG_TD && ce->tag != GUMBO_TAG_TH)
                        continue;
                    if (element_is_hidden(ce, B))
                        continue;
                    Widget *cell = target;
                    BoxProps box;
                    element_box(ce, B, box);
                    int h = element_height_px(ce, box);
                    bool paint = B.in_full_width || element_is_full_width(ce, B);
                    /* Shrink-wrapped chip tables (Learn More): the TD's
                     * text-align:center is for a content-sized table, not
                     * a stretched HtmlText across the 67% column. */
                    TextAlignment ta = align;
                    if (paint) {
                        bool ch = false;
                        ta = element_align(ce, align, ch, B);
                    }
                    if (paint)
                        cell = wrap_if_bg(target, ce, 2, B);
                    if (cell == target && (h > 1 || box.radius_px > 0.0f ||
                                           box.max_width_px > 80.0f)) {
                        HtmlBlock *sp = make_block(target, FlexDirection::Column, 0);
                        decorate_block(sp, ce, B);
                        cell = sp;
                    }
                    Style cst = st;
                    TextAlignment cascade_align = ta;
                    apply_cascade(ce, cst, cascade_align, B);
                    if (paint)
                        ta = cascade_align;
                    cst.superscript = st.superscript;
                    reset_box_style(cst);
                    if (ce->tag == GUMBO_TAG_TH)
                        cst.bold = true;
                    build_children(cell, &ce->children, cst, B, list_depth, ta);
                }
                break;
            }
            /* cellspacing=0 tables (the email default) pack cells flush;
             * an 8px flex gap made Benchmark columns drift per-row. */
            HtmlBlock *row = make_block(container, FlexDirection::Row, 0);
            row->m_bg = block_background(el, B);
            float explicit_sum = 0.0f;
            int auto_cells = 0;
            for (unsigned j = 0; j < el->children.length; ++j) {
                GumboNode *cn = (GumboNode *)el->children.data[j];
                if (cn->type != GUMBO_NODE_ELEMENT) continue;
                GumboTag ct = cn->v.element.tag;
                if (ct != GUMBO_TAG_TD && ct != GUMBO_TAG_TH) continue;
                if (cell_px_width(&cn->v.element, B) >= 0)
                    continue;
                if (cell_is_empty(&cn->v.element))
                    continue;
                float pct = cell_width_pct(&cn->v.element, B);
                if (pct > 0.0f)
                    explicit_sum += pct;
                else
                    auto_cells++;
            }
            float auto_grow = auto_cells > 0
                ? std::max(100.0f - explicit_sum, 5.0f) / (float)auto_cells
                : 1.0f;
            std::vector<float> row_grows;
            int col_i = 0;
            for (unsigned j = 0; j < el->children.length; ++j) {
                GumboNode *cn = (GumboNode *)el->children.data[j];
                if (cn->type != GUMBO_NODE_ELEMENT) continue;
                GumboElement *ce = &cn->v.element;
                if (ce->tag != GUMBO_TAG_TD && ce->tag != GUMBO_TAG_TH)
                    continue;
                if (element_is_hidden(ce, B))
                    continue;
                HtmlBlock *cell = make_block(row, FlexDirection::Column, 2);
                cell->m_bg = block_background(ce, B);
                decorate_block(cell, ce, B);
                bool ch = false;
                TextAlignment ta = element_align(ce, align, ch, B);
                Style cst = st;
                apply_cascade(ce, cst, ta, B);
                cst.superscript = st.superscript;
                reset_box_style(cst);
                if (ce->tag == GUMBO_TAG_TH)
                    cst.bold = true;
                build_children(cell, &ce->children, cst, B, list_depth, ta);
                int px = cell_px_width(ce, B);
                float grow = auto_grow;
                float shrink = 1.0f;
                /* -1 ("auto"): FlexLayout::preferred_size() falls back to
                 * the cell's own clamped preferred size when basis < 0,
                 * vs. reporting a literal 0px — needed so a real (grow-
                 * distributed) content cell still contributes its true
                 * width to the row/table's bottom-up preferred size,
                 * e.g. when an ancestor shrink-wraps via
                 * AlignItems::Center (see GUMBO_TAG_TABLE's
                 * center_shrink). Full-width tables use basis 0 so
                 * grow (including a column template from the header)
                 * owns the width and every row's columns line up. */
                int basis = -1;
                if (px > 0) {
                    grow = 0.0f;
                    shrink = 0.0f;
                    basis = px;
                    cell->set_min_width(px);
                    cell->set_max_width(px);
                } else if (px == 0 || cell_is_empty(ce)) {
                    grow = 0.0f;
                    shrink = 0.0f;
                    basis = 0;
                    cell->set_max_width(0);
                } else {
                    float pct = cell_width_pct(ce, B);
                    if (pct > 0.0f)
                        grow = pct;
                    else if (const char *cs = attr(ce, "colspan"))
                        grow = auto_grow * (float)std::max(atoi(cs), 1);
                    if (!B.table_col_grow.empty() &&
                        col_i < (int)B.table_col_grow.size())
                        grow = B.table_col_grow[col_i];
                    if (B.in_full_width)
                        basis = 0;
                }
                row_grows.push_back(grow);
                col_i++;
                if (auto *fl = dynamic_cast<FlexLayout *>(row->layout()))
                    fl->set_flex_item(cell, FlexLayout::FlexItem(grow, shrink, basis));
            }
            if (B.table_col_grow.empty() && row_grows.size() >= 2)
                B.table_col_grow = std::move(row_grows);
            break;
        }
        case GUMBO_TAG_TD:
        case GUMBO_TAG_TH: {
            Style cst = st;
            if (tag == GUMBO_TAG_TH)
                cst.bold = true;
            /* Orphaned cell in a column (after a flattened 1-col table):
             * don't wrap unless it paints a background. In a row, keep
             * a cell widget so flex-grow can share width. */
            if (container_is_column(container)) {
                Widget *cell = container;
                if (B.in_full_width || element_is_full_width(el, B))
                    cell = wrap_if_bg(container, el, 2, B);
                build_children(cell, &el->children, cst, B, list_depth, align);
            } else {
                HtmlBlock *cell = make_block(container, FlexDirection::Column, 2);
                cell->m_bg = block_background(el, B);
                build_children(cell, &el->children, cst, B, list_depth, align);
                if (auto *fl = dynamic_cast<FlexLayout *>(container->layout()))
                    fl->set_flex_item(cell, FlexLayout::FlexItem(cell_grow(el), 1.0f, 0));
            }
            break;
        }
        case GUMBO_TAG_UL:
        case GUMBO_TAG_OL: {
            if (list_is_nav_row(el, B)) {
                HtmlBlock *row = make_block(container, FlexDirection::Row, 0);
                for (unsigned j = 0; j < el->children.length; ++j) {
                    GumboNode *cn = (GumboNode *)el->children.data[j];
                    if (cn->type != GUMBO_NODE_ELEMENT) continue;
                    GumboElement *ce = &cn->v.element;
                    if (ce->tag != GUMBO_TAG_LI) continue;
                    if (element_is_hidden(ce, B)) continue;
                    HtmlBlock *cell = make_block(row, FlexDirection::Column, 0,
                                                 AlignItems::Center);
                    decorate_block(cell, ce, B);
                    bool ch = false;
                    TextAlignment ta = element_align(ce, align, ch, B);
                    Style cst = st;
                    apply_cascade(ce, cst, ta, B);
                    cst.superscript = st.superscript;
                    reset_box_style(cst);
                    build_children(cell, &ce->children, cst, B, 0, ta);
                    float grow = flex_column_pct(ce, B);
                    if (grow <= 0.0f) grow = 1.0f;
                    int basis = B.in_full_width ? 0 : -1;
                    if (auto *fl = dynamic_cast<FlexLayout *>(row->layout()))
                        fl->set_flex_item(cell,
                                          FlexLayout::FlexItem(grow, 1.0f, basis));
                }
            } else {
                Flow listF;
                listF.align = align;
                walk_inline(node, st, listF, B, list_depth);
                if (listF.has_content())
                    new HtmlText(container, std::move(listF.doc),
                                 NVGcolor{ { { 0, 0, 0, 0 } } });
            }
            break;
        }
        case GUMBO_TAG_CENTER: {
            /* Alignment is a Document property, not a widget. Nested
             * <center> in HTML mail must not add a save frame. */
            Widget *target = wrap_if_bg(container, el, 4, B);
            build_children(target, &el->children, st, B, list_depth,
                           TextAlignment::Center);
            break;
        }
        case GUMBO_TAG_CAPTION: {
            Widget *target = wrap_if_bg(container, el, 4, B);
            Style cst = st;
            cst.italic = true;
            cst.fgColor = B.meta;
            build_children(target, &el->children, cst, B, list_depth,
                           TextAlignment::Center);
            break;
        }
        case GUMBO_TAG_HTML:
        case GUMBO_TAG_BODY: {
            NVGcolor bg = block_background(el, B);
            if (tag == GUMBO_TAG_BODY && bg.a > 0.0f) {
                if (auto *hb = dynamic_cast<HtmlBlock *>(container))
                    hb->m_bg = bg;
                B.view->set_background(bg);
            }
            build_children(container, &el->children, st, B, list_depth, align);
            break;
        }
        default: {
            bool align_changed = false;
            TextAlignment child_align = element_align(el, align, align_changed, B);
            NVGcolor bg = block_background(el, B);
            BoxProps box;
            element_box(el, B, box);
            /* A lone (ungrouped) table-cell's max-width is a fallback
             * bound, not a "center me as a column" hint — see BoxProps.
             * table_cell.  Using it here shrank single-column email rows
             * (e.g. a hero image row) down to their cell's mobile bound. */
            int cap = (!box.table_cell && box.max_width_px > 80.0f &&
                       box.max_width_px < 4000.0f)
                          ? (int)box.max_width_px : 0;
            if (bg.a <= 0.0f && !align_changed && cap <= 0 &&
                box.radius_px <= 0.0f && !box.center &&
                !box.inline_flex && box.pad_x <= 0.5f && box.pad_y <= 0.5f &&
                box.bg_image_url.empty()) {
                Style cst = st;
                TextAlignment ta = child_align;
                apply_cascade(el, cst, ta, B);
                cst.superscript = st.superscript;
                reset_box_style(cst);
                build_children(container, &el->children, cst, B, list_depth,
                               ta);
            } else {
                Widget *host = container;
                /* Shrink-wrap inline-flex chips (the notification pill)
                 * and max-width columns so they don't stretch full-width
                 * with the icon sitting at the top-left of a red bar.
                 * max-width alone is a ceiling (PennyMac .header-menu is
                 * 610px inside a 600px column) — only margin:auto is a
                 * "center me as a page column" hint. */
                bool shrink = box.inline_flex ||
                              (box.center && box.width_px > 0.0f &&
                               box.width_pct < 99.0f);
                bool parent_centers = false;
                if (auto *pfl = dynamic_cast<FlexLayout *>(container->layout()))
                    parent_centers = pfl->align_items() == AlignItems::Center;
                if (((cap > 0 && box.center) || shrink) && !parent_centers) {
                    HtmlBlock *outer = make_block(container, FlexDirection::Column,
                                                  0, AlignItems::Center);
                    host = outer;
                }
                HtmlBlock *blk = make_block(host, FlexDirection::Column, 4);
                blk->m_bg = bg;
                decorate_block(blk, el, B);
                Style cst = st;
                TextAlignment ta = child_align;
                apply_cascade(el, cst, ta, B);
                cst.superscript = st.superscript;
                reset_box_style(cst);
                build_children(blk, &el->children, cst, B, list_depth, ta);
            }
            break;
        }
        }
    }
    flush();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HtmlDocument
// ---------------------------------------------------------------------------

HtmlDocument::HtmlDocument(Widget *parent) : Widget(parent) {
    set_layout(new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                              AlignItems::Stretch, 16, 4));
    set_live(true);   // see make_block: never bake HTML content into draw lists
#ifdef DEBUG
    if (debug_draw())
        fprintf(stderr, "[trace] HtmlDocument flatten+skip-identity-save\n");
#endif
}

void HtmlDocument::clear() {
#ifdef DEBUG
    if (debug_draw())
        fprintf(stderr, "[trace] HtmlDocument::clear %p children=%zu\n",
                (void *)this, m_children.size());
#endif
    while (!m_children.empty())
        remove_child(m_children.back());
    m_has_remote = false;
}

Vector2i HtmlDocument::preferred_size(NVGcontext *ctx) const {
    Vector2i p = Widget::preferred_size(ctx);
    /* Width comes from the reading pane. Reporting a max-width (600, 424, …)
     * as preferred locked the split and let the view paint over the list. */
    p.x() = 0;
    return p;
}

void HtmlDocument::relayout() {
    if (Screen *s = screen()) {
        s->perform_layout();
        s->redraw();
    }
}

int HtmlDocument::bind_loaded_images() {
    if (!image_resolver)
        return 0;
    int n = 0;
    std::function<void(Widget *)> walk = [&](Widget *w) {
        if (auto *ht = dynamic_cast<HtmlText *>(w)) {
            bool dirty = false;
            for (auto &p : ht->m_doc.paragraphs) {
                if (p->isImage && p->image <= 0 && !p->image_src.empty()) {
                    HtmlImageInfo ri = image_resolver(p->image_src);
                    if (ri.id > 0) {
                        p->image = ri.id;
                        /* Keep the HTML/CSS layout box (max-width:148px
                         * etc.). Only correct aspect from the decoded
                         * bitmap. */
                        if (p->image_w > 0.0f && ri.w > 0.0f && ri.h > 0.0f)
                            p->image_h = p->image_w * (ri.h / ri.w);
                        else if (ri.w > 0.0f && ri.h > 0.0f) {
                            p->image_w = ri.w;
                            p->image_h = ri.h;
                        }
                        dirty = true;
                        ++n;
                    }
                }
                for (Text &run : p->runs) {
                    if (run.image > 0 || run.image_src.empty())
                        continue;
                    HtmlImageInfo ri = image_resolver(run.image_src);
                    if (ri.id <= 0)
                        continue;
                    run.image = ri.id;
                    dirty = true;
                    ++n;
                }
            }
            if (dirty) {
                ht->m_doc.markLayoutDirty();
                ht->m_measured_w = -1;
                ht->m_natural_w  = -1;
            }
        } else if (auto *hb = dynamic_cast<HtmlBlock *>(w)) {
            if (hb->m_bg_image <= 0 && !hb->m_bg_image_src.empty()) {
                HtmlImageInfo ri = image_resolver(hb->m_bg_image_src);
                if (ri.id > 0) {
                    hb->m_bg_image   = ri.id;
                    hb->m_bg_image_w = ri.w;
                    hb->m_bg_image_h = ri.h;
                    ++n;
                }
            }
        }
        for (Widget *c : w->children())
            walk(c);
    };
    walk(this);
    if (n > 0)
        relayout();
    return n;
}

void HtmlDocument::set_html(const std::string &html) {
    clear();

    /* Light text on dark background -> dark mode link/code colors. */
    bool dark_text_is_light = (m_text.r + m_text.g + m_text.b) > 1.5f;
    Builder B;
    B.view = this;
    B.accent = dark_text_is_light ? nvgRGBA(110, 160, 255, 255)
                                  : nvgRGBA(10, 80, 220, 255);
    B.meta = m_meta;
    B.code_bg = dark_text_is_light ? nvgRGBA(50, 52, 62, 255)
                                   : nvgRGBA(228, 229, 236, 255);
    B.dark = dark_text_is_light;

    GumboOutput *out = gumbo_parse(html.c_str());
    std::string css;
    collect_style_text(out->root, css);
    parse_stylesheet(css, B.sheet);
    Style base;
    base.fontSize = 17.0f;
    base.fgColor  = m_text;
    build_children(this, &out->root->v.document.children, base, B, 0,
                   TextAlignment::Left);
    gumbo_destroy_output(&kGumboDefaultOptions, out);

    if (m_children.empty())
        new HtmlText(this, Document(), NVGcolor{ { { 0, 0, 0, 0 } } });
#ifdef DEBUG
    if (debug_draw()) {
        fprintf(stderr, "[trace] set_html done %p children=%zu parent=%p css_rules=%zu\n",
                (void *)this, m_children.size(), (void *)m_parent,
                B.sheet.rules.size());
        fprintf(stderr, "%s", debug_summary().c_str());
    }
#endif
    relayout();
}

void HtmlDocument::set_plain(const std::string &text) {
    clear();
    Document doc;
    Style normal;
    normal.fontSize = 17.0f;
    normal.fgColor  = m_text;

    std::string line;
    Paragraph *cur = nullptr;
    for (size_t i = 0; i <= text.size(); ++i) {
        char c = (i < text.size()) ? text[i] : '\n';
        if (c != '\n') { line += c; continue; }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { cur = nullptr; continue; }
        if (!cur) cur = doc.addParagraph();
        else      cur->addText(" ", normal);
        cur->addText(line, normal);
        line.clear();
    }
    if (doc.paragraphs.empty())
        doc.addParagraph();
    new HtmlText(this, std::move(doc), NVGcolor{ { { 0, 0, 0, 0 } } });
    relayout();
}

void HtmlDocument::set_document(Document &&doc) {
    clear();
    new HtmlText(this, std::move(doc), NVGcolor{ { { 0, 0, 0, 0 } } });
    relayout();
}

std::string HtmlDocument::debug_summary() const {
    std::string out;
    std::function<void(const Widget *, int)> walk = [&](const Widget *w, int d) {
        char line[256];
        std::snprintf(line, sizeof(line), "%*s(%d,%d %dx%d)",
                      d * 2, "", w->position().x(), w->position().y(),
                      w->size().x(), w->size().y());
        out += line;
        if (auto *hb = dynamic_cast<const HtmlBlock *>(w)) {
            char extra[96];
            std::snprintf(extra, sizeof(extra), "  [bg.a=%.2f r=%.0f]",
                          hb->m_bg.a, hb->m_radius);
            out += extra;
        }
        if (auto *ht = dynamic_cast<const HtmlText *>(w)) {
            std::string t;
            for (const auto &p : ht->m_doc.paragraphs) {
                if (p->isRule) { t += "<hr>"; continue; }
                if (p->isImage) { t += "<img>"; continue; }
                if (p->isBullet) t += "* ";
                t += p->plain_text();
                for (const Text &r : p->runs) {
                    char rs[160];
                    std::snprintf(rs, sizeof(rs),
                        " [fg=%.2f,%.2f,%.2f,%.2f bg=%.2f,%.2f,%.2f,%.2f %s%s%s%s sz=%.1f pad=%.0f,%.0f brd=%.1f]",
                        r.style.fgColor.r, r.style.fgColor.g,
                        r.style.fgColor.b, r.style.fgColor.a,
                        r.style.bgColor.r, r.style.bgColor.g,
                        r.style.bgColor.b, r.style.bgColor.a,
                        r.style.bold ? "b" : "", r.style.italic ? "i" : "",
                        r.style.underline ? "u" : "",
                        r.style.monospace ? "m" : "",
                        r.style.fontSize, r.style.padX, r.style.padY,
                        r.style.borderWidth);
                    if (r.style.superscript) t += " sup";
                    t += rs;
                }
                t += " | ";
            }
            if (t.size() > 700) t = t.substr(0, 700) + "...";
            out += "  \"" + t + "\"";
        }
        out += "\n";
        for (const Widget *c : w->children()) walk(c, d + 1);
    };
    walk(this, 0);
    return out;
}

void HtmlDocument::request_reflow() {
    m_reflow_pending = true;
    if (Screen *s = screen())
        s->redraw();
}

void HtmlDocument::draw(NVGcontext *ctx) {
    if (nvgIsRecordingDisplayList(ctx))
        return;
    trace_draw("HtmlDocument", this);
    if (m_reflow_pending) {
#ifdef DEBUG
        if (debug_draw()) fprintf(stderr, "[trace] HtmlDocument reflow\n");
#endif
        /* A leaf's measured height changed during the last paint.  Re-run
         * the ScrollPanel's layout BEFORE painting our subtree so every
         * widget below paints with consistent geometry (never relayout
         * mid-paint). */
        m_reflow_pending = false;
        if (m_parent)
            m_parent->perform_layout(ctx);
    }
    /* Clip to our rect in parent (ScrollPanel) space. Widget::draw would
     * add another 2 saves per nested table; walk children ourselves. */
    int depth0 = nvgStateDepth(ctx);
    nvgSave(ctx);
    if (nvgStateDepth(ctx) <= depth0)
        return;
    nvgIntersectScissor(ctx, (float)m_pos.x(), (float)m_pos.y(),
                        (float)m_size.x(), (float)m_size.y());
    if (m_bg.a > 0.0f) {
        nvgBeginPath(ctx);
        nvgRect(ctx, (float)m_pos.x(), (float)m_pos.y(),
                (float)m_size.x(), (float)m_size.y());
        nvgFillColor(ctx, m_bg);
        nvgFill(ctx);
    }
    nvgTranslate(ctx, (float)m_pos.x(), (float)m_pos.y());
    for (Widget *c : m_children) {
        if (c->visible())
            c->draw(ctx);
    }
    nvgRestore(ctx);
}
