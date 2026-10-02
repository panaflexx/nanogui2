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
#include <nanogui/scrollpanel.h>
#include <nanogui/zoomscrollpanel.h>
#include <GLFW/glfw3.h>
#include "gumbo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h> // open() for /dev/null redirect in open_url_secure child
#include <unistd.h>
#endif

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
static inline float hue2rgb(float p, float q, float t) {
    if (t < 0.f) t += 1.f;
    if (t > 1.f) t -= 1.f;
    if (t < 1.f/6.f) return p + (q - p) * 6.f * t;
    if (t < 1.f/2.f) return q;
    if (t < 2.f/3.f) return p + (q - p) * (2.f/3.f - t) * 6.f;
    return p;
}
static NVGcolor hsl_to_rgb(float h, float s, float l) {
    h = std::fmod(h, 360.f); if (h < 0) h += 360.f; h /= 360.f;
    s = std::clamp(s, 0.f, 1.f); l = std::clamp(l, 0.f, 1.f);
    if (s == 0.f) { unsigned v = (unsigned)std::lround(l * 255.f); return nvgRGB(v, v, v); }
    float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
    float p = 2.f * l - q;
    float r = hue2rgb(p, q, h + 1.f/3.f), g = hue2rgb(p, q, h), b = hue2rgb(p, q, h - 1.f/3.f);
    return nvgRGB((unsigned)std::lround(r*255.f), (unsigned)std::lround(g*255.f), (unsigned)std::lround(b*255.f));
}

NVGcolor parse_html_color(const char *s, bool &ok) {
    ok = true;
    if (s && s[0] == '#') {
        unsigned r = 0, g = 0, b = 0, a = 255;
        size_t len = strlen(s + 1);
        if (len == 8 && std::sscanf(s + 1, "%02x%02x%02x%02x", &r, &g, &b, &a) == 4)
            return nvgRGBA(r, g, b, a);
        if (len == 6 && std::sscanf(s + 1, "%02x%02x%02x", &r, &g, &b) == 3)
            return nvgRGB(r, g, b);
        if (len == 4 && std::sscanf(s + 1, "%01x%01x%01x%01x", &r, &g, &b, &a) == 4)
            return nvgRGBA(r * 17, g * 17, b * 17, a * 17);
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
        if (l.rfind("hsl", 0) == 0) {
            size_t p = l.find('(');
            if (p != std::string::npos) {
                bool has_alpha = l.rfind("hsla", 0) == 0;
                float comp[4] = {0,0,0,1}; int n=0; size_t i=p+1;
                while (i < l.size() && n < 4) {
                    while (i < l.size() && (l[i]==' '||l[i]==','||l[i]=='%')) ++i;
                    if (i >= l.size() || l[i]==')') break;
                    char *end=nullptr;
                    float v=strtof(l.c_str()+i,&end);
                    if (end==l.c_str()+i) break;
                    comp[n++]=v;
                    i=end-l.c_str();
                    // consume trailing % for s/l
                    while (i < l.size() && l[i]==' ') ++i;
                    if (i < l.size() && l[i]=='%') ++i;
                }
                if (n >= 3) {
                    NVGcolor c = hsl_to_rgb(comp[0], comp[1]/100.f, comp[2]/100.f);
                    if (has_alpha && n>=4) c.a = std::clamp(comp[3],0.f,1.f);
                    return c;
                }
            }
        }
        if (l == "transparent") { ok=true; return nvgRGBA(0,0,0,0); }
        if (l == "currentcolor") { ok=false; return nvgRGB(0,0,0); }
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
    float min_width_px  = 0.0f;
    float min_height_px = 0.0f;
    bool  border_box    = false; // box-sizing: border-box
    bool  overflow_hidden = false;
    /* display:inline-flex / inline-block / inline — shrink-to-content
     * instead of stretching as a block. */
    bool  inline_flex   = false;
    /* display:inline-flex specifically (not inline-block/inline, which
     * share the shrink-to-content behavior above but carry no flexbox
     * centering intent — an MJML column's display:inline-block is a
     * layout-flow convention, not "center my children"). */
    bool  is_inline_flex = false;
    /* vertical-align:middle on a flex/painted box — center children. */
    bool  align_middle  = false;
    /* float:left — email nav bars (PennyMac header-menu LIs). */
    bool  float_left    = false;
    /* display:block on an <img>: no line-box gap under the image. */
    bool  display_block = false;
    /* line-height:0 (or 0px) on the element itself — Outlook spacer
     * cells holding only an &nbsp;. */
    bool  zero_line_height = false;
    /* CSS vertical-align as a table-cell placement: 't'op, 'm'iddle,
     * 'b'ottom, or 0 if unset (see decorate_cell). */
    char  cell_valign   = 0;
    /* CSS `background-image:url(...)` on a plain <div> — common in
     * marketing/notification email (Meta digests use it for every real
     * photo, never <img src>).  Resolved and painted like an <img>. */
    std::string bg_image_url;
};

static inline bool css_finite(float v) { return std::isfinite(v) && std::fabs(v) < 1e6f; }

static void apply_line_height(const std::string &v, float &lh, float fontSize) {
    std::string t = trim_lower(v);
    if (t.empty() || t == "normal") { lh = 0.f; return; }
    char *end = nullptr;
    float n = std::strtof(t.c_str(), &end);
    if (end == t.c_str() || !css_finite(n)) return;
    std::string unit = trim_lower(end);
    if (unit.empty()) { // unitless — factor of font-size
        if (n > 0.05f && n < 5.f) lh = fontSize * n;
        else if (n >= 5.f) lh = n;
    } else if (unit == "px" || unit == "pt") {
        if (unit == "pt") n *= (96.f/72.f);
        lh = std::max(n, 1.f);
    } else if (unit == "em") {
        lh = std::max(fontSize * n, 1.f);
    } else if (unit == "%") {
        lh = std::max(fontSize * (n/100.f), 1.f);
    }
    lh = std::min(lh, 200.f);
}

/* font-size value: Npx / Npt / Nem / N% (em and % are relative to the
 * inherited size), plus the CSS absolute-size keywords (browsers anchor
 * medium at 16px — nmail's email base is 17, close enough that the
 * classic values line up) and the relative smaller/larger.  The USAA nav
 * links use font-size:xx-small; dropping the keyword left them at the
 * 17px base, too wide for their 183px cell, so they wrapped. */
void apply_font_size(const std::string &v, float &size) {
    if (v == "xx-small") { size = 9.0f;  return; }
    if (v == "x-small")  { size = 10.0f; return; }
    if (v == "small")    { size = 13.0f; return; }
    if (v == "medium")   { size = 16.0f; return; }
    if (v == "large")    { size = 18.0f; return; }
    if (v == "x-large")  { size = 24.0f; return; }
    if (v == "xx-large") { size = 32.0f; return; }
    if (v == "smaller")  { size = std::max(size / 1.2f, 6.0f);  return; }
    if (v == "larger")   { size = std::min(size * 1.2f, 72.0f); return; }
    char *end = nullptr;
    float n = std::strtof(v.c_str(), &end);
    if (end == v.c_str() || n <= 0.0f || !css_finite(n))
        return;
    std::string unit = trim_lower(end);
    if (unit == "pt")      size = n * (96.0f / 72.0f);
    else if (unit == "em") size = size * n;
    else if (unit == "%")  size = size * (n / 100.0f);
    else                   size = n;   // px (or unitless)
    size = std::min(std::max(size, 6.0f), 72.0f);
}

/* font: [style] [weight] size[/line-height] family… — take the
 * style/weight/size, ignore the family list (families normalize
 * elsewhere).  USAA's "font: bold 16px Arial, sans-serif" header. */
static void handle_font_shorthand(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t) {
    size_t p = 0;
    bool got_size = false;
    while (p < val.size()) {
        while (p < val.size() && std::isspace((unsigned char)val[p])) ++p;
        if (p >= val.size()) break;
        size_t q = val.find_first_of(" \t", p);
        std::string tok = val.substr(p, q == std::string::npos ? std::string::npos : q - p);
        p = (q == std::string::npos) ? val.size() : q + 1;
        if (tok == "bold" || tok == "bolder") { st.bold = true; continue; }
        if (tok == "italic" || tok == "oblique") { st.italic = true; continue; }
        if (tok == "normal") { st.bold = false; st.italic = false; continue; }
        if (!got_size) {
            size_t slash = tok.find('/');
            float before = st.fontSize;
            apply_font_size(slash == std::string::npos ? tok : tok.substr(0, slash),
                            st.fontSize);
            if (st.fontSize != before)
                got_size = true;
        }
    }
}
/* CSS length: px / pt / % / unitless.  "auto"/"none" are ignored. */
bool parse_css_len(const std::string &val, float &px, float &pct) {
    px = 0.0f;
    pct = 0.0f;
    if (val.empty() || val == "auto" || val == "none" || val == "initial")
        return false;
    char *end = nullptr;
    float n = std::strtof(val.c_str(), &end);
    if (end == val.c_str() || !css_finite(n))
        return false;
    if (std::fabs(n) > 1e6f) return false;
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

/* Table-driven style dispatcher — replaces 300-line if/else chain.
 * Keeps exact same semantics; cover-gate is gold_diff + pill hacks.
 * Helpers are the small per-property functions below. */
enum PropId {
    P_NONE, P_COLOR, P_BG, P_BG_COLOR, P_BG_IMAGE, P_FONT_SIZE, P_FONT_WEIGHT, P_FONT_STYLE,
    P_TEXT_DECOR, P_FONT_FAMILY, P_FONT, P_DISPLAY, P_MSO_HIDE, P_TEXT_ALIGN, P_LINE_HEIGHT, P_VERT_ALIGN,
    P_FLOAT, P_PADDING, P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_WIDTH, P_MAX_WIDTH, P_HEIGHT, P_MAX_HEIGHT, P_MIN_WIDTH, P_MIN_HEIGHT,
    P_BORDER, P_BORDER_WIDTH, P_BORDER_COLOR, P_BORDER_STYLE, P_BORDER_RADIUS,
    P_LETTER_SPACING, P_TEXT_TRANSFORM, P_OPACITY, P_WHITE_SPACE, P_BOX_SIZING,
    P_OVERFLOW, P_OVERFLOW_X, P_OVERFLOW_Y, P_MARGIN, P_MARGIN_LEFT, P_MARGIN_RIGHT
};
static const std::pair<const char*,PropId> kPropMap[] = {
    {"background-color",P_BG_COLOR},{"background-image",P_BG_IMAGE},{"background",P_BG},
    {"border-radius",P_BORDER_RADIUS},{"border-color",P_BORDER_COLOR},{"border-style",P_BORDER_STYLE},{"border-width",P_BORDER_WIDTH},{"border",P_BORDER},
    {"box-sizing",P_BOX_SIZING},{"color",P_COLOR},{"display",P_DISPLAY},
    {"float",P_FLOAT},{"font-family",P_FONT_FAMILY},{"font-size",P_FONT_SIZE},{"font-style",P_FONT_STYLE},{"font-weight",P_FONT_WEIGHT},{"font",P_FONT},
    {"height",P_HEIGHT},{"letter-spacing",P_LETTER_SPACING},{"line-height",P_LINE_HEIGHT},
    {"margin",P_MARGIN},{"margin-left",P_MARGIN_LEFT},{"margin-right",P_MARGIN_RIGHT},
    {"max-height",P_MAX_HEIGHT},{"max-width",P_MAX_WIDTH},{"min-height",P_MIN_HEIGHT},{"min-width",P_MIN_WIDTH},
    {"mso-hide",P_MSO_HIDE},{"opacity",P_OPACITY},{"overflow",P_OVERFLOW},{"overflow-x",P_OVERFLOW_X},{"overflow-y",P_OVERFLOW_Y},
    {"padding",P_PADDING},{"padding-bottom",P_PADDING_BOTTOM},{"padding-left",P_PADDING_LEFT},{"padding-right",P_PADDING_RIGHT},{"padding-top",P_PADDING_TOP},
    {"text-align",P_TEXT_ALIGN},{"text-decoration",P_TEXT_DECOR},{"text-decoration-line",P_TEXT_DECOR},{"text-transform",P_TEXT_TRANSFORM},
    {"vertical-align",P_VERT_ALIGN},{"white-space",P_WHITE_SPACE},{"width",P_WIDTH},
};
static PropId lookup_prop(const std::string &k) {
    // tiny linear scan (40 entries, branch-predicted) — faster than hash for one tag
    for (auto &p: kPropMap) if (k==p.first) return p.second;
    return P_NONE;
}
// Forward decls for dispatch
static void handle_color(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_bg(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_bg_image(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_font_size(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_font_shorthand(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_font_weight(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_font_style(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_text_decor(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_font_family(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_display(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_mso_hide(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_text_align(const std::string &v, Style &st, TextAlignment& a, bool& ha, BoxProps*, const char*, size_t);
static void handle_line_height(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_vert_align(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_float(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_padding_shorthand(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_padding_side(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_width(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_max_width(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_height(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_max_height(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_min_width(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_min_height(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_border(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_border_radius(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_letter_spacing(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_text_transform(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_opacity(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_white_space(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_box_sizing(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_overflow(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
static void handle_margin(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t);
struct PropHandler { PropId id; void (*fn)(const std::string&,Style&,TextAlignment&,bool&,BoxProps*,const char*,size_t); };
static const PropHandler kHandlers[] = {
    {P_COLOR,handle_color},{P_BG,handle_bg},{P_BG_COLOR,handle_bg},{P_BG_IMAGE,handle_bg_image},
    {P_FONT_SIZE,handle_font_size},{P_FONT,handle_font_shorthand},{P_FONT_WEIGHT,handle_font_weight},{P_FONT_STYLE,handle_font_style},{P_TEXT_DECOR,handle_text_decor},{P_FONT_FAMILY,handle_font_family},
    {P_DISPLAY,handle_display},{P_MSO_HIDE,handle_mso_hide},{P_TEXT_ALIGN,handle_text_align},{P_LINE_HEIGHT,handle_line_height},
    {P_VERT_ALIGN,handle_vert_align},{P_FLOAT,handle_float},
    {P_PADDING,handle_padding_shorthand},{P_PADDING_TOP,handle_padding_side},{P_PADDING_RIGHT,handle_padding_side},{P_PADDING_BOTTOM,handle_padding_side},{P_PADDING_LEFT,handle_padding_side},
    {P_WIDTH,handle_width},{P_MAX_WIDTH,handle_max_width},{P_HEIGHT,handle_height},{P_MAX_HEIGHT,handle_max_height},{P_MIN_WIDTH,handle_min_width},{P_MIN_HEIGHT,handle_min_height},
    {P_BORDER,handle_border},{P_BORDER_WIDTH,handle_border},{P_BORDER_COLOR,handle_border},{P_BORDER_STYLE,handle_border},{P_BORDER_RADIUS,handle_border_radius},
    {P_LETTER_SPACING,handle_letter_spacing},{P_TEXT_TRANSFORM,handle_text_transform},{P_OPACITY,handle_opacity},{P_WHITE_SPACE,handle_white_space},
    {P_BOX_SIZING,handle_box_sizing},{P_OVERFLOW,handle_overflow},{P_OVERFLOW_X,handle_overflow},{P_OVERFLOW_Y,handle_overflow},
    {P_MARGIN,handle_margin},{P_MARGIN_LEFT,handle_margin},{P_MARGIN_RIGHT,handle_margin},
};
static void (*handler_for(PropId id))(const std::string&,Style&,TextAlignment&,bool&,BoxProps*,const char*,size_t) {
    for (auto &h : kHandlers) {
        if (h.id == id)
            return h.fn;
    }
    return nullptr;
}
// Per-property implementations — moved verbatim from old if/else
static void handle_color(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ bool ok=false; NVGcolor c=parse_html_color(val.c_str(),ok); if(ok) st.fgColor=c; }
static void handle_bg(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){
    std::string lower=val; std::vector<std::string> toks; { size_t p=0; while(p<lower.size()){ while(p<lower.size() && std::isspace((unsigned char)lower[p])) ++p; if(p>=lower.size()) break; if(lower.compare(p,4,"url(")==0){ size_t q=lower.find(')',p); if(q==std::string::npos) q=lower.size()-1; toks.push_back(lower.substr(p,q-p+1)); p=q+1; } else { size_t q=lower.find_first_of(" \t",p); toks.push_back(lower.substr(p,q==std::string::npos?std::string::npos:q-p)); if(q==std::string::npos) break; p=q; } } }
    for(auto &tok:toks){ if(tok.rfind("url(",0)==0) continue; if(tok=="no-repeat"||tok=="repeat"||tok=="repeat-x"||tok=="repeat-y"||tok=="cover"||tok=="contain"||tok=="center"||tok=="left"||tok=="right"||tok=="top"||tok=="bottom"||tok.rfind("center/",0)==0) continue; bool ok=false; NVGcolor c=parse_html_color(tok.c_str(),ok); if(ok){ st.bgColor=c; break; } }
}
static void handle_bg_image(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps *box, const char *decl_raw, size_t colon) {
    (void)val; (void)st;
    if (!box)
        return;
    std::string raw = trim(std::string(decl_raw).substr(colon + 1));
    size_t up = raw.find("url(");
    if (up == std::string::npos)
        return;
    size_t ustart = up + 4;
    size_t uend = raw.find(')', ustart);
    if (uend == std::string::npos)
        return;
    std::string url = trim(raw.substr(ustart, uend - ustart));
    if (!url.empty() && (url.front() == '\'' || url.front() == '"'))
        url.erase(url.begin());
    if (!url.empty() && (url.back() == '\'' || url.back() == '"'))
        url.pop_back();
    if (!url.empty())
        box->bg_image_url = url;
}
static void handle_font_size(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ apply_font_size(v, st.fontSize); }
static void handle_font_weight(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ if(v=="bold"||v=="bolder"||atoi(v.c_str())>=600) st.bold=true; else if(v=="normal"||v=="lighter"||atoi(v.c_str())==400) st.bold=false; }
static void handle_font_style(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ if(v=="italic"||v=="oblique") st.italic=true; else if(v=="normal") st.italic=false; }
static void handle_text_decor(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ if(v.find("none")!=std::string::npos){ st.underline=false; st.strike=false; } else { if(v.find("underline")!=std::string::npos) st.underline=true; if(v.find("line-through")!=std::string::npos) st.strike=true; } }
static void handle_font_family(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ if(v.find("mono")!=std::string::npos||v.find("courier")!=std::string::npos||v.find("consol")!=std::string::npos||v.find("menlo")!=std::string::npos) st.monospace=true; }
static void handle_display(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(v=="none") st.displayNone=true; else st.displayNone=false; if(box && v=="table-cell") box->table_cell=true; if(box && (v=="inline-flex"||v=="inline-block"||v=="inline")) box->inline_flex=true; if(box && v=="inline-flex") box->is_inline_flex=true; if(box) box->display_block=(v=="block"); }
static void handle_mso_hide(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ if(v=="all") st.displayNone=true; }
static void handle_text_align(const std::string &v, Style &, TextAlignment &a, bool &ha, BoxProps*, const char*, size_t){ if(v=="center"){a=TextAlignment::Center; ha=true;} else if(v=="right"){a=TextAlignment::Right; ha=true;} else if(v=="justify"){a=TextAlignment::Justify; ha=true;} else if(v=="left"){a=TextAlignment::Left; ha=true;} }
static void handle_line_height(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ apply_line_height(v, st.lineHeight, st.fontSize); if(box){ char *e=nullptr; float n=std::strtof(v.c_str(),&e); box->zero_line_height=(e!=v.c_str() && n==0.f); } }
static void handle_vert_align(const std::string &v, Style &st, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(v=="super"){ st.superscript=true; st.verticalMiddle=false; } else if(v=="baseline"||v=="bottom"||v=="top"){ st.superscript=false; st.verticalMiddle=false; } else if(v=="middle"||v=="center"){ st.superscript=false; st.verticalMiddle=true; } if(box && (v=="middle"||v=="center")) box->align_middle=true; if(box) box->cell_valign=(v=="middle"||v=="center")?'m':(v=="bottom")?'b':(v=="top"||v=="baseline")?'t':0; }
static void handle_float(const std::string &v, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(box) box->float_left=(v=="left"); }
static void handle_padding_shorthand(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps *box, const char*, size_t) {
    std::vector<std::string> tok;
    size_t p = 0;
    while (p < val.size()) {
        while (p < val.size() && std::isspace((unsigned char)val[p]))
            ++p;
        if (p >= val.size())
            break;
        size_t q = val.find_first_of(" \t", p);
        tok.push_back(val.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos)
            break;
        p = q;
    }
    if (tok.empty())
        return;
    std::string top, right, bottom, left;
    if (tok.size() == 1) {
        top = right = bottom = left = tok[0];
    } else if (tok.size() == 2) {
        top = bottom = tok[0];
        right = left = tok[1];
    } else if (tok.size() == 3) {
        top = tok[0];
        right = left = tok[1];
        bottom = tok[2];
    } else {
        top = tok[0];
        right = tok[1];
        bottom = tok[2];
        left = tok[3];
    }
    float py = 0, pxv = 0, px1 = 0, pct1 = 0, px2 = 0, pct2 = 0;
    if (parse_css_len(top, px1, pct1) && pct1 == 0.f)
        py = std::max(py, px1);
    if (parse_css_len(bottom, px1, pct1) && pct1 == 0.f)
        py = std::max(py, px1);
    if (parse_css_len(left, px2, pct2) && pct2 == 0.f)
        pxv = std::max(pxv, px2);
    if (parse_css_len(right, px2, pct2) && pct2 == 0.f)
        pxv = std::max(pxv, px2);
    if (py > 0) {
        st.padY = std::max(st.padY, py);
        if (box) box->pad_y = std::max(box->pad_y, py);
    }
    if (pxv > 0) {
        st.padX = std::max(st.padX, pxv);
        if (box) box->pad_x = std::max(box->pad_x, pxv);
    }
}
thread_local PropId t_last_pad_side=P_NONE;
thread_local PropId t_last_border=P_BORDER;
thread_local PropId t_last_overflow=P_OVERFLOW;
static void handle_padding_side(const std::string &valRaw, Style &st, TextAlignment&, bool&, BoxProps *box, const char*, size_t colon_unused){
    (void)colon_unused; // side is encoded in valRaw? We dispatch via PropId so need key. Instead peek key via st trick: caller passes val, not key. So split by PropId at call: we handle generically.
    // This entry is called only for single side; the key distinction is which Pad it sets — we collapse to max based on which handler was chosen.
    // To avoid key re-parse, we use a thread_local last_key stored by dispatcher (set before call).
    PropId side=t_last_pad_side; float px=0,pct=0; if(!parse_css_len(valRaw,px,pct)||pct!=0.f) return; if(side==P_PADDING_TOP||side==P_PADDING_BOTTOM){ st.padY=std::max(st.padY,px); if(box) box->pad_y=std::max(box->pad_y,px);} else { st.padX=std::max(st.padX,px); if(box) box->pad_x=std::max(box->pad_x,px);} }
static void handle_width(const std::string &v, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; float px=0,pct=0; if(!parse_css_len(v,px,pct)) return; if(pct>0) box->width_pct=pct; else box->width_px=px; }
static void handle_max_width(const std::string &v, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; float px=0,pct=0; if(!parse_css_len(v,px,pct)) return; if(pct<=0) box->max_width_px=px; else if(box->width_pct<=0) box->width_pct=pct; }
static void handle_height(const std::string &v, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; float px=0,pct=0; if(!parse_css_len(v,px,pct)) return; if(pct<=0) box->height_px=px; }
static void handle_max_height(const std::string &v, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; float px=0,pct=0; if(!parse_css_len(v,px,pct)) return; if(pct<=0) box->max_height_px=px; }
static void handle_min_width(const std::string &v, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; float px=0,pct=0; if(parse_css_len(v,px,pct)&&pct==0.f) box->min_width_px=px; }
static void handle_min_height(const std::string &v, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; float px=0,pct=0; if(parse_css_len(v,px,pct)&&pct==0.f) box->min_height_px=px; }
static void handle_border(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){
    // covers border, border-width, border-color, border-style — same body split into border vs others happens in dispatcher via key, but reuse
    // Determine which sub-key we are by thread_local (see dispatcher). For generic border, parse width||color||style.
    PropId k=t_last_border; if(val=="none"||val=="0"||val=="0px"||val.find("none")==0){ if(k!=P_BORDER_COLOR) st.borderWidth=0; return; }
    size_t rp=0; while(rp<val.size()){ while(rp<val.size()&&std::isspace((unsigned char)val[rp])) ++rp; if(rp>=val.size()) break; size_t sp=val.find_first_of(" \t",rp); std::string tok=val.substr(rp, sp==std::string::npos?std::string::npos:sp-rp); rp=(sp==std::string::npos)?val.size():sp+1; if(tok=="solid"||tok=="dashed"||tok=="dotted"||tok=="double"||tok=="groove"||tok=="ridge") continue; float px=0,pct=0; if(parse_css_len(tok,px,pct)&&pct==0.f){ st.borderWidth=px; continue; } bool ok=false; NVGcolor c=parse_html_color(tok.c_str(),ok); if(ok){ st.borderColor=c; if(st.borderWidth<=0.f && k==P_BORDER_COLOR) st.borderWidth=1; } }
}
static void handle_border_radius(const std::string &val, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; float px=0,pct=0; size_t sp=val.find_first_of(" \t"); std::string a=(sp==std::string::npos)?val:val.substr(0,sp); if(parse_css_len(a,px,pct)){ if(pct>0) box->radius_px=9999.f; else if(px>0) box->radius_px=px; } }
static void handle_letter_spacing(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ float px=0,pct=0; if(parse_css_len(val,px,pct)&&pct==0.f) st.letterSpacing=std::max(st.letterSpacing,px); }
static void handle_text_transform(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ if(val.find("uppercase")!=std::string::npos) st.allCaps=true; else if(val.find("none")!=std::string::npos) st.allCaps=false; }
static void handle_opacity(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ char *e=nullptr; float v=strtof(val.c_str(),&e); if(e!=val.c_str()) st.opacity=std::clamp(v,0.f,1.f); }
static void handle_white_space(const std::string &val, Style &st, TextAlignment&, bool&, BoxProps*, const char*, size_t){ if(val=="nowrap") st.whiteSpace=WhiteSpace::Nowrap; else if(val=="pre"||val=="pre-wrap") st.whiteSpace=WhiteSpace::Pre; else if(val=="normal") st.whiteSpace=WhiteSpace::Normal; }
static void handle_box_sizing(const std::string &val, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; if(val=="border-box") box->border_box=true; else if(val=="content-box") box->border_box=false; }
static void handle_overflow(const std::string &val, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; PropId k=t_last_overflow; (void)k; if(val=="hidden"||val=="clip") box->overflow_hidden=true; else if(val=="visible"||val=="auto"||val=="scroll") box->overflow_hidden=false; }
static void handle_margin(const std::string &val, Style&, TextAlignment&, bool&, BoxProps *box, const char*, size_t){ if(!box) return; if(val.find("auto")!=std::string::npos) box->center=true; }
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

        PropId pid = lookup_prop(key);
        if (pid == P_NONE) continue;
        if (pid==P_PADDING_TOP||pid==P_PADDING_RIGHT||pid==P_PADDING_BOTTOM||pid==P_PADDING_LEFT) t_last_pad_side=pid;
        if (pid==P_BORDER||pid==P_BORDER_WIDTH||pid==P_BORDER_COLOR||pid==P_BORDER_STYLE) t_last_border=pid;
        if (pid==P_OVERFLOW||pid==P_OVERFLOW_X||pid==P_OVERFLOW_Y) t_last_overflow=pid;
        auto fn = handler_for(pid);
        if (fn) fn(val, st, align, has_align, box, decl.c_str(), colon);
        // alias: background and background-color share P_BG* -> already mapped, no extra
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
    /* Heading level of the element currently being walked. brk() keeps
     * it so a <br> inside <h1> starts another heading paragraph. */
    int            pending_header = 0;
    /* The last thing walked was a <br>: the next paragraph is a soft line
     * break (Paragraph::softBreak), not a new block. */
    bool           pending_br = false;

    Paragraph *para() {
        if (!cur) {
            cur = doc.addParagraph();
            cur->alignment = align;
            cur->headerLevel = pending_header;
            cur->softBreak = pending_br;
            cur_has_text = false;
            pending_br = false;
        }
        return cur;
    }
    /* End the current block: the next text starts a new paragraph.
     * Does not clear pending_header. */
    void brk() { cur = nullptr; cur_has_text = false; pending_br = false; }

    /* <br>: the next text goes on a new line of the same block, with no
     * paragraph gap.  The second <br> of <br><br> leaves a blank line in
     * the surrounding font, as browsers do.  A <br> right after a block
     * boundary keeps the existing paragraph gap and adds nothing. */
    void lineBreak(const Style &st) {
        if (!cur && !pending_br)
            return;
        if (!cur)
            para()->addText(Text("", st));
        brk();
        pending_br = true;
    }

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
        if (st.allCaps) {
            for (char &c : t) c = (char)std::toupper((unsigned char)c);
        }
        Text text(t, st);
        if (!st.linkUrl.empty()) text.linkUrl = st.linkUrl;
        para()->addText(text);
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
// Secure URL validation: only allow http/https/mailto. Decodes %XX, rejects control chars and javascript:.
static std::string url_decode(const std::string &s) {
    std::string out; out.reserve(s.size());
    for (size_t i=0;i<s.size();++i) {
        if (s[i]=='%' && i+2<s.size() && std::isxdigit((unsigned char)s[i+1]) && std::isxdigit((unsigned char)s[i+2])) {
            char hex[3]={s[i+1],s[i+2],0}; char *e; long v=strtol(hex,&e,16); if(e==hex+2 && v>=0) { out.push_back((char)v); i+=2; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}
static bool is_allowed_url(const std::string &raw, std::string &normalized) {
    std::string s = url_decode(raw);
    // trim
    size_t b=s.find_first_not_of(" \t\r\n"), e=s.find_last_not_of(" \t\r\n");
    if(b==std::string::npos){normalized.clear(); return false;}
    s=s.substr(b,e-b+1);
    if(s.empty()) return false;
    for(char c: s) if((unsigned char)c < 32) return false; // no control
    std::string lower; lower.reserve(s.size()); for(char c:s) lower.push_back(std::tolower((unsigned char)c));
    if(lower.rfind("javascript:",0)==0 || lower.rfind("data:",0)==0 || lower.rfind("vbscript:",0)==0) return false;
    if(lower.rfind("https://",0)==0 || lower.rfind("http://",0)==0) { normalized=s; return true; }
    if(lower.rfind("mailto:",0)==0) { normalized=s; return true; }
    return false;
}
static bool open_url_secure(const std::string &raw) {
    std::string url;
    if (!is_allowed_url(raw, url)) return false;
#if defined(_WIN32)
    return (int)(intptr_t)ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL) > 32;
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd>2) close(fd); }
#if defined(__APPLE__)
        execlp("open", "open", url.c_str(), (char*)nullptr);
#else
        execlp("xdg-open", "xdg-open", url.c_str(), (char*)nullptr);
        execlp("gio", "gio", "open", url.c_str(), (char*)nullptr);
        execlp("sensible-browser", "sensible-browser", url.c_str(), (char*)nullptr);
        execlp("x-www-browser", "x-www-browser", url.c_str(), (char*)nullptr);
#endif
        _exit(127);
    }
    if (pid > 0) {
        fprintf(stderr, "[nmail] open_url_secure: forked pid=%d for url=%s\n", (int)pid, url.c_str());
        fflush(stderr);
        return true;
    }
    return false;
#endif
}

class HtmlBlock;

static std::string ancestor_block_link(const Widget *from);

/* Press and release can both try to open the same address: the leaf
 * gets the release, then the block under the pointer does too. */
static bool follow_link(Widget *from, const std::string &url) {
    if (url.empty()) return false;
    static double last_t = -1.0;
    static std::string last_u;
    double now = glfwGetTime();
    if (last_t >= 0.0 && url == last_u && now - last_t < 0.05)
        return true;
    last_t = now;
    last_u = url;
    for (Widget *pp = from; pp; pp = pp->parent())
        if (auto *hd = dynamic_cast<HtmlDocument *>(pp)) {
            if (hd->notify_link_click(url)) return true;
            break;
        }
    std::string n;
    if (is_allowed_url(url, n)) {
        open_url_secure(n);
        return true;
    }
    return false;
}

class HtmlText : public Widget {
public:
    Document m_doc;
    NVGcolor m_bg;
    int      m_measured_h = 0;
    int      m_measured_w = -1;
    int      m_natural_w  = -1;  // cached max-content width
    mutable std::string m_hover_url;

    HtmlText(Widget *parent, Document &&doc, NVGcolor bg)
        : Widget(parent), m_doc(std::move(doc)), m_bg(bg) {
        set_live(true);
    }

    /* Caret lives on one leaf at a time. Image and rule paragraphs are
     * not typed into; keystrokes open a text paragraph beside them. */
    bool  m_have_caret = false;
    size_t m_caret_para = 0;
    size_t m_caret_col  = 0;
    bool  m_typing_on = false;
    Style m_typing;
    double m_last_click = -1.0;
    int    m_clicks = 0;
    Vector2i m_down_pt;
    bool   m_press_dragged = false;
    int    m_press_travel = 0;
    bool   m_press_shift = false;
    std::string m_press_url;

    HtmlDocument *owner() const;
    bool editing() const;
    void ensure_layout(NVGcontext *ctx);
    void solo_caret();
    void place_caret(size_t para, size_t col, bool extend = false);
    Style style_for_insert() const;
    void insert_text(const std::string &s);
    void insert_newline();
    void ensure_text_para();
    void break_paragraph();
    void delete_backward();
    void delete_forward();
    void move_caret(int key, int mods);
    void move_to_leaf(HtmlText *ht, bool at_end, bool extend = false);
    void erase_span(size_t p0, size_t c0, size_t p1, size_t c1);
    void erase_to_end(size_t para, size_t col);
    void erase_from_start(size_t para, size_t col);
    void mark_dirty_leaf();
    void draw_selection(NVGcontext *ctx);
    void toggle_inline(bool Style::*flag);
    void toggle_header_level(int level);
    void toggle_bullet_para();
    void toggle_code_para();
    void after_edit();
    void after_move();
    void scroll_caret();
    bool keyboard_event(int key, int scancode, int action, int mods) override;
    bool keyboard_character_event(unsigned int codepoint) override;
    bool mouse_drag_event(const Vector2i &p, const Vector2i &rel,
                          int button, int mods) override;

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
        draw_selection(ctx);
        if (m_have_caret && editing() && focused()) {
            Document::CaretInfo info = m_doc.richCaretInfo(ctx, m_caret_para, m_caret_col);
            if (info.valid) {
                float cx = info.x;
                float y0 = info.y_top;
                float y1 = std::max(info.y_bottom, info.y_top + 12.f);
                nvgBeginPath(ctx);
                nvgRect(ctx, cx, y0, 1.5f, y1 - y0);
                nvgFillColor(ctx, nvgRGBA(80, 140, 255, 255));
                nvgFill(ctx);
            }
        }
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
    /* p is parent space, same as richHitTest. Words sit at the layout
     * origin: m_pos after paint, 0 after a measure-only pass. */
    std::string hit_url(const Vector2i &p) const {
        if (m_doc.m_rich_layout.empty()) return "";
        float x = (float)p.x() - (float)m_pos.x() + m_doc.m_layout_origin_x;
        float y = (float)p.y() - (float)m_pos.y() + m_doc.m_layout_origin_y;
        const Document::RichLine *found=nullptr;
        for(auto &rl: m_doc.m_rich_layout){ if(y>=rl.y_top-2 && y<rl.y_bottom+2){ found=&rl; break; } }
        if(!found){ // scrolled: try absolute as fallback
            Vector2i abs = absolute_position(); float ax=(float)abs.x()+(float)p.x(); float ay=(float)abs.y()+(float)p.y();
            for(auto &rl: m_doc.m_rich_layout){ if(ay>=rl.y_top-2 && ay<rl.y_bottom+2){ found=&rl; x=ax; break; } }
            if(!found) return "";
        }
        for(auto &w: found->words){ if(!w.linkUrl.empty() && x>=w.x-2 && x< w.x+w.advance+2) return w.linkUrl; }
        return "";
    }
    void notify_hover(const std::string &url) {
        for (Widget *pp = parent(); pp; pp = pp->parent())
            if (auto *hd = dynamic_cast<HtmlDocument *>(pp)) { hd->set_hover_url(url); break; }
    }
    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int b, int m) override {
        // set_cursor on the widget under the pointer; Screen::cursor_pos_callback_event copies widget->cursor() to GLFW.
        // Return true when hovering a link so the event is considered handled and not swallowed.
        if (editing()) {
            set_cursor(Cursor::IBeam);
            return true;
        }
        std::string url = hit_url(p);
        m_hover_url = url;
        bool is_link = !url.empty();
        set_cursor(is_link ? Cursor::Hand : Cursor::IBeam);
        notify_hover(url);
        bool handled = Widget::mouse_motion_event(p, rel, b, m);
        return is_link || handled;
    }
    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        if (editing()) {
            set_cursor(enter ? Cursor::IBeam : Cursor::Arrow);
            return Widget::mouse_enter_event(p, enter);
        }
        if(!enter){ set_cursor(Cursor::Arrow); m_hover_url.clear(); notify_hover(""); }
        else { std::string u=hit_url(p); m_hover_url=u; if(!u.empty()) set_cursor(Cursor::Hand); else set_cursor(Cursor::IBeam); notify_hover(u); }
        return Widget::mouse_enter_event(p, enter);
    }
    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int mods) override {
        if (button != GLFW_MOUSE_BUTTON_1)
            return Widget::mouse_button_event(p, button, down, mods);
        HtmlDocument *hd = owner();
        if (!hd) return Widget::mouse_button_event(p, button, down, mods);
        if (down) {
            request_focus();
            NVGcontext *ctx = screen() ? screen()->nvg_context() : nullptr;
            size_t para = 0, col = 0;
            if (ctx) {
                ensure_layout(ctx);
                /* p and the document words are both in this leaf's
                 * parent space (draw origin is m_pos). */
                std::pair<size_t, size_t> hit =
                    m_doc.richHitTest(ctx, (float)p.x(), (float)p.y());
                para = hit.first;
                col = hit.second;
            }
            double now = glfwGetTime();
            bool near = std::abs(p.x() - m_down_pt.x()) +
                        std::abs(p.y() - m_down_pt.y()) <= 6;
            bool shift = (mods & GLFW_MOD_SHIFT) != 0;
            if (!shift && m_last_click >= 0.0 && now - m_last_click < 0.4 && near) {
                if (m_clicks < 3) ++m_clicks;
            } else {
                m_clicks = 1;
            }
            m_last_click = now;
            m_down_pt = p;
            m_press_dragged = false;
            m_press_travel = 0;
            m_press_shift = shift;
            m_press_url.clear();
            /* Remember the address under the press. Mouse-up may arrive in
             * another coordinate space once the pointer leaves the leaf. */
            if (!editing() && !shift && m_clicks <= 1) {
                m_press_url = hit_url(p);
                if (m_press_url.empty())
                    m_press_url = ancestor_block_link(this);
            }
            if (shift || m_clicks <= 1)
                place_caret(para, col, shift);
            else if (m_clicks == 2)
                hd->select_word_at(this, para, col);
            else
                hd->select_paragraph_at(this, para);
            after_move();
            return true;
        }
        /* A drag, a multi-click, or a live selection stays a selection. */
        if (editing() || m_press_shift || m_press_dragged || m_clicks > 1 ||
            hd->has_text_selection())
            return true;
        if (follow_link(this, m_press_url))
            return true;
        return false;
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
    std::string m_link_url; // if this block is inside <a href> (block_anchor)

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
    void notify_hover_block(const std::string &url) {
        for (Widget *pp = parent(); pp; pp = pp->parent())
            if (auto *hd = dynamic_cast<HtmlDocument *>(pp)) { hd->set_hover_url(url); break; }
    }
    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i&, int, int) override {
        if(!m_link_url.empty()) { set_cursor(Cursor::Hand); notify_hover_block(m_link_url); }
        else { set_cursor(Cursor::Arrow); notify_hover_block(""); }
        return Widget::mouse_motion_event(p, Vector2i(0,0), 0, 0);
    }
    virtual bool mouse_enter_event(const Vector2i &, bool enter) override {
        if(!m_link_url.empty()) { set_cursor(enter ? Cursor::Hand : Cursor::Arrow); notify_hover_block(enter ? m_link_url : ""); }
        else { set_cursor(Cursor::Arrow); if(!enter) notify_hover_block(""); else notify_hover_block(""); }
        return Widget::mouse_enter_event(Vector2i(0,0), enter);
    }
    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int mods) override {
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
            Widget *child = *it;
            if (child->visible() && child->contains(p - m_pos)) {
                if (child->mouse_button_event(p - m_pos, button, down, mods)) return true;
            }
        }
        if(!m_link_url.empty() && button==GLFW_MOUSE_BUTTON_1 && !down){
            if (follow_link(this, m_link_url)) return true;
        }
        return false;
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
        bool bg_image_pending = m_bg_image <= 0 && !m_bg_image_src.empty();
        if (m_bg.a > 0.0f || m_bg_image > 0 || bg_image_pending) {
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
            } else if (bg_image_pending && w > 0.0f && h > 0.0f) {
                /* CSS background-image that hasn't resolved (still
                 * loading, or the fetch failed) — a neutral gray tile,
                 * same look as a plain <img> that hasn't loaded, instead
                 * of leaving the box fully invisible. */
                nvgBeginPath(ctx);
                if (rad > 0.5f)
                    nvgRoundedRect(ctx, x, y, w, h, rad);
                else
                    nvgRect(ctx, x, y, w, h);
                nvgFillColor(ctx, nvgRGBA(128, 128, 136, 40));
                nvgFill(ctx);
                nvgBeginPath(ctx);
                if (rad > 0.5f)
                    nvgRoundedRect(ctx, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, rad);
                else
                    nvgRect(ctx, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f);
                nvgStrokeColor(ctx, nvgRGBA(128, 128, 136, 90));
                nvgStrokeWidth(ctx, 1.0f);
                nvgStroke(ctx);
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

static std::string ancestor_block_link(const Widget *from) {
    for (const Widget *w = from; w; w = w->parent()) {
        if (dynamic_cast<const HtmlDocument *>(w))
            break;
        if (auto *b = dynamic_cast<const HtmlBlock *>(w))
            if (!b->m_link_url.empty())
                return b->m_link_url;
    }
    return {};
}

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
    // Rule bucket index — built once per set_html to avoid O(rules) scan per element.
    mutable std::vector<const CssRule*> universal;
    mutable std::unordered_map<std::string, std::vector<const CssRule*>> by_id;
    mutable std::unordered_map<std::string, std::vector<const CssRule*>> by_class;
    mutable std::unordered_map<std::string, std::vector<const CssRule*>> by_tag;
    mutable std::unordered_map<std::string, std::vector<const CssRule*>> by_ancestor_class;
    mutable std::unordered_map<std::string, std::vector<const CssRule*>> by_ancestor_tag;
    mutable bool indexed = false;
    void build_index() const;
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
    /* Minimum width for rows of the current table when a CSS width:Npx
     * table is block-aligned (-webkit-center/-webkit-right) — pins the
     * shrink-wrapped rows to the exact CSS width (the 188px USAA security
     * stamp).  Nested tables save/restore this.  0 = content-sized. */
    int           table_row_min_w = 0;
};

const char *attr(GumboElement *el, const char *name) {
    GumboAttribute *a = gumbo_get_attribute(&el->attributes, name);
    return (a && a->value) ? a->value : nullptr;
}

/* Custom / unknown tag name, lowercased. HTML5 slots like <nmail-widget>
 * arrive as GUMBO_TAG_UNKNOWN; original_tag holds the source spelling. */
std::string gumbo_tag_name(GumboElement *el) {
    if (el->tag != GUMBO_TAG_UNKNOWN) {
        const char *n = gumbo_normalized_tagname(el->tag);
        return n ? n : "";
    }
    GumboStringPiece t = el->original_tag;
    gumbo_tag_from_original_text(&t);
    if (!t.data || t.length == 0)
        return "";
    std::string n(t.data, t.length);
    for (char &c : n)
        c = (char)std::tolower((unsigned char)c);
    return n;
}

/* Host slot: <nmail-widget id="..."> or any element with data-nmail-widget. */
bool host_embed_id(GumboElement *el, std::string &id) {
    if (const char *d = attr(el, "data-nmail-widget")) {
        id = d;
        return !id.empty();
    }
    if (el->tag == GUMBO_TAG_UNKNOWN && gumbo_tag_name(el) == "nmail-widget") {
        const char *v = attr(el, "id");
        id = v ? v : "";
        return true;
    }
    return false;
}

void try_host_embed(Widget *container, GumboElement *el, const std::string &id,
                    Builder &B) {
    if (!B.view || !B.view->embed_widget)
        return;
    HtmlEmbedSpec spec;
    spec.id = id;
    for (unsigned i = 0; i < el->attributes.length; ++i) {
        GumboAttribute *a = (GumboAttribute *)el->attributes.data[i];
        if (a && a->name)
            spec.attrs.emplace_back(a->name, a->value ? a->value : "");
    }
    B.view->embed_widget(container, spec);
}

// ---------------------------------------------------------------------------
// Stylesheet: <style> blocks from the mail (class / id / element selectors,
// prefers-color-scheme media). Inline style="" still wins.
// ---------------------------------------------------------------------------
void collect_style_text(GumboNode *node, std::string &out, int depth = 0) {
    if (!node || depth > 200) return;
    if (node->type == GUMBO_NODE_DOCUMENT) {
        GumboVector *kids = &node->v.document.children;
        for (unsigned i = 0; i < kids->length; ++i)
            collect_style_text((GumboNode *)kids->data[i], out, depth + 1);
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
        collect_style_text((GumboNode *)el->children.data[i], out, depth + 1);
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

static std::string strip_pseudo(const std::string &s) {
    // Drop trailing :hover/:first-child etc. and [attr] — keep base before them.
    size_t c = s.find('[');
    size_t p = s.find(':');
    size_t cut = std::min(c==std::string::npos? s.size():c, p==std::string::npos? s.size():p);
    return trim_lower(s.substr(0, cut));
}

bool parse_simple_selector(const std::string &raw, CssSel &out) {
    // Gmail/Outlook data-ogsc hack: [data-ogsc] or [data-ogsb] wraps dark-mode overrides.
    // Those rules are for Gmail's custom dark mode engine and must be ignored in our
    // normal renderer (otherwise white link colors leak onto white background, e.g.
    // bookAuthorName a => #fff on #fff invisible author name).
    std::string lower0 = trim_lower(raw);
    if (lower0.find("[data-og") != std::string::npos) return false;
    std::string s = lower0;
    if (s.empty()) return false;
    // Strip trailing pseudo/attr bracket remainder if present
    // Do per-token so "a[x-apple-data-detectors]" -> "a"
    // Split descendant chain "table td a" -> last token is target
    // Collect ancestor tokens (before last space) as disjunctive must-have-set
    // We keep only first ancestor class/tag (compat) but accept multi-depth via loose check.
    // Normalize "a > b" -> "a b", commas already split upstream
    { std::string t; for (char c: s) t += (c=='>'||c=='+'||c=='~')?' ':c; s=t; }
    // remove bracket and pseudo tails per token
    { std::vector<std::string> toks; size_t p=0; while(p<s.size()){ while(p<s.size()&&std::isspace((unsigned char)s[p])) ++p; if(p>=s.size()) break; size_t q=s.find_first_of(" \t",p); std::string tok=s.substr(p,q==std::string::npos?std::string::npos:q-p); std::string base=strip_pseudo(tok); // keep base tag/class/id part
            // but keep class/id inside [] stripped tok: e.g. ".foo" stays, "a[href]" -> "a"
            // For "*", keep empty tag.
            toks.push_back(base.empty()? std::string():base); if(q==std::string::npos) break; p=q; }
        if (toks.empty()) return false;
        if (toks.size()>=2) {
            // ancestor exists — try to parse ancestor's last token's class/tag
            std::string anc = toks[toks.size()-2];
            std::string rest = toks.back();
            if (rest.empty()) return false;
            CssSel a, b;
            // For multi-depth, stitch ancestor as the immediate previous; deeper ancestors are treated as disjunctive
            // but we only store one — matches "table td a" if any parent has class/tag of anc
            // Parse each token as simple (tag + .class/.#id)
            auto parseTok = [&](const std::string &tok, CssSel &o)->bool{
                o=CssSel{}; if(tok.empty()) return false; size_t i=0; if(tok[0]!='.'&&tok[0]!='#'){ size_t j=i; while(j<tok.size()&&(std::isalnum((unsigned char)tok[j])||tok[j]=='-'||tok[j]=='_')) ++j; o.tag=tok.substr(0,j); if(o.tag=="*") o.tag.clear(); i=j; } while(i<tok.size()){ if(tok[i]=='.'||tok[i]=='#'){ char k=tok[i++]; size_t j=i; while(j<tok.size()&&(std::isalnum((unsigned char)tok[j])||tok[j]=='-'||tok[j]=='_')) ++j; if(j==i) return false; std::string id=tok.substr(i,j-i); if(k=='.') o.classes.push_back(id); else o.id=id; i=j; } else return false; } if(o.tag.empty()&&o.id.empty()&&o.classes.empty()) return false; o.spec=(o.id.empty()?0:100)+(int)o.classes.size()*10+(o.tag.empty()?0:1); return true; };
            if (!parseTok(anc,a) || !parseTok(rest,b)) return false;
            out=b;
            if (!a.classes.empty()) out.ancestor_class=a.classes[0];
            else if (!a.tag.empty()) out.ancestor_tag=a.tag;
            else if (!a.id.empty()) out.ancestor_class=a.id; else return false;
            out.spec+=10; return true;
        } else {
            s = toks[0];
        }
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

void Stylesheet::build_index() const {
    if (indexed) return;
    indexed = true;
    universal.clear(); by_id.clear(); by_class.clear();
    by_tag.clear(); by_ancestor_class.clear(); by_ancestor_tag.clear();
    for (const auto &r : rules) {
        bool placed = false;
        if (!r.sel.id.empty()) { by_id[r.sel.id].push_back(&r); placed = true; }
        for (auto &cl : r.sel.classes) { by_class[cl].push_back(&r); placed = true; }
        if (!r.sel.tag.empty()) { by_tag[r.sel.tag].push_back(&r); placed = true; }
        // If no primary key, it is universal or ancestor-only — bucket under ancestor for filtering
        if (!placed) {
            if (!r.sel.ancestor_class.empty()) by_ancestor_class[r.sel.ancestor_class].push_back(&r);
            else if (!r.sel.ancestor_tag.empty()) by_ancestor_tag[r.sel.ancestor_tag].push_back(&r);
            else universal.push_back(&r);
        } else {
            // Rules with a primary key plus an ancestor selector also need ancestor index
            if (!r.sel.ancestor_class.empty()) by_ancestor_class[r.sel.ancestor_class].push_back(&r);
            if (!r.sel.ancestor_tag.empty()) by_ancestor_tag[r.sel.ancestor_tag].push_back(&r);
        }
    }
}

void parse_stylesheet(const std::string &css, Stylesheet &ss) {
    std::string s = strip_css_comments(css);
    size_t i = 0;
    int order = 0;
    ss.indexed = false;
    parse_css_rules(s, i, s.size(), CssMedia::Any, ss, order);
    ss.build_index();
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

// Per-set_html memo: avoids 400k allocs (100 rules × 500 elements) when
// css_sel_matches called from apply_cascade for every node.
// Class spans are offsets into classes_buf. string_view into that buffer
// dangles once the memo is moved, and a hash node that holds the text
// makes rehash load the bytes (an "https://" value faulted on macOS).
struct ElemMemo {
    std::string tag; // lower
    std::string id;  // lower, empty if none
    std::string classes_buf; // lowercased "a b c" (single alloc)
    struct Span { uint32_t off, len; };
    std::vector<Span> classes;
    bool has = false;

    bool has_class(std::string_view need) const {
        for (Span s : classes) {
            if (s.len != need.size()) continue;
            if (std::memcmp(classes_buf.data() + s.off, need.data(), s.len) == 0)
                return true;
        }
        return false;
    }
};
/* Strings live in the deque. The hash table only stores an index, so
 * rehash never walks a node whose first bytes are URL text. push_back
 * does not move existing deque elements. */
static thread_local std::deque<ElemMemo> g_elem_store;
static thread_local std::unordered_map<GumboElement*, uint32_t> g_elem_index;

static const ElemMemo &elem_memo(GumboElement *el) {
    auto it = g_elem_index.find(el);
    if (it != g_elem_index.end()) return g_elem_store[it->second];
    ElemMemo m;
    const char *n = gumbo_normalized_tagname(el->tag);
    m.tag = (n && *n) ? trim_lower(n) : std::string{};
    const char *cid = attr(el, "id");
    m.id = cid ? trim_lower(cid) : std::string{};
    const char *c = attr(el, "class");
    if (c) {
        m.classes_buf = trim_lower(c);
        size_t i2 = 0;
        while (i2 < m.classes_buf.size()) {
            while (i2 < m.classes_buf.size() && std::isspace((unsigned char)m.classes_buf[i2])) ++i2;
            size_t j = i2;
            while (j < m.classes_buf.size() && !std::isspace((unsigned char)m.classes_buf[j])) ++j;
            if (j > i2 && j - i2 <= 0xffffffffu)
                m.classes.push_back({ (uint32_t)i2, (uint32_t)(j - i2) });
            i2 = j;
        }
    }
    m.has = true;
    uint32_t idx = (uint32_t)g_elem_store.size();
    g_elem_store.push_back(std::move(m));
    g_elem_index.emplace(el, idx);
    return g_elem_store[idx];
}
static inline void elem_memo_clear() {
    g_elem_index.clear();
    g_elem_store.clear();
}

bool css_sel_matches(const CssSel &sel, GumboElement *el, GumboNode *node) {
    const ElemMemo &m = elem_memo(el);
    if (!sel.tag.empty() && sel.tag != m.tag)
        return false;
    if (!sel.id.empty() && sel.id != m.id)
        return false;
    if (!sel.classes.empty()) {
        for (const auto &need : sel.classes) {
            if (!m.has_class(need)) return false;
        }
    }
    if (!sel.ancestor_class.empty() || !sel.ancestor_tag.empty()) {
        bool ok = false;
        for (GumboNode *p = node ? node->parent : nullptr; p; p = p->parent) {
            if (p->type != GUMBO_NODE_ELEMENT) continue;
            GumboElement *pe = &p->v.element;
            if (!sel.ancestor_class.empty()) {
                const ElemMemo &pm = elem_memo(pe);
                if (pm.has_class(sel.ancestor_class)) { ok = true; break; }
            }
            if (!sel.ancestor_tag.empty() &&
                elem_memo(pe).tag == sel.ancestor_tag) {
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
    if (!sheet.indexed) sheet.build_index();
    struct Hit { int spec, order; const CssRule *r; };
    std::vector<Hit> hits;
    hits.reserve(8);
    const ElemMemo &mm = elem_memo(el);
    // Fast-path: collect bucketed candidates and dedup via sort+unique (no hash).
    std::vector<const CssRule*> cand;
    cand.reserve(32);
    auto add_bucket = [&](const std::vector<const CssRule*> &b){
        cand.insert(cand.end(), b.begin(), b.end());
    };
    if (!mm.id.empty()) { auto it = sheet.by_id.find(mm.id); if (it!=sheet.by_id.end()) add_bucket(it->second); }
    for (ElemMemo::Span s : mm.classes) {
        auto it = sheet.by_class.find(mm.classes_buf.substr(s.off, s.len));
        if (it != sheet.by_class.end()) add_bucket(it->second);
    }
    if (!mm.tag.empty()) { auto it = sheet.by_tag.find(mm.tag); if (it!=sheet.by_tag.end()) add_bucket(it->second); }
    add_bucket(sheet.universal);
    // by_ancestor_* duplicates are already in by_class/tag buckets above — no extra union.
    if (!cand.empty()) {
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        for (const CssRule *rp : cand) {
            const auto &r = *rp;
            if (r.media == CssMedia::Skip) continue;
            if (r.media == CssMedia::Dark && !dark) continue;
            if (r.media == CssMedia::Light && dark) continue;
            // For rules that were only ancestor-keyed (should be in universal already)
            // css_sel_matches does the ancestor walk; low cost now that cand is small.
            if (!css_sel_matches(r.sel, el, node)) continue;
            hits.push_back({ r.sel.spec, r.order, &r });
        }
    }
    // Rescue ancestor-only rules that were solely under by_ancestor_* and not yet seen.
    // They are genuinely universal w.r.t the leaf (e.g. ".ancestor .leaf" where leaf has
    // no id/class/tag match). Probe those buckets directly on miss.
    if (hits.empty() && cand.size() < sheet.rules.size()) {
        auto probe_ancestor = [&](const auto &map){
            for (auto &kv : map) {
                for (auto *rp : kv.second) {
                    if (std::binary_search(cand.begin(), cand.end(), rp)) continue;
                    const auto &r=*rp;
                    if (r.media==CssMedia::Skip) continue;
                    if (r.media==CssMedia::Dark && !dark) continue;
                    if (r.media==CssMedia::Light && dark) continue;
                    if (!css_sel_matches(r.sel, el, node)) continue;
                    hits.push_back({r.sel.spec, r.order, &r});
                    cand.push_back(rp); // mark seen for second map
                }
            }
        };
        // Sort cand first so binary_search above is valid; if we added, keep sorted invariant cheap
        // (size small, just sort again before second probe)
        if (!cand.empty()) std::sort(cand.begin(), cand.end());
        probe_ancestor(sheet.by_ancestor_class);
        if (!cand.empty()) std::sort(cand.begin(), cand.end());
        probe_ancestor(sheet.by_ancestor_tag);
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
    // linkUrl intentionally NOT cleared — inherited from <a> ancestor so
    // nested <b> inside <a> retains the href for hit-testing.
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
    /* Host parchment card. Message rules (text-align:right !important,
     * margin:auto, a fixed width) must not move it. Inline !important
     * already ran; this still wins when the rule is on an ancestor and
     * the paragraph stamps that inherited alignment. */
    if (const char *chrome = attr(el, "data-nmail-chrome")) {
        align = TextAlignment::Left;
        st.displayNone = false;
        if (box) {
            box->center = false;
            box->float_left = false;
            box->inline_flex = false;
            box->is_inline_flex = false;
            box->align_middle = false;
            box->max_width_px = 0.f;
            box->width_px = 0.f;
            box->width_pct = std::string(chrome) == "root" ? 100.f : 0.f;
        }
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
    if (box.table_cell) {
        if (box.width_px > 0.0f) return box.width_px;
        // Unlayer: class "u-col-20" / "u-col-28" / "u-col-32" encodes % weight.
        // Also "u-col-25p33" -> 25.33 . Use it as weight so MENU/CATERING/ABOUT/LOCATIONS split 20/28/20/32.
        const char *cls = attr(el, "class");
        if (cls) {
            std::string s = trim_lower(cls);
            size_t q = s.find("u-col-");
            if (q != std::string::npos) {
                std::string tail = s.substr(q + 6);
                std::string num;
                for (char c : tail) { if (std::isdigit((unsigned char)c) || c=='p' || c=='.') num+=c; else break; }
                for (char &c : num) if (c=='p') c='.';
                char *e=nullptr; float v=strtof(num.c_str(), &e);
                if (e!=num.c_str() && css_finite(v) && v > 0.f && v < 200.f) return v;
            }
        }
        return 1.0f;
    }
    const char *cls = attr(el, "class");
    if (cls) {
        std::string s = trim_lower(cls);
        size_t p = s.find("mj-column-per-");
        if (p != std::string::npos) {
            int n = atoi(s.c_str() + p + 14);
            if (n > 0 && n < 100)
                return (float)n;
        }
        size_t q = s.find("u-col-");
        if (q != std::string::npos) {
            std::string tail = s.substr(q + 6);
            std::string num;
            for (char c : tail) { if (std::isdigit((unsigned char)c) || c=='p' || c=='.') num+=c; else break; }
            for (char &c : num) if (c=='p') c='.';
            char *e=nullptr; float v=strtof(num.c_str(), &e);
            if (e!=num.c_str() && css_finite(v) && v > 0.f) return v;
        }
    }
    return 0.0f;
}

/* PennyMac-style header nav: <ul><li style="float:left; width:N%">.
 * Two or more floated or %-width items belong on one row, not as a
 * vertical bullet list.  Also suppress bullets for nav rows so tiny
 * header links don't get stray bullets. */
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
        if (box.float_left || (box.width_pct > 0.0f && box.width_pct < 99.0f))
            ++side;
        ++n;
    }
    if (n >= 2 && side >= 2) return true;
    return n >= 2 && side >= 1 && n <= 6;
}

// Unused helper kept for reference; tight spacing now handled in Document::draw
// (image→short text / empty paragraph).  HtmlDocument root gap is 0 — inter-row
// spacing comes from per-row v-container-padding paddings only.

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
        char *e = nullptr;
        float v = strtof(tag.c_str() + p, &e);
        if (e == tag.c_str() + p || !css_finite(v) || v <= 0.f || v > 5000.f) return 0.f;
        return v;
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
                     float hint_w = 0.0f, float hint_h = 0.0f,
                     bool *display_block = nullptr) {
    BoxProps box;
    Style dummy;
    TextAlignment a = TextAlignment::Left;
    apply_cascade(el, dummy, a, B, &box);
    if (display_block)
        *display_block = box.display_block;

    const char *wattr = attr(el, "width");
    const char *hattr = attr(el, "height");
    auto safe_parse = [](const char *s)->float{
        if(!s||!*s) return 0.f;
        char *e=nullptr; float v=strtof(s,&e);
        if(e==s || !css_finite(v) || v<0.f || v>5000.f) return 0.f;
        return v;
    };
    float aw = safe_parse(wattr);
    float ah = safe_parse(hattr);
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

/* Chromium maps a NON-table element's align="center"/"right" attribute to
 * text-align:-webkit-center / -webkit-right, which — unlike plain CSS
 * text-align — also aligns BLOCK-level children that don't span the full
 * width (the max-width wrapper div around a centered logo or emoji strip,
 * the 188px security stamp table inside a 367px align="right" cell).
 * align= on a <table> means margin:auto instead (handled in the TABLE
 * case), and any CSS text-align on an element overrides the inherited
 * -webkit- value, so only the attribute on non-table ancestors counts.
 * Walk up from `node` and let the nearest such signal decide. */
enum class WebkitAlign { None, Center, Right };
WebkitAlign ancestor_webkit_align(GumboNode *node) {
    for (GumboNode *p = node->parent; p; p = p->parent) {
        if (p->type != GUMBO_NODE_ELEMENT)
            continue;
        GumboElement *pe = &p->v.element;
        GumboTag t = pe->tag;
        if (t == GUMBO_TAG_TABLE || t == GUMBO_TAG_THEAD ||
            t == GUMBO_TAG_TBODY || t == GUMBO_TAG_TFOOT)
            continue;   /* table align= is margin:auto, not inherited */
        const char *style = attr(pe, "style");
        if (style && std::string(style).find("text-align") != std::string::npos)
            return WebkitAlign::None;   /* plain CSS text-align beats the attribute */
        if (const char *a = attr(pe, "align")) {
            std::string v = trim_lower(a);
            if (v == "center")
                return WebkitAlign::Center;
            if (v == "right")
                return WebkitAlign::Right;
            return WebkitAlign::None;
        }
    }
    return WebkitAlign::None;
}

void build_children(Widget *container, GumboVector *kids, Style st,
                    Builder &B, int list_depth, TextAlignment align, int depth=0);
void walk_inline(GumboNode *node, Style st, Flow &F, Builder &B,
                 int list_depth, int depth=0);

void walk_inline_children(GumboVector *kids, Style st, Flow &F, Builder &B,
                          int list_depth, int depth) {
    if (depth > 80) return;
    for (unsigned i = 0; i < kids->length; ++i)
        walk_inline((GumboNode *)kids->data[i], st, F, B, list_depth, depth+1);
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
                 int list_depth, int depth) {
    if (depth > 80) return;
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_CDATA) {
        F.emit(node->v.text.text, st);
        return;
    }
    if (node->type == GUMBO_NODE_WHITESPACE) {
        F.emit(" ", st);
        return;
    }
    if (node->type == GUMBO_NODE_DOCUMENT) {
        walk_inline_children(&node->v.document.children, st, F, B, list_depth, depth+1);
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
        F.lineBreak(st);
        return;
    case GUMBO_TAG_HR:
        F.brk();
        F.doc.addParagraph()->isRule = true;
        F.brk();
        return;
    case GUMBO_TAG_IMG: {
        const char *src = attr(el, "src");
        const char *alt = attr(el, "alt");
        /* Strip surrounding whitespace/newlines like a real URL parser:
         * marketing mail generators emit <img src="\n\nhttps://...\n\n">
         * and the untrimmed string silently failed every scheme check
         * below, so the image never fetched or painted (backmarket hero
         * banner). */
        std::string s = src ? trim(src) : std::string();
        if (!s.empty()) {
            /* width="0" height="0" read-receipt pixel: skip it entirely —
             * the unknown-size fallback in size_html_image() would
             * otherwise draw a stray 160x100 placeholder box. */
            const char *w0 = attr(el, "width");
            const char *h0 = attr(el, "height");
            if (w0 && h0 && trim(w0) == "0" && trim(h0) == "0")
                return;
            if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0)
                B.view->note_remote_image();
            HtmlImageInfo ri;
            if (B.view->image_resolver)
                ri = B.view->image_resolver(src);

            float hint_w = 0.0f, hint_h = 0.0f;
            mso_size_hint(node, hint_w, hint_h);
            float pw = 0.0f, ph = 0.0f;
            bool display_block = false;
            size_html_image(el, B, ri, pw, ph, hint_w, hint_h, &display_block);

            constexpr float kInlineIconMaxPx = 32.0f;
            if (pw > 0.0f && ph > 0.0f &&
                pw <= kInlineIconMaxPx && ph <= kInlineIconMaxPx) {
                Text t;
                t.isImageRun = true;
                t.image = ri.id; t.image_w = pw; t.image_h = ph; t.image_src = s;
                /* display:block has no line box, so no gap under the
                 * image: pin the line to exactly the image height (the
                 * USAA stamp's 8x29 / 14x29 lock slices). */
                if (display_block)
                    t.style.lineHeight = ph;
                if (!st.linkUrl.empty()) t.linkUrl = st.linkUrl;
                F.para()->addText(t); F.cur_has_text = true;
                return;
            }

            F.brk();
            Paragraph *ip = F.doc.addParagraph();
            ip->isImage   = true;
            ip->alignment = F.align;
            ip->image     = ri.id;
            ip->image_w   = pw;
            ip->image_h   = ph;
            ip->image_src = std::move(s);
            if (!st.linkUrl.empty()) ip->linkUrl = st.linkUrl;
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

    int saved_header = F.pending_header;
    switch (tag) {
    case GUMBO_TAG_B: case GUMBO_TAG_STRONG:
        st.bold = true; break;
    case GUMBO_TAG_I: case GUMBO_TAG_EM: case GUMBO_TAG_CITE: case GUMBO_TAG_DFN:
        st.italic = true; break;
    case GUMBO_TAG_U: case GUMBO_TAG_INS:
        st.underline = true; break;
    case GUMBO_TAG_S: case GUMBO_TAG_STRIKE: case GUMBO_TAG_DEL:
        st.strike = true; break;
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
        walk_inline_children(&el->children, st, F, B, list_depth, depth+1);
        F.pre = pre_save;
        F.brk();
        return;
    }
    case GUMBO_TAG_A: {
        st.fgColor = B.accent;
        st.underline = true;
        const char *href = attr(el, "href");
        if (href && href[0]) st.linkUrl = href;
        break;
    }
    case GUMBO_TAG_H1: st.bold = true; st.fontSize *= 1.6f; F.pending_header = 1; break;
    case GUMBO_TAG_H2: st.bold = true; st.fontSize *= 1.4f; F.pending_header = 2; break;
    case GUMBO_TAG_H3: st.bold = true; st.fontSize *= 1.2f; F.pending_header = 3; break;
    case GUMBO_TAG_H4: st.bold = true; st.fontSize *= 1.1f; F.pending_header = 4; break;
    case GUMBO_TAG_H5: st.bold = true; st.fontSize *= 1.1f; F.pending_header = 5; break;
    case GUMBO_TAG_H6: st.bold = true; st.fontSize *= 1.1f; F.pending_header = 6; break;
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
            // else: apply_cascade already handled vertical-align:middle → st.verticalMiddle
        }
    }

    // P0-4: inline <a>/<span> that paints a pill (bg + pad/border/radius) must
    // become a block box to get FlexLayout Center/Center.  Otherwise
    // background is per-word and sits on baseline.
    bool inline_pill = (tag == GUMBO_TAG_A || tag == GUMBO_TAG_SPAN || tag == GUMBO_TAG_FONT || tag == GUMBO_TAG_LABEL)
        && st.bgColor.a > 0.02f && (st.padX > 0.5f || st.padY > 0.5f || st.borderWidth > 0.5f);
    // Also promote if width/height or border-radius was set via CSS box
    if (!inline_pill && (tag == GUMBO_TAG_A || tag == GUMBO_TAG_SPAN)) {
        BoxProps bp; Style d2; TextAlignment aa = TextAlignment::Left;
        apply_cascade(el, d2, aa, B, &bp, node);
        if (d2.bgColor.a > 0.02f && (bp.radius_px > 0.5f || bp.pad_x > 0.5f || bp.pad_y > 0.5f))
            inline_pill = true;
    }
    if (inline_pill && !block) {
        // Flush current flow into its own HtmlText, then create a centered pill block
        // NOTE: walk_inline has no container Widget — the pill must live inside the
        // current Flow's HtmlText would not get centering.  Instead we force a
        // break so the pill's box is handled by build_children's default:
        // when this node returns, its children will have been emitted as text.
        // To get block centering we mark the paragraph's alignment and use a
        // pill Style that produces a single-run pill.  For now, keep it inline but
        // ensure verticalMiddle is set so Document centers it in lineHeight.
        st.verticalMiddle = true;
        if (st.lineHeight == 0.f) {
            float lh = st.fontSize + st.padY * 2.f + st.borderWidth * 2.f;
            st.lineHeight = std::max(lh, st.fontSize * 1.0f + 8.f);
        }
    }

    walk_inline_children(&el->children, st, F, B, list_depth, depth+1);

    F.pending_header = saved_header;
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

/* True if `td` contains nothing but a single nested <table> — the
 * "double table" idiom some email templates use to re-center content
 * for Outlook (<table width=N style="margin:auto"><tr><td><table>...
 * real content...</table></td></tr></table>).  Such a cell has no
 * layout identity of its own; it's a transparent re-wrap of its one
 * child table. */
bool cell_is_table_passthrough(GumboElement *td) {
    GumboNode *only = nullptr;
    for (unsigned i = 0; i < td->children.length; ++i) {
        GumboNode *cn = (GumboNode *)td->children.data[i];
        if (cn->type == GUMBO_NODE_WHITESPACE || cn->type == GUMBO_NODE_COMMENT)
            continue;
        if (cn->type == GUMBO_NODE_TEXT) {
            for (const char *p = cn->v.text.text; *p; ++p)
                if (!std::isspace((unsigned char)*p)) return false;
            continue;
        }
        if (cn->type != GUMBO_NODE_ELEMENT || only)
            return false;
        only = cn;
    }
    return only && only->v.element.tag == GUMBO_TAG_TABLE;
}

/* True if any <tr> belonging to THIS table (not a nested <table>, which
 * has its own independent layout context — e.g. the pill's tiny inner
 * icon+"1" table) has more than one real cell: a column grid (a photo
 * row, a caption's own layout table) whose cells are sized by
 * proportional flex-grow at *layout* time, not by summing children
 * bottom-up.  Their reported preferred size is near zero (grow-based
 * cells carry flex-basis 0), so shrink-wrapping the table via
 * AlignItems::Center — fine for a single-column table like an avatar or
 * a notification "pill" — collapses a grid to a sliver.
 *
 * A cell that's nothing but a re-wrapping <table> (see
 * cell_is_table_passthrough) isn't a real boundary — keep looking inside
 * it for the grid it forwards to, instead of stopping and reporting "no
 * grid here" for what is really just an Outlook centering wrapper. */
bool table_has_multicell_row(GumboNode *node, bool is_root = true, int depth = 0) {
    if (node->type != GUMBO_NODE_ELEMENT || depth > 200)
        return false;
    GumboElement *el = &node->v.element;
    if (!is_root && el->tag == GUMBO_TAG_TABLE)
        return false;
    if (el->tag == GUMBO_TAG_TR && count_row_cells(el) > 1)
        return true;
    if ((el->tag == GUMBO_TAG_TD || el->tag == GUMBO_TAG_TH) &&
        cell_is_table_passthrough(el)) {
        for (unsigned i = 0; i < el->children.length; ++i) {
            GumboNode *cn = (GumboNode *)el->children.data[i];
            if (cn->type == GUMBO_NODE_ELEMENT && cn->v.element.tag == GUMBO_TAG_TABLE)
                return table_has_multicell_row(cn, true, depth + 1);
        }
        return false;
    }
    for (unsigned i = 0; i < el->children.length; ++i)
        if (table_has_multicell_row((GumboNode *)el->children.data[i], false, depth + 1))
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

/* <td style="line-height:0">&nbsp;</td>: an Outlook spacer whose only
 * line box is 0px tall, so the cell is exactly its height= (the 7px
 * bottom row of the USAA stamp).  Built as a text leaf, the &nbsp; made
 * it a full line of body text. */
bool cell_is_zero_line_spacer(GumboElement *ce, const Builder &B) {
    for (unsigned i = 0; i < ce->children.length; ++i) {
        GumboNode *cn = (GumboNode *)ce->children.data[i];
        if (cn->type == GUMBO_NODE_ELEMENT)
            return false;
        if (cn->type != GUMBO_NODE_TEXT)
            continue;
        for (const unsigned char *p = (const unsigned char *)cn->v.text.text; *p; ++p) {
            if (p[0] == 0xC2 && p[1] == 0xA0) { ++p; continue; }  // &nbsp;
            if (!std::isspace(*p))
                return false;
        }
    }
    BoxProps box;
    element_box(ce, B, box);
    return box.zero_line_height;
}

int element_height_px(GumboElement *el, const BoxProps &box) {
    if (box.height_px > 1.0f)
        return (int)box.height_px;
    const char *h = attr(el, "height");
    if (h && h[0]) {
        char *e=nullptr; float fv=strtof(h, &e);
        if (e!=h && css_finite(fv) && fv > 1.f) return (int)fv;
    }
    return 0;
}

/* Pixel width of a table cell.  -1 = auto/%;  0 = collapse (width="0"). */
int cell_px_width(GumboElement *el, const Builder &B) {
    const char *w = attr(el, "width");
    if (w && w[0]) {
        std::string v = trim_lower(w);
        if (!v.empty() && v.back() != '%') {
            char *e=nullptr; float fv=strtof(v.c_str(), &e);
            if (e!=v.c_str() && css_finite(fv) && fv >= 0.f && fv < 2000.f) return (int)fv;
        }
        return -1;
    }
    BoxProps box;
    element_box(el, B, box);
    if (box.width_px > 0.0f && box.width_px < 2000.0f && box.width_pct <= 0.0f)
        return (int)box.width_px;
    return -1;
}

TextAlignment element_align(GumboElement *el, TextAlignment inherited,
                            bool &changed, const Builder &B);

/* Pixel width of a cell's actual content, for a <td> that has no width
 * of its own but wraps a single explicitly-sized element (a common
 * photo-grid idiom: <td><a><div style="width:130px;...">tile</div></a>
 * </td>).  Real browsers auto-size table columns to content, so such a
 * cell sits snugly against its siblings instead of stretching to an
 * equal share of the row — drill through single-element "passthrough"
 * wrappers (<a>, <center>, ...) to find the sized element.  -1 if the
 * cell holds more than one thing or nothing definite.
 *
 * A cell whose OWN alignment is center/right (align="right", a common
 * "logo left, avatar right" header idiom) wants to stay a growing cell
 * even though its content is small — align is exactly the signal that
 * there's slack space to push the content into. Shrinking it to content
 * width here packed the avatar right next to the logo instead of at the
 * far edge. */
int cell_content_px_width(GumboElement *td, const Builder &B) {
    bool changed = false;
    TextAlignment ta = element_align(td, TextAlignment::Left, changed, B);
    if (ta == TextAlignment::Center || ta == TextAlignment::Right)
        return -1;
    GumboElement *el = td;
    for (int guard = 0; guard < 8; ++guard) {
        GumboNode *only = nullptr;
        for (unsigned i = 0; i < el->children.length; ++i) {
            GumboNode *cn = (GumboNode *)el->children.data[i];
            if (cn->type == GUMBO_NODE_WHITESPACE || cn->type == GUMBO_NODE_COMMENT)
                continue;
            if (cn->type == GUMBO_NODE_TEXT) {
                bool blank = true;
                for (const char *p = cn->v.text.text; *p; ++p)
                    if (!std::isspace((unsigned char)*p)) { blank = false; break; }
                if (!blank) return -1;
                continue;
            }
            if (cn->type != GUMBO_NODE_ELEMENT || only)
                return -1;
            only = cn;
        }
        if (!only) return -1;
        el = &only->v.element;
        int px = cell_px_width(el, B);
        if (px > 0)
            return px;
    }
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
            char *e=nullptr; float pct=strtof(v.c_str(), &e);
            if (e!=v.c_str() && css_finite(pct) && pct > 0.0f && pct < 99.0f)
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
    int th_count = 0, td_count = 0;
    int with_width = 0;
    int with_block_display = 0;
    for (unsigned i = 0; i < tr->children.length; ++i) {
        GumboNode *cn = (GumboNode *)tr->children.data[i];
        if (cn->type != GUMBO_NODE_ELEMENT) continue;
        GumboElement *ce = &cn->v.element;
        if (ce->tag != GUMBO_TAG_TD && ce->tag != GUMBO_TAG_TH)
            continue;
        if (ce->tag == GUMBO_TAG_TH) th_count++; else td_count++;
        if (cell_px_width(ce, B) >= 0 || cell_width_pct(ce, B) > 0.0f)
            with_width++;
        const char *st = attr(ce, "style");
        if (st) {
            std::string l = trim_lower(st);
            if (l.find("display:block") != std::string::npos ||
                l.find("display: block") != std::string::npos ||
                l.find("inline-block") != std::string::npos)
                with_block_display++;
        }
        if (cell_is_empty(ce)) {
            if (cell_px_width(ce, B) > 0)
                wide_empty++;
        } else {
            content++;
        }
    }
    if (content <= 1 && wide_empty == 0)
        return true;
    // MyHeritage-style card: single <tr> with 3 <th display:block/inline-block>
    // and no widths.  Intended as a vertical stack (image on top, title,
    // blurb) not 3 side-by-side columns.  As a Row the 480px image is
    // flex-shrunk to ~20px and then Document scales the bitmap to that
    // cell width, so on every relayout/scroll the image appears to
    // "resize".  Collapse to Column so each <th> is a full-width row.
    // Guard: only TH-only rows with no explicit widths and at least one
    // block/inline-block display — leaves real 50/50 TD columns alone.
    if (th_count >= 2 && td_count == 0 && with_width == 0 && with_block_display > 0)
        return true;
    // Also catch TH-only rows with no widths even without explicit display
    // (some templates rely on default block stacking for TH).
    if (th_count >= 3 && td_count == 0 && with_width == 0 && content == th_count)
        return true;
    return false;
}

void decorate_block(HtmlBlock *b, GumboElement *el, const Builder &B) {
    BoxProps box;
    element_box(el, B, box);
    // overflow:hidden — no explicit Widget flag; HtmlBlock draw already clips via nvgIntersectScissor per block.
    float bw = 0; // border width for border-box adjustment
    // Capture borderWidth if present on the box via cascaded Style — we need it for border-box
    // Since element_box already applied borderWidth to Style, peek it here:
    { Style tmp; TextAlignment a=TextAlignment::Left; apply_cascade(el, tmp, a, B); bw = tmp.borderWidth; }
    float padX = box.pad_x, padY = box.pad_y;
    auto apply_len = [&](float v, bool isMin){
        (void)isMin;
        if (box.border_box && (padX>0 || padY>0 || bw>0)) {
            // For fixed width/height, content box = outer - 2*pad - border
            // Only adjust the fixed outer we set via min==max; otherwise flex handles content.
        }
        return v;
    };
    if (box.max_width_px > 80.0f && box.max_width_px < 4000.0f)
        b->set_max_width((int)box.max_width_px);
    if (box.min_width_px > 0.5f)
        b->set_min_width((int)std::lround(box.min_width_px));
    if (box.width_px > 0.0f && box.width_px < 4000.0f &&
        box.width_pct <= 0.0f) {
        float outer = box.width_px;
        if (box.border_box) outer = std::max(box.min_width_px, outer - 2.f*padX - 2.f*bw);
        else outer += 2.f*padX + 2.f*bw;
        (void)apply_len;
        int w = (int)std::lround(std::max(outer, 1.f));
        b->set_min_width(std::max(b->min_size().x(), w));
        if (box.min_width_px <= 0.5f) b->set_max_width(w);
        else b->set_max_width(std::max(b->max_size().x(), w));
    } else if (box.min_width_px > 0.5f && b->max_size().x() <= 0) {
        // min-width only: ensure at least that
        b->set_min_width((int)std::lround(box.min_width_px));
    }
    if (box.min_height_px > 0.5f)
        b->set_min_height((int)std::lround(box.min_height_px));
    if (box.radius_px > 0.0f)
        b->m_radius = box.radius_px;
    int h = element_height_px(el, box);
    if (h > 1) {
        if (box.border_box) h = std::max((int)box.min_height_px, h - (int)std::lround(2.f*padY) - (int)std::lround(2.f*bw));
        else h += (int)std::lround(2.f*padY) + (int)std::lround(2.f*bw);
        b->set_min_height(std::max(b->min_size().y(), h));
    }
    if (auto *fl = dynamic_cast<FlexLayout *>(b->layout())) {
        /* reset_box_style() strips padding from descendant runs; this is
         * where CSS padding actually insets the widget's children. */
        if (box.pad_x > 0.5f || box.pad_y > 0.5f)
            fl->set_padding((int)std::lround(box.pad_x),
                            (int)std::lround(box.pad_y));
        /* Instagram's notification chip is `display:inline-flex;
         * vertical-align:middle; text-align:center` — a painted pill
         * whose heart+"1" must sit in the geometric middle, not at
         * flex-start (top-left) of the padding box. Only true
         * inline-flex carries that centering intent — inline-block
         * (checked via the broader inline_flex flag elsewhere for its
         * shared shrink-to-content sizing) is a layout-flow convention
         * MJML columns use for side-by-side placement, not a request to
         * center their children (that centered a schwab.com email's
         * whole header row instead of stretching it). */
        if (box.is_inline_flex || (box.align_middle && b->m_bg.a > 0.0f)) {
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

/* cellpadding= of the table that owns row `tr` (0 if none). */
int table_cellpadding(GumboNode *tr) {
    for (GumboNode *p = tr->parent; p && p->type == GUMBO_NODE_ELEMENT;
         p = p->parent) {
        if (p->v.element.tag != GUMBO_TAG_TABLE)
            continue;
        const char *cp = attr(&p->v.element, "cellpadding");
        int n = cp ? atoi(cp) : 0;
        return (n > 0 && n <= 32) ? n : 0;
    }
    return 0;
}

/* HTML table-cell behavior the CSS box doesn't cover: the table's
 * cellpadding= insets a cell with no CSS padding of its own, and the
 * content sits at the cell's vertical-align — CSS, else valign= on the
 * cell or its row, else the HTML default of middle.  The USAA security
 * stamp's text column needs both to sit centered and clear of the lock. */
void decorate_cell(HtmlBlock *cell, GumboElement *td, GumboNode *tr,
                   const Builder &B, bool place_vertically = true) {
    auto *fl = dynamic_cast<FlexLayout *>(cell->layout());
    if (!fl)
        return;
    BoxProps box;
    element_box(td, B, box);
    if (box.pad_x <= 0.5f && box.pad_y <= 0.5f) {
        int n = table_cellpadding(tr);
        if (n > 0)
            fl->set_padding(n, n);
    }
    if (!place_vertically)
        return;
    char va = box.cell_valign;
    if (!va) {
        const char *a = attr(td, "valign");
        if (!a)
            a = attr(&tr->v.element, "valign");
        std::string v = a ? trim_lower(a) : std::string("middle");
        va = (v == "top" || v == "baseline") ? 't' : (v == "bottom") ? 'b' : 'm';
    }
    fl->set_justify_content(va == 'm' ? JustifyContent::Center
                            : va == 'b' ? JustifyContent::FlexEnd
                                        : JustifyContent::FlexStart);
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
                    Builder &B, int list_depth, TextAlignment align, int depth) {
    if (depth > 80) return;
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
            walk_inline(node, st, F, B, list_depth, depth+1);
            continue;
        }
        GumboElement *el = &node->v.element;
        GumboTag tag = el->tag;

        /* Host widget slot: flush the text flow and ask the factory to
         * parent a live child here. Unknown ids (and sender-injected tags
         * the host did not register) are skipped. */
        std::string embed_id;
        if (host_embed_id(el, embed_id)) {
            flush();
            try_host_embed(container, el, embed_id, B);
            continue;
        }

        bool block_anchor = tag == GUMBO_TAG_A && element_has_block_child(el);

        if ((!is_container_tag(tag) || is_skipped_tag(tag)) && !block_anchor) {
            walk_inline(node, st, F, B, list_depth, depth+1);
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
                    HtmlBlock *row = make_block(container, FlexDirection::Row, 0);
                    // Vertically center columns when they have unequal height (e.g. madeleine banner logo 82 vs buttons 61).
                    if (auto *rfl = dynamic_cast<FlexLayout*>(row->layout())) rfl->set_align_items(AlignItems::Center);
                    for (GumboElement *ce : cols) {
                        HtmlBlock *cell = make_block(row, FlexDirection::Column, 0);
                        cell->m_bg = block_background(ce, B);
                        float grow = flex_column_pct(ce, B);
                        if (grow <= 0.0f) grow = 1.0f;
                        build_children(cell, &ce->children, st, B, list_depth, align, depth+1);
                        if (auto *fl = dynamic_cast<FlexLayout *>(row->layout()))
                            fl->set_flex_item(cell,
                                              FlexLayout::FlexItem(grow, 1.0f, 0));
                    }
                    i = k - 1;
                    continue;
                }
            }
        }

        // Block-anchor <a> wrapping a div/table: propagate href to the wrapper block
        // so the whole block is clickable (and shows Hand), not just inline text.
        std::string anchor_href;
        if (block_anchor) {
            const char *href = attr(el, "href");
            if (href && href[0]) anchor_href = href;
        }
        // Remember container's child count to tag the newly created block
        size_t anchor_before = container->children().size();

        switch (tag) {
        case GUMBO_TAG_TABLE:
        case GUMBO_TAG_TBODY:
        case GUMBO_TAG_THEAD:
        case GUMBO_TAG_TFOOT: {
            bool save_fw = B.in_full_width;
            std::vector<float> save_cols;
            int save_row_min_w = B.table_row_min_w;
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
                /* Legacy `align="center"` on <table> is the pre-CSS
                 * equivalent of `margin:auto` — real browsers shrink-wrap
                 * and center such a table exactly like margin:auto does. */
                const char *align_attr = attr(el, "align");
                if (align_attr && trim_lower(align_attr) == "center")
                    box.center = true;
                bool cap = box.max_width_px > 80.0f && box.max_width_px < 4000.0f;
                /* Email convention: max-width:600px (even with width:100%)
                 * is a centered column, not a shrink-wrapped align=center
                 * cell — that path collapsed Amazon's 33/67 rows.
                 * `margin:auto` plus an explicit non-% width= is the
                 * other common centering idiom (a lone avatar, a
                 * notification "pill") — shrink-wrap and center it too,
                 * unless it's actually a column grid in disguise.
                 * A table with NO width signal anywhere (attribute or
                 * CSS) is the same idiom by default: real "auto" table
                 * layout shrinks to content unless told to fill. */
                bool has_width_attr = attr(el, "width") && attr(el, "width")[0];
                bool has_any_width_signal = has_width_attr ||
                                            box.width_px > 0.0f ||
                                            box.width_pct > 0.0f;
                bool center_shrink = !cap && box.center &&
                                     (has_explicit_shrink_width(el) ||
                                      !has_any_width_signal) &&
                                     !table_has_multicell_row(node);
                /* align="center"/"right" on a non-table ancestor
                 * (-webkit-center/-webkit-right) also block-aligns a
                 * shrink-wrapped table as a whole: the classic icon strip
                 * (<td align="center"><table width="173px">…social
                 * icons…) and the USAA security stamp (<td align="right"
                 * width="367"><table style="width:188px">…).  Rows align
                 * individually (Center/FlexEnd) since the table builds no
                 * single widget of its own; a CSS width:Npx pins them to
                 * the exact width via B.table_row_min_w. */
                WebkitAlign wc_table = WebkitAlign::None;
                if (!cap && !center_shrink && !box.center &&
                    (has_explicit_shrink_width(el) ||
                     (box.width_px > 0.0f && box.width_pct <= 0.0f)))
                    wc_table = ancestor_webkit_align(node);
                if (cap || center_shrink || wc_table != WebkitAlign::None) {
                    /* `outer` centers the TABLE AS A WHOLE within its own
                     * parent (the shrink-wrap/margin:auto semantic) — its
                     * own ROWS must stay flush against each other
                     * (AlignItems::Stretch), not individually re-center
                     * relative to `outer`.  Center here collapsed a
                     * multi-row table (e.g. a paragraph row next to a
                     * narrower single-cell button row) into staggered
                     * left edges instead of one shared left margin. */
                    HtmlBlock *outer = make_block(container, FlexDirection::Column,
                                                  0, wc_table == WebkitAlign::Center
                                                         ? AlignItems::Center
                                                     : wc_table == WebkitAlign::Right
                                                         ? AlignItems::FlexEnd
                                                         : AlignItems::Stretch);
                    target = outer;
                    /* The width= attribute form (the 173px social strip)
                     * stays content-sized — its measured content
                     * legitimately exceeds the nominal attribute. */
                    if (wc_table != WebkitAlign::None && box.width_px > 80.0f &&
                        box.width_px < 4000.0f)
                        B.table_row_min_w = (int)std::lround(box.width_px);
                    else
                        B.table_row_min_w = 0;
                }
                if (bg.a > 0.0f || cap || box.radius_px > 0.0f) {
                    HtmlBlock *inner = make_block(target, FlexDirection::Column, 0);
                    inner->m_bg = bg;
                    decorate_block(inner, el, B);
                    target = inner;
                }
            }
            build_children(target, &el->children, st, B, list_depth, align, depth+1);
            B.in_full_width = save_fw;
            B.table_row_min_w = save_row_min_w;
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
                    /* Stacked cells have no row height to place within,
                     * but cellpadding still insets them (the USAA stamp's
                     * one-cell text table). */
                    const bool cpad = table_cellpadding(node) > 0;
                    if (cell == target && (h > 1 || box.radius_px > 0.0f ||
                                           box.max_width_px > 80.0f || cpad)) {
                        HtmlBlock *sp = make_block(target, FlexDirection::Column, 0);
                        sp->m_bg = block_background(ce, B);
                        decorate_block(sp, ce, B);
                        cell = sp;
                    }
                    if (cell != target && cpad)
                        if (auto *hb = dynamic_cast<HtmlBlock *>(cell))
                            decorate_cell(hb, ce, node, B, false);
                    Style cst = st;
                    TextAlignment cascade_align = ta;
                    apply_cascade(ce, cst, cascade_align, B);
                    if (paint)
                        ta = cascade_align;
                    cst.superscript = st.superscript;
                    reset_box_style(cst);
                    if (ce->tag == GUMBO_TAG_TH)
                        cst.bold = true;
                    build_children(cell, &ce->children, cst, B, list_depth, ta, depth+1);
                }
                break;
            }
            /* cellspacing / border-collapse: honor explicit gaps, default 0 for email */
            int row_gap = 0;
            {
                const char *cs = attr(el, "cellspacing");
                if (cs && *cs) {
                    int v = atoi(cs);
                    if (v >= 0 && v <= 32) row_gap = v;
                } else {
                    // Check style attrs for border-collapse:separate or border-spacing
                    const char *st = attr(el, "style");
                    if (st && std::string(st).find("border-spacing") != std::string::npos) {
                        // crude: take first px number
                        const char *p = std::strstr(st, "border-spacing");
                        if (p) { char *e=nullptr; float n=strtof(p+14,&e); if(e!=p+14 && n>=0 && n<=32) row_gap=(int)n; }
                    }
                    const char *bc = attr(el, "border");
                    (void)bc;
                }
            }
            HtmlBlock *row = make_block(container, FlexDirection::Row, row_gap);
            row->m_bg = block_background(el, B);
            /* CSS width:Npx on a block-aligned table pins every row to the
             * exact width (the 188px USAA stamp inside a 367px cell). */
            if (B.table_row_min_w > 0)
                row->set_min_width(B.table_row_min_w);
            float explicit_sum = 0.0f;
            int auto_cells = 0;
            for (unsigned j = 0; j < el->children.length; ++j) {
                GumboNode *cn = (GumboNode *)el->children.data[j];
                if (cn->type != GUMBO_NODE_ELEMENT) continue;
                GumboTag ct = cn->v.element.tag;
                if (ct != GUMBO_TAG_TD && ct != GUMBO_TAG_TH) continue;
                if (cell_px_width(&cn->v.element, B) >= 0)
                    continue;
                if (cell_content_px_width(&cn->v.element, B) > 0)
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
                HtmlBlock *cell = make_block(row, FlexDirection::Column, 0);
                cell->m_bg = block_background(ce, B);
                decorate_block(cell, ce, B);
                decorate_cell(cell, ce, node, B);
                bool ch = false;
                TextAlignment ta = element_align(ce, align, ch, B);
                Style cst = st;
                apply_cascade(ce, cst, ta, B);
                cst.superscript = st.superscript;
                reset_box_style(cst);
                if (ce->tag == GUMBO_TAG_TH)
                    cst.bold = true;
                if (!cell_is_zero_line_spacer(ce, B))
                    build_children(cell, &ce->children, cst, B, list_depth, ta, depth+1);
                int px = cell_px_width(ce, B);
                bool content_sized = false;
                if (px < 0) {
                    px = cell_content_px_width(ce, B);
                    content_sized = px > 0;
                }
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
                    /* A content-drilled width measures the CONTENT box
                     * only — the cell's own horizontal padding sits
                     * outside it.  The social-icon cells (padding:0 0 0
                     * 15px around a 32px <img>) otherwise pinned the cell
                     * to 32px and squeezed the icon into the 2px
                     * remainder. */
                    if (content_sized) {
                        BoxProps cbox;
                        element_box(ce, B, cbox);
                        px += (int)std::lround(2.0f * cbox.pad_x);
                    }
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
                build_children(cell, &el->children, cst, B, list_depth, align, depth+1);
            } else {
                HtmlBlock *cell = make_block(container, FlexDirection::Column, 0);
                cell->m_bg = block_background(el, B);
                build_children(cell, &el->children, cst, B, list_depth, align, depth+1);
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
                    build_children(cell, &ce->children, cst, B, 0, ta, depth+1);
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
                walk_inline(node, st, listF, B, list_depth, depth+1);
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
            build_children(target, &el->children, st, B, list_depth, TextAlignment::Center, depth+1);
            break;
        }
        case GUMBO_TAG_CAPTION: {
            Widget *target = wrap_if_bg(container, el, 4, B);
            Style cst = st;
            cst.italic = true;
            cst.fgColor = B.meta;
            build_children(target, &el->children, cst, B, list_depth, TextAlignment::Center, depth+1);
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
            build_children(container, &el->children, st, B, list_depth, align, depth+1);
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
            /* An explicit pixel width (CSS `width:Npx`, e.g. an anchor
             * sized to a fixed button width) is a hard constraint too,
             * not just max-width — a block-anchor <a style="width:370px">
             * wrapping a wider inner table must still cap to 370, not
             * silently pass through and stretch to fill its container. */
            bool has_width_px = box.width_px > 0.0f && box.width_pct <= 0.0f;
            if (bg.a <= 0.0f && !align_changed && cap <= 0 &&
                box.radius_px <= 0.0f && !box.center &&
                !box.inline_flex && box.pad_x <= 0.5f && box.pad_y <= 0.5f &&
                !has_width_px && box.bg_image_url.empty()) {
                Style cst = st;
                TextAlignment ta = child_align;
                apply_cascade(el, cst, ta, B);
                cst.superscript = st.superscript;
                reset_box_style(cst);
                build_children(container, &el->children, cst, B, list_depth, ta, depth+1);
            } else {
                Widget *host = container;
                /* Shrink-wrap inline-flex chips (the notification pill)
                 * and max-width columns so they don't stretch full-width
                 * with the icon sitting at the top-left of a red bar.
                 * max-width alone is a ceiling (PennyMac .header-menu is
                 * 610px inside a 600px column) — only margin:auto is a
                 * "center me as a page column" hint.
                 * inline-flex/inline-block normally means "shrink to
                 * content", but an explicit width:100% contradicts that
                 * (an MJML column uses display:inline-block purely to
                 * sit side-by-side, not to shrink) — width wins. */
                bool cap_triggered = cap > 0 && box.center;
                bool shrink = (box.inline_flex && box.width_pct < 99.0f) ||
                              (box.center && box.width_px > 0.0f &&
                               box.width_pct < 99.0f);
                /* align="center"/"right" on a non-table ancestor
                 * (-webkit-center/-webkit-right): a width-capped block
                 * like the max-width:114px emoji wrapper aligns as a
                 * whole instead of hugging the left edge of its
                 * stretched cell. */
                WebkitAlign wc_align = WebkitAlign::None;
                if (!cap_triggered && !shrink && (cap > 0 || has_width_px))
                    wc_align = ancestor_webkit_align(node);
                bool parent_centers = false;
                if (auto *pfl = dynamic_cast<FlexLayout *>(container->layout()))
                    parent_centers = pfl->align_items() == AlignItems::Center;
                if ((cap_triggered || shrink || wc_align != WebkitAlign::None) &&
                    !parent_centers) {
                    /* Two different reasons land here, and they want
                     * opposite child alignment. `cap_triggered` (a
                     * max-width/no-width centered column, e.g. a
                     * max-width:768px page wrapper) wraps FLOWING
                     * content — its own children (a <table>'s rows) must
                     * stretch flush against each other, not individually
                     * re-center (this collapsed a schwab.com email's
                     * header row into a narrow 3-line column). `shrink`
                     * (a true inline-flex chip, or margin:auto plus an
                     * explicit small pixel width) wraps a SINGLE small
                     * thing that must stay centered at its own size,
                     * not stretch to fill whatever width an ancestor
                     * hands `outer` (this turned the Instagram
                     * notification pill into a full-width red bar). */
                    HtmlBlock *outer = make_block(container, FlexDirection::Column,
                                                  0, cap_triggered
                                                         ? AlignItems::Stretch
                                                     : wc_align == WebkitAlign::Right
                                                         ? AlignItems::FlexEnd
                                                         : AlignItems::Center);
                    host = outer;
                }
                HtmlBlock *blk = make_block(host, FlexDirection::Column, 0);
                blk->m_bg = bg;
                decorate_block(blk, el, B);
                Style cst = st;
                TextAlignment ta = child_align;
                apply_cascade(el, cst, ta, B);
                cst.superscript = st.superscript;
                reset_box_style(cst);
                build_children(blk, &el->children, cst, B, list_depth, ta, depth+1);
            }
            break;
        }
        }
        // Tag the newly created wrapper(s) for block <a> so the whole tile is clickable
        if (!anchor_href.empty() && container->children().size() > anchor_before) {
            for (size_t ai = anchor_before; ai < container->children().size(); ++ai) {
                Widget *aw = container->children()[ai];
                if (auto *ab = dynamic_cast<HtmlBlock*>(aw)) {
                    if (ab->m_link_url.empty()) ab->m_link_url = anchor_href;
                }
                // If the block_anchor created an inner wrapper (e.g. outer Center), tag its child too
                for (Widget *inner : aw->children())
                    if (auto *ib = dynamic_cast<HtmlBlock*>(inner))
                        if (ib->m_link_url.empty()) ib->m_link_url = anchor_href;
            }
        }
    }
    flush();
}

// ---------------------------------------------------------------------------
// HtmlText editing. One leaf holds the caret. Typing never calls
// Screen::perform_layout — that would fight the compose window's size.
// ---------------------------------------------------------------------------

static size_t utf8_step_next(const std::string &s, size_t col) {
    if (col >= s.size()) return s.size();
    ++col;
    while (col < s.size() && ((unsigned char)s[col] & 0xC0) == 0x80) ++col;
    return col;
}

static size_t utf8_step_prev(const std::string &s, size_t col) {
    if (col == 0) return 0;
    --col;
    while (col > 0 && ((unsigned char)s[col] & 0xC0) == 0x80) --col;
    return col;
}

struct EditHit { size_t run = 0; size_t in_run = 0; };

/* Column is in the paragraph's text bytes. Image runs have no bytes. */
static EditHit run_at(const Paragraph &p, size_t col) {
    size_t acc = 0;
    for (size_t i = 0; i < p.runs.size(); ++i) {
        if (p.runs[i].isImageRun) continue;
        size_t n = p.runs[i].content.size();
        if (col <= acc + n) return { i, col > acc ? col - acc : 0 };
        acc += n;
    }
    if (p.runs.empty()) return {};
    return { p.runs.size() - 1, p.runs.back().isImageRun
                                 ? 0 : p.runs.back().content.size() };
}

static void erase_cols(Paragraph *p, size_t a, size_t b) {
    if (!p || b <= a) return;
    size_t acc = 0;
    for (Text &r : p->runs) {
        if (r.isImageRun) continue;
        size_t n = r.content.size();
        size_t lo = acc, hi = acc + n;
        size_t ea = std::max(a, lo), eb = std::min(b, hi);
        if (ea < eb) r.content.erase(ea - lo, eb - ea);
        acc = hi;
    }
    p->runs.erase(std::remove_if(p->runs.begin(), p->runs.end(),
        [](const Text &t) { return !t.isImageRun && t.content.empty(); }),
        p->runs.end());
}

static void collect_text_leaves(Widget *w, std::vector<HtmlText *> &out) {
    if (!w) return;
    if (auto *ht = dynamic_cast<HtmlText *>(w)) { out.push_back(ht); return; }
    if (dynamic_cast<HtmlBlock *>(w) || dynamic_cast<HtmlDocument *>(w)) {
        for (Widget *c : w->children())
            collect_text_leaves(c, out);
    }
}

/* Screen drag coordinates ignore ZoomScrollPanel's pan and zoom.
 * Map a screen point into this leaf's parent space, which is where
 * Document word positions and richHitTest live. */
static Vector2i parent_point_from_screen(const HtmlText *ht, const Vector2i &screen_p) {
    const ZoomScrollPanel *zsp = nullptr;
    const HtmlDocument *hd = nullptr;
    for (const Widget *w = ht; w; w = w->parent()) {
        if (!zsp) zsp = dynamic_cast<const ZoomScrollPanel *>(w);
        if (!hd) hd = dynamic_cast<const HtmlDocument *>(w);
    }
    Vector2i doc_pt;
    if (zsp && zsp->parent()) {
        Vector2i in_parent = screen_p - zsp->parent()->absolute_position();
        doc_pt = zsp->to_child(in_parent);
    } else if (hd) {
        doc_pt = screen_p - hd->absolute_position();
    } else if (ht->parent()) {
        return screen_p - ht->parent()->absolute_position();
    } else {
        return screen_p;
    }
    Vector2i leaf_p = doc_pt;
    for (const Widget *w = ht; w && w->parent() && w->parent() != hd; w = w->parent())
        leaf_p -= w->parent()->position();
    return leaf_p;
}

struct DocHit { HtmlText *leaf = nullptr; size_t para = 0; size_t col = 0; };

static DocHit hit_document(HtmlDocument *hd, const Vector2i &screen_p) {
    DocHit best;
    if (!hd) return best;
    Screen *scr = hd->screen();
    NVGcontext *ctx = scr ? scr->nvg_context() : nullptr;
    std::vector<HtmlText *> leaves;
    collect_text_leaves(hd, leaves);
    int best_dist = 1 << 30;
    for (HtmlText *ht : leaves) {
        if (!ht->parent()) continue;
        Vector2i pt = parent_point_from_screen(ht, screen_p);
        if (ctx) ht->ensure_layout(ctx);
        bool inside = ht->contains(pt);
        Vector2i local = pt - ht->position();
        int dist = 0;
        if (local.y() < 0) dist = -local.y();
        else if (local.y() >= ht->height()) dist = local.y() - ht->height();
        if (!inside && dist >= best_dist) continue;
        size_t para = 0, col = 0;
        if (ctx && !ht->m_doc.m_rich_layout.empty()) {
            std::pair<size_t, size_t> hit =
                ht->m_doc.richHitTest(ctx, (float)pt.x(), (float)pt.y());
            para = hit.first;
            col = hit.second;
        }
        if (inside) return { ht, para, col };
        best_dist = dist;
        best = { ht, para, col };
    }
    return best;
}

static void autoscroll_for_drag(Widget *from, const Vector2i &screen_p) {
    for (Widget *w = from; w; w = w->parent()) {
        if (auto *sp = dynamic_cast<ScrollPanel *>(w)) {
            Vector2i o = sp->absolute_position();
            float frac = sp->scroll().y();
            if (screen_p.y() < o.y() + 28) frac -= 0.04f;
            else if (screen_p.y() > o.y() + sp->height() - 28) frac += 0.04f;
            else return;
            if (frac < 0.f) frac = 0.f;
            if (frac > 1.f) frac = 1.f;
            sp->set_scroll(frac);
            return;
        }
        if (auto *zp = dynamic_cast<ZoomScrollPanel *>(w)) {
            Vector2i o = zp->absolute_position();
            float y = zp->scroll().y();
            if (screen_p.y() < o.y() + 28) y -= 0.04f;
            else if (screen_p.y() > o.y() + zp->height() - 28) y += 0.04f;
            else return;
            if (y < 0.f) y = 0.f;
            if (y > 1.f) y = 1.f;
            zp->set_scroll(y);
            return;
        }
    }
}

HtmlDocument *HtmlText::owner() const {
    for (const Widget *p = parent(); p; p = p->parent())
        if (auto *hd = dynamic_cast<const HtmlDocument *>(p))
            return const_cast<HtmlDocument *>(hd);
    return nullptr;
}

bool HtmlText::editing() const {
    HtmlDocument *hd = owner();
    return hd && hd->editable();
}

void HtmlText::ensure_layout(NVGcontext *ctx) {
    if (!ctx) return;
    int w = m_size.x() > 0 ? m_size.x() : m_measured_w;
    w = std::max(w, 10);
    if (w == m_measured_w && m_measured_h > 0 && !m_doc.layoutDirty() &&
        !m_doc.m_rich_layout.empty())
        return;
    m_doc.contentWidth = (float)w;
    m_doc.layout_only = true;
    m_doc.draw(ctx, (float)m_pos.x(), (float)m_pos.y());
    m_doc.layout_only = false;
    m_measured_h = (int)std::ceil(m_doc.last_drawn_height);
    m_measured_w = w;
}

void HtmlText::solo_caret() {
    HtmlDocument *hd = owner();
    if (!hd) return;
    std::vector<HtmlText *> leaves;
    collect_text_leaves(hd, leaves);
    for (HtmlText *ht : leaves) {
        if (ht == this) continue;
        ht->m_have_caret = false;
        ht->m_typing_on = false;
    }
}

void HtmlText::place_caret(size_t para, size_t col, bool extend) {
    solo_caret();
    if (m_doc.paragraphs.empty()) m_doc.addParagraph();
    if (para >= m_doc.paragraphs.size()) para = m_doc.paragraphs.size() - 1;
    size_t n = m_doc.paragraphs[para]->byte_length();
    if (col > n) col = n;
    m_have_caret = true;
    m_caret_para = para;
    m_caret_col = col;
    if (!extend) m_typing_on = false;
    if (HtmlDocument *hd = owner())
        hd->set_text_caret(this, m_caret_para, m_caret_col, extend);
}

Style HtmlText::style_for_insert() const {
    if (m_typing_on) return m_typing;
    Style s;
    s.fontSize = 17.f;
    if (HtmlDocument *hd = owner()) s.fgColor = hd->text_color();
    if (m_caret_para >= m_doc.paragraphs.size()) return s;
    const Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    size_t acc = 0;
    const Text *last = nullptr;
    for (const Text &r : p->runs) {
        if (r.isImageRun) continue;
        last = &r;
        if (m_caret_col <= acc + r.content.size()) return r.style;
        acc += r.content.size();
    }
    if (last) return last->style;
    return s;
}

void HtmlText::ensure_text_para() {
    if (m_doc.paragraphs.empty()) {
        m_doc.addParagraph();
        m_caret_para = 0;
        m_caret_col = 0;
        return;
    }
    if (m_caret_para >= m_doc.paragraphs.size())
        m_caret_para = m_doc.paragraphs.size() - 1;
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    if (!p->isImage && !p->isRule) return;
    Paragraph *n = m_doc.insertParagraph(m_caret_para + 1);
    n->alignment = p->alignment;
    m_caret_para += 1;
    m_caret_col = 0;
}

static void inject_chunk(Paragraph *par, size_t col, const std::string &chunk,
                         const Style &st, bool fresh_run) {
    if (!par || chunk.empty()) return;
    if (par->runs.empty()) {
        par->addText(chunk, st);
        return;
    }
    if (fresh_run) {
        size_t ri = par->split_run_at(col);
        Text t;
        t.style = st;
        t.content = chunk;
        if (ri > par->runs.size()) ri = par->runs.size();
        par->runs.insert(par->runs.begin() + (ptrdiff_t)ri, std::move(t));
        return;
    }
    EditHit h = run_at(*par, col);
    if (h.run >= par->runs.size() || par->runs[h.run].isImageRun) {
        Text t;
        t.style = st;
        t.content = chunk;
        size_t at = std::min(h.run + (h.run < par->runs.size() &&
                                      par->runs[h.run].isImageRun ? 1 : 0),
                             par->runs.size());
        par->runs.insert(par->runs.begin() + (ptrdiff_t)at, std::move(t));
        return;
    }
    par->runs[h.run].content.insert(h.in_run, chunk);
}

/* Split the caret paragraph. At the end of a heading the new paragraph
 * is body text, so the heading size does not leak onto the next line. */
void HtmlText::break_paragraph() {
    ensure_text_para();
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    size_t col = std::min(m_caret_col, p->byte_length());
    bool at_end = col >= p->byte_length();
    Paragraph *np = m_doc.insertParagraph(m_caret_para + 1);
    np->alignment = p->alignment;
    np->isBullet = p->isBullet;
    np->leftIndent = p->leftIndent;
    np->headerLevel = (at_end && p->headerLevel > 0) ? 0 : p->headerLevel;

    std::vector<Text> tail;
    size_t acc = 0;
    for (size_t i = 0; i < p->runs.size();) {
        Text &r = p->runs[i];
        if (r.isImageRun) {
            if (acc >= col) {
                tail.push_back(std::move(r));
                p->runs.erase(p->runs.begin() + (ptrdiff_t)i);
                continue;
            }
            ++i;
            continue;
        }
        size_t n = r.content.size();
        if (acc + n <= col) { acc += n; ++i; continue; }
        if (acc >= col) {
            tail.push_back(std::move(r));
            p->runs.erase(p->runs.begin() + (ptrdiff_t)i);
            continue;
        }
        Text rest;
        rest.style = r.style;
        rest.linkUrl = r.linkUrl;
        rest.content = r.content.substr(col - acc);
        r.content.erase(col - acc);
        tail.push_back(std::move(rest));
        ++i;
        while (i < p->runs.size()) {
            tail.push_back(std::move(p->runs[i]));
            p->runs.erase(p->runs.begin() + (ptrdiff_t)i);
        }
        break;
    }
    if (at_end && p->headerLevel > 0) {
        Style body = style_for_insert();
        body.bold = false;
        body.fontSize = 17.f;
        body.monospace = false;
        m_typing = body;
        m_typing_on = true;
        for (Text &t : tail) {
            if (t.isImageRun) continue;
            t.style.bold = false;
            t.style.fontSize = 17.f;
        }
    }
    for (Text &t : tail) np->runs.push_back(std::move(t));
    m_caret_para += 1;
    m_caret_col = 0;
}

void HtmlText::insert_text(const std::string &s) {
    if (!editing() || s.empty()) return;
    if (HtmlDocument *hd = owner()) {
        if (hd->has_text_selection()) {
            HtmlText *t = dynamic_cast<HtmlText *>(hd->delete_selection());
            if (t != this) {
                if (t) t->insert_text(s);
                return;
            }
        }
    }
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i != s.size() && s[i] != '\n') continue;
        std::string chunk = s.substr(start, i - start);
        if (!chunk.empty() && chunk.back() == '\r') chunk.pop_back();
        ensure_text_para();
        if (!chunk.empty()) {
            Paragraph *par = m_doc.paragraphs[m_caret_para].get();
            inject_chunk(par, m_caret_col, chunk, style_for_insert(), m_typing_on);
            m_caret_col += chunk.size();
        }
        if (i < s.size()) break_paragraph();
        start = i + 1;
    }
    after_edit();
}

void HtmlText::insert_newline() {
    if (!editing()) return;
    if (HtmlDocument *hd = owner()) {
        if (hd->has_text_selection()) {
            HtmlText *t = dynamic_cast<HtmlText *>(hd->delete_selection());
            if (t != this) {
                if (t) t->insert_newline();
                return;
            }
        }
    }
    ensure_text_para();
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    if (p->isBullet && p->byte_length() == 0) {
        if (p->leftIndent > 16.f) p->leftIndent -= 16.f;
        else { p->isBullet = false; p->leftIndent = 0.f; }
        after_edit();
        return;
    }
    break_paragraph();
    after_edit();
}

void HtmlText::delete_backward() {
    if (!editing() || m_doc.paragraphs.empty()) return;
    if (HtmlDocument *hd = owner()) {
        if (hd->has_text_selection()) { hd->delete_selection(); return; }
    }
    if (m_caret_para >= m_doc.paragraphs.size())
        m_caret_para = m_doc.paragraphs.size() - 1;
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    if (p->isImage || p->isRule) {
        m_doc.removeParagraph(m_caret_para);
        if (m_doc.paragraphs.empty()) m_doc.addParagraph();
        if (m_caret_para >= m_doc.paragraphs.size())
            m_caret_para = m_doc.paragraphs.size() - 1;
        m_caret_col = 0;
        after_edit();
        return;
    }
    if (m_caret_col > 0) {
        size_t prev = utf8_step_prev(p->plain_text(), m_caret_col);
        erase_cols(p, prev, m_caret_col);
        m_caret_col = prev;
        after_edit();
        return;
    }
    if (m_caret_para == 0) return;
    Paragraph *prevp = m_doc.paragraphs[m_caret_para - 1].get();
    if (prevp->isImage || prevp->isRule) {
        m_doc.removeParagraph(m_caret_para - 1);
        --m_caret_para;
        m_caret_col = 0;
        after_edit();
        return;
    }
    size_t join = prevp->byte_length();
    for (Text &r : p->runs) prevp->runs.push_back(std::move(r));
    m_doc.removeParagraph(m_caret_para);
    --m_caret_para;
    m_caret_col = join;
    after_edit();
}

void HtmlText::delete_forward() {
    if (!editing() || m_doc.paragraphs.empty()) return;
    if (HtmlDocument *hd = owner()) {
        if (hd->has_text_selection()) { hd->delete_selection(); return; }
    }
    if (m_caret_para >= m_doc.paragraphs.size())
        m_caret_para = m_doc.paragraphs.size() - 1;
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    if (p->isImage || p->isRule) {
        m_doc.removeParagraph(m_caret_para);
        if (m_doc.paragraphs.empty()) m_doc.addParagraph();
        m_caret_col = 0;
        after_edit();
        return;
    }
    if (m_caret_col < p->byte_length()) {
        size_t nxt = utf8_step_next(p->plain_text(), m_caret_col);
        erase_cols(p, m_caret_col, nxt);
        after_edit();
        return;
    }
    if (m_caret_para + 1 >= m_doc.paragraphs.size()) return;
    Paragraph *next = m_doc.paragraphs[m_caret_para + 1].get();
    if (next->isImage || next->isRule) {
        m_doc.removeParagraph(m_caret_para + 1);
        after_edit();
        return;
    }
    for (Text &r : next->runs) p->runs.push_back(std::move(r));
    m_doc.removeParagraph(m_caret_para + 1);
    after_edit();
}

static HtmlText *neighbor_leaf(HtmlText *self, int dir) {
    HtmlDocument *hd = self->owner();
    if (!hd) return nullptr;
    std::vector<HtmlText *> leaves;
    collect_text_leaves(hd, leaves);
    for (size_t i = 0; i < leaves.size(); ++i) {
        if (leaves[i] != self) continue;
        if (dir < 0 && i > 0) return leaves[i - 1];
        if (dir > 0 && i + 1 < leaves.size()) return leaves[i + 1];
        return nullptr;
    }
    return nullptr;
}

void HtmlText::move_to_leaf(HtmlText *ht, bool at_end, bool extend) {
    if (!ht) return;
    if (ht->m_doc.paragraphs.empty()) ht->m_doc.addParagraph();
    size_t para = 0;
    if (at_end) {
        para = ht->m_doc.paragraphs.size() - 1;
        for (int i = (int)ht->m_doc.paragraphs.size() - 1; i >= 0; --i) {
            Paragraph *p = ht->m_doc.paragraphs[(size_t)i].get();
            if (!p->isImage && !p->isRule) { para = (size_t)i; break; }
        }
    } else {
        for (size_t i = 0; i < ht->m_doc.paragraphs.size(); ++i) {
            Paragraph *p = ht->m_doc.paragraphs[i].get();
            if (!p->isImage && !p->isRule) { para = i; break; }
        }
    }
    size_t col = at_end ? ht->m_doc.paragraphs[para]->byte_length() : 0;
    ht->request_focus();
    ht->place_caret(para, col, extend);
    ht->after_move();
}

void HtmlText::move_caret(int key, int mods) {
    if (m_doc.paragraphs.empty()) m_doc.addParagraph();
    if (m_caret_para >= m_doc.paragraphs.size())
        m_caret_para = m_doc.paragraphs.size() - 1;
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    bool para_jump = (mods & SYSTEM_COMMAND_MOD) != 0;
    bool extend = (mods & GLFW_MOD_SHIFT) != 0;
    auto finish = [&]() {
        if (HtmlDocument *hd = owner())
            hd->set_text_caret(this, m_caret_para, m_caret_col, extend);
        after_move();
    };

    if (key == GLFW_KEY_HOME) {
        if (para_jump) {
            m_caret_para = 0;
            m_caret_col = 0;
        } else {
            m_caret_col = 0;
        }
        finish();
        return;
    }
    if (key == GLFW_KEY_END) {
        if (para_jump) m_caret_para = m_doc.paragraphs.size() - 1;
        m_caret_col = m_doc.paragraphs[m_caret_para]->byte_length();
        finish();
        return;
    }
    if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) {
        int dir = key == GLFW_KEY_LEFT ? -1 : 1;
        std::string text = p->plain_text();
        if (dir < 0 && m_caret_col > 0) {
            m_caret_col = utf8_step_prev(text, m_caret_col);
            finish();
            return;
        }
        if (dir > 0 && m_caret_col < text.size()) {
            m_caret_col = utf8_step_next(text, m_caret_col);
            finish();
            return;
        }
        int step = dir < 0 ? -1 : 1;
        for (int i = (int)m_caret_para + step;
             i >= 0 && i < (int)m_doc.paragraphs.size(); i += step) {
            Paragraph *q = m_doc.paragraphs[(size_t)i].get();
            if (q->isImage || q->isRule) continue;
            m_caret_para = (size_t)i;
            m_caret_col = dir < 0 ? q->byte_length() : 0;
            finish();
            return;
        }
        move_to_leaf(neighbor_leaf(this, dir), dir < 0, extend);
        return;
    }

    Screen *scr = screen();
    NVGcontext *ctx = scr ? scr->nvg_context() : nullptr;
    if (!ctx) { finish(); return; }
    ensure_layout(ctx);
    Document::CaretInfo info = m_doc.richCaretInfo(ctx, m_caret_para, m_caret_col);
    int dir = key == GLFW_KEY_UP ? -1 : 1;
    if (m_doc.m_rich_layout.empty() || !info.valid) {
        move_to_leaf(neighbor_leaf(this, dir), dir < 0, extend);
        return;
    }
    float x = info.x;
    float y = dir < 0 ? info.y_top - 3.f : info.y_bottom + 3.f;
    const Document::RichLine &first = m_doc.m_rich_layout.front();
    const Document::RichLine &last = m_doc.m_rich_layout.back();
    if ((dir < 0 && y < first.y_top) || (dir > 0 && y >= last.y_bottom)) {
        move_to_leaf(neighbor_leaf(this, dir), dir < 0, extend);
        return;
    }
    std::pair<size_t, size_t> hit = m_doc.richHitTest(ctx, x, y);
    m_caret_para = hit.first;
    m_caret_col = hit.second;
    if (m_caret_para >= m_doc.paragraphs.size())
        m_caret_para = m_doc.paragraphs.size() - 1;
    size_t n = m_doc.paragraphs[m_caret_para]->byte_length();
    if (m_caret_col > n) m_caret_col = n;
    m_typing_on = false;
    finish();
}

void HtmlText::toggle_inline(bool Style::*flag) {
    if (!editing()) return;
    if (HtmlDocument *hd = owner()) {
        if (hd->has_text_selection()) {
            hd->restyle_selection(flag);
            return;
        }
    }
    ensure_text_para();
    Style st = style_for_insert();
    st.*flag = !(st.*flag);
    m_typing = st;
    m_typing_on = true;
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    if (p->byte_length() > 0) {
        EditHit h = run_at(*p, m_caret_col);
        if (h.run < p->runs.size() && !p->runs[h.run].isImageRun)
            p->runs[h.run].style.*flag = st.*flag;
    }
    after_edit();
}

void HtmlText::toggle_header_level(int level) {
    if (!editing()) return;
    ensure_text_para();
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    if (p->isImage || p->isRule) return;
    if (p->headerLevel == level) level = 0;
    p->headerLevel = level;
    float scale = level == 1 ? 1.6f : level == 2 ? 1.4f : level == 3 ? 1.2f :
                  level >= 4 ? 1.1f : 1.f;
    for (Text &r : p->runs) {
        if (r.isImageRun) continue;
        r.style.bold = level > 0;
        r.style.fontSize = 17.f * scale;
    }
    m_typing_on = false;
    after_edit();
}

void HtmlText::toggle_bullet_para() {
    if (!editing()) return;
    ensure_text_para();
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    if (p->isImage || p->isRule) return;
    p->isBullet = !p->isBullet;
    if (p->isBullet) {
        if (p->leftIndent < 16.f) p->leftIndent = 16.f;
    } else if (p->leftIndent <= 16.f) {
        p->leftIndent = 0.f;
    }
    after_edit();
}

void HtmlText::toggle_code_para() {
    if (!editing()) return;
    ensure_text_para();
    Paragraph *p = m_doc.paragraphs[m_caret_para].get();
    bool any = false, all = true;
    for (const Text &r : p->runs) {
        if (r.isImageRun || r.content.empty()) continue;
        any = true;
        if (!r.style.monospace) all = false;
    }
    bool on = any ? !all : !m_typing.monospace;
    for (Text &r : p->runs) {
        if (r.isImageRun) continue;
        r.style.monospace = on;
    }
    m_typing = style_for_insert();
    m_typing.monospace = on;
    m_typing_on = true;
    after_edit();
}

void HtmlText::scroll_caret() {
    Screen *scr = screen();
    if (!scr || !m_have_caret) return;
    NVGcontext *ctx = scr->nvg_context();
    if (!ctx) return;
    ensure_layout(ctx);
    Document::CaretInfo info = m_doc.richCaretInfo(ctx, m_caret_para, m_caret_col);
    if (!info.valid) return;
    ScrollPanel *sp = nullptr;
    for (Widget *p = parent(); p && !sp; p = p->parent())
        sp = dynamic_cast<ScrollPanel *>(p);
    if (!sp || sp->child_count() < 1) return;
    Widget *content = sp->child_at(0);
    int content_h = content->height();
    int view_h = sp->height();
    if (view_h <= 0 || content_h <= view_h + 1) return;
    int y = (int)info.y_top;
    for (Widget *p = parent(); p && p != content; p = p->parent())
        y += p->position().y();
    int y1 = y + std::max(12, (int)(info.y_bottom - info.y_top));
    int span = content_h - view_h;
    int view_top = (int)(sp->scroll().y() * (float)span);
    float frac = sp->scroll().y();
    if (y < view_top)
        frac = (float)std::max(0, y) / (float)span;
    else if (y1 > view_top + view_h)
        frac = (float)std::max(0, y1 - view_h) / (float)span;
    else
        return;
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    sp->set_scroll(frac);
}

void HtmlText::after_edit() {
    m_doc.markLayoutDirty();
    m_measured_w = -1;
    m_natural_w = -1;
    scroll_caret();
    if (HtmlDocument *hd = owner()) {
        hd->note_edit();
        hd->request_reflow();
    }
    if (Screen *s = screen()) s->redraw();
}

void HtmlText::after_move() {
    scroll_caret();
    if (HtmlDocument *hd = owner()) hd->caret_moved();
    if (Screen *s = screen()) s->redraw();
}

bool HtmlText::keyboard_event(int key, int, int action, int mods) {
    if (!editing() || !m_have_caret) return false;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) {
        switch (key) {
        case GLFW_KEY_BACKSPACE: case GLFW_KEY_DELETE:
        case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER:
        case GLFW_KEY_LEFT: case GLFW_KEY_RIGHT:
        case GLFW_KEY_UP: case GLFW_KEY_DOWN:
        case GLFW_KEY_HOME: case GLFW_KEY_END:
            return true;
        default:
            return false;
        }
    }
    bool cmd = (mods & SYSTEM_COMMAND_MOD) != 0;
    if (cmd && key == GLFW_KEY_B) { toggle_inline(&Style::bold); return true; }
    if (cmd && key == GLFW_KEY_I) { toggle_inline(&Style::italic); return true; }
    if (cmd && key == GLFW_KEY_U) { toggle_inline(&Style::underline); return true; }
    switch (key) {
    case GLFW_KEY_BACKSPACE: delete_backward(); return true;
    case GLFW_KEY_DELETE:    delete_forward();  return true;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:  insert_newline();  return true;
    case GLFW_KEY_LEFT: case GLFW_KEY_RIGHT:
    case GLFW_KEY_UP: case GLFW_KEY_DOWN:
    case GLFW_KEY_HOME: case GLFW_KEY_END:
        move_caret(key, mods);
        return true;
    default:
        return false;
    }
}

bool HtmlText::keyboard_character_event(unsigned int codepoint) {
    if (!editing() || !m_have_caret || codepoint < 32) return false;
    char buf[5] = {0};
    if (codepoint < 0x80) {
        buf[0] = (char)codepoint;
    } else if (codepoint < 0x800) {
        buf[0] = (char)(0xC0 | (codepoint >> 6));
        buf[1] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        buf[0] = (char)(0xE0 | (codepoint >> 12));
        buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        buf[0] = (char)(0xF0 | (codepoint >> 18));
        buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (codepoint & 0x3F));
    }
    insert_text(buf);
    return true;
}

void HtmlText::erase_span(size_t p0, size_t c0, size_t p1, size_t c1) {
    if (m_doc.paragraphs.empty()) {
        m_doc.addParagraph();
        m_caret_para = 0;
        m_caret_col = 0;
        return;
    }
    if (p0 > p1 || (p0 == p1 && c0 > c1)) {
        std::swap(p0, p1);
        std::swap(c0, c1);
    }
    if (p0 >= m_doc.paragraphs.size()) return;
    if (p1 >= m_doc.paragraphs.size()) {
        p1 = m_doc.paragraphs.size() - 1;
        c1 = m_doc.paragraphs[p1]->byte_length();
    }
    auto clamp_col = [](const Paragraph *p, size_t c) {
        size_t n = p->byte_length();
        return c > n ? n : c;
    };
    c0 = clamp_col(m_doc.paragraphs[p0].get(), c0);
    c1 = clamp_col(m_doc.paragraphs[p1].get(), c1);
    if (p0 == p1) {
        Paragraph *p = m_doc.paragraphs[p0].get();
        if (!p->isImage && !p->isRule)
            erase_cols(p, c0, c1);
        m_caret_para = p0;
        m_caret_col = c0;
        return;
    }

    Paragraph *start = m_doc.paragraphs[p0].get();
    Paragraph *end = m_doc.paragraphs[p1].get();
    bool start_block = start->isImage || start->isRule;
    bool end_block = end->isImage || end->isRule;
    if (!start_block)
        erase_cols(start, c0, start->byte_length());
    if (!end_block)
        erase_cols(end, 0, c1);
    /* Paragraphs strictly inside the range, including images, go away. */
    for (size_t i = p1; i > p0 + 1; --i)
        m_doc.removeParagraph(i - 1);
    if (p0 + 1 < m_doc.paragraphs.size()) {
        Paragraph *dst = m_doc.paragraphs[p0].get();
        Paragraph *src = m_doc.paragraphs[p0 + 1].get();
        bool dst_block = dst->isImage || dst->isRule;
        bool src_block = src->isImage || src->isRule;
        /* A block touched only at column 0 stays. Text paragraphs join. */
        if (src_block && c1 == 0) {
            if (dst_block && c0 == 0)
                m_doc.removeParagraph(p0);
        } else if (dst_block && c0 == 0) {
            m_doc.removeParagraph(p0);
        } else if (!dst_block && !src_block) {
            for (Text &r : src->runs)
                dst->runs.push_back(std::move(r));
            m_doc.removeParagraph(p0 + 1);
        } else if (!dst_block && src_block) {
            m_doc.removeParagraph(p0 + 1);
        }
    }
    if (m_doc.paragraphs.empty()) m_doc.addParagraph();
    if (p0 >= m_doc.paragraphs.size())
        p0 = m_doc.paragraphs.size() - 1;
    m_caret_para = p0;
    m_caret_col = std::min(c0, m_doc.paragraphs[p0]->byte_length());
}

void HtmlText::erase_to_end(size_t para, size_t col) {
    if (m_doc.paragraphs.empty()) {
        m_doc.addParagraph();
        m_caret_para = 0;
        m_caret_col = 0;
        return;
    }
    if (para >= m_doc.paragraphs.size())
        para = m_doc.paragraphs.size() - 1;
    Paragraph *p = m_doc.paragraphs[para].get();
    size_t n = p->byte_length();
    if (col > n) col = n;
    if ((p->isImage || p->isRule) && col == 0) {
        while (m_doc.paragraphs.size() > para)
            m_doc.removeParagraph(m_doc.paragraphs.size() - 1);
    } else {
        if (!p->isImage && !p->isRule)
            erase_cols(p, col, p->byte_length());
        while (m_doc.paragraphs.size() > para + 1)
            m_doc.removeParagraph(m_doc.paragraphs.size() - 1);
    }
    if (m_doc.paragraphs.empty()) m_doc.addParagraph();
    if (para >= m_doc.paragraphs.size())
        para = m_doc.paragraphs.size() - 1;
    m_caret_para = para;
    m_caret_col = std::min(col, m_doc.paragraphs[para]->byte_length());
}

void HtmlText::erase_from_start(size_t para, size_t col) {
    if (m_doc.paragraphs.empty()) {
        m_doc.addParagraph();
        return;
    }
    if (para >= m_doc.paragraphs.size()) {
        m_doc.paragraphs.clear();
        m_doc.addParagraph();
        m_caret_para = 0;
        m_caret_col = 0;
        return;
    }
    for (size_t i = 0; i < para && !m_doc.paragraphs.empty(); ++i)
        m_doc.removeParagraph(0);
    if (m_doc.paragraphs.empty()) {
        m_doc.addParagraph();
        return;
    }
    Paragraph *p = m_doc.paragraphs[0].get();
    size_t n = p->byte_length();
    if (col > n) col = n;
    if (!p->isImage && !p->isRule)
        erase_cols(p, 0, col);
    m_caret_para = 0;
    m_caret_col = 0;
}

void HtmlText::mark_dirty_leaf() {
    m_doc.markLayoutDirty();
    m_measured_w = -1;
    m_natural_w = -1;
    m_have_caret = false;
    m_typing_on = false;
}

void HtmlText::draw_selection(NVGcontext *ctx) {
    HtmlDocument *hd = owner();
    if (!hd || m_doc.m_rich_layout.empty()) return;
    size_t p0 = 0, c0 = 0, p1 = 0, c1 = 0;
    if (!hd->leaf_selection(this, p0, c0, p1, c1)) return;
    for (const Document::RichLine &rl : m_doc.m_rich_layout) {
        if (rl.para_idx < p0 || rl.para_idx > p1) continue;
        size_t lo, hi;
        if (rl.para_idx == p0 && rl.para_idx == p1) {
            lo = std::max(c0, rl.byte_start);
            hi = std::min(c1, rl.byte_end);
        } else if (rl.para_idx == p0) {
            lo = std::max(c0, rl.byte_start);
            hi = rl.byte_end;
        } else if (rl.para_idx == p1) {
            lo = rl.byte_start;
            hi = std::min(c1, rl.byte_end);
        } else {
            lo = rl.byte_start;
            hi = rl.byte_end;
        }
        if (lo > hi) continue;
        float x0, x1;
        if (rl.words.empty()) {
            x0 = rl.x_start;
            x1 = rl.x_start + 4.f;
        } else {
            auto x_for = [&](size_t bc) -> float {
                Document::CaretInfo ci = m_doc.richCaretInfo(ctx, rl.para_idx, bc);
                return ci.valid ? ci.x : rl.x_start;
            };
            x0 = x_for(lo);
            x1 = x_for(hi);
            if (rl.para_idx < p1 && hi == rl.byte_end)
                x1 = rl.words.back().x + rl.words.back().advance + 6.f;
        }
        nvgBeginPath(ctx);
        nvgRect(ctx, x0, rl.y_top,
                std::max(2.f, x1 - x0),
                std::max(2.f, rl.y_bottom - rl.y_top));
        nvgFillColor(ctx, nvgRGBA(70, 110, 180, 130));
        nvgFill(ctx);
    }
}

bool HtmlText::mouse_drag_event(const Vector2i &p, const Vector2i &rel,
                                int button, int) {
    if (!(button & (1 << GLFW_MOUSE_BUTTON_1)))
        return false;
    if (!parent()) return true;
    HtmlDocument *hd = owner();
    if (!hd) return true;
    /* rel is screen pixels since the press. A converted point can
     * disagree with m_down_pt under pan and zoom and turn a click
     * into a selection, which then refuses the link. */
    m_press_travel += std::abs(rel.x()) + std::abs(rel.y());
    if (m_press_travel > 3)
        m_press_dragged = true;
    if (!m_press_dragged) return true;
    /* Screen's drag point ignores ZoomScrollPanel pan and zoom.
     * Recover the screen point, then map it back into leaf space. */
    Vector2i screen_p = p + parent()->absolute_position();
    autoscroll_for_drag(this, screen_p);
    DocHit h = hit_document(hd, screen_p);
    if (h.leaf) {
        h.leaf->request_focus();
        h.leaf->place_caret(h.para, h.col, true);
        h.leaf->after_move();
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HtmlDocument
// ---------------------------------------------------------------------------

HtmlDocument::HtmlDocument(Widget *parent) : Widget(parent) {
    // Madeleine's u-row gaps are already encoded as v-container-padding paddings;
    // a global 16/4 padding+gap doubles the inter-row gap and creates the top 2px
    // + bottom extra you flagged.  Keep HtmlDocument gap 0 and rely on per-row
    // padding + Document::paragraphSpacing (tightened above) for vertical rhythm.
    set_layout(new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                              AlignItems::Stretch, 0, 0));
    set_live(true);   // see make_block: never bake HTML content into draw lists
#ifdef DEBUG
    if (debug_draw())
        fprintf(stderr, "[trace] HtmlDocument flatten+skip-identity-save\n");
#endif
}

bool HtmlDocument::notify_link_click(const std::string &url) {
    return on_link_click ? on_link_click(url) : false;
}

void HtmlDocument::set_hover_url(const std::string &url) {
    if (m_last_hover_url == url) return;
    m_last_hover_url = url;
    if (on_link_hover) on_link_hover(url);
}
bool HtmlDocument::mouse_enter_event(const Vector2i &p, bool enter) {
    if (!enter) set_hover_url("");
    return Widget::mouse_enter_event(p, enter);
}
void HtmlDocument::clear() {
    m_anchor_leaf = nullptr;
    m_focus_leaf = nullptr;
    m_anchor_para = 0;
    m_anchor_col = 0;
    m_focus_para = 0;
    m_focus_col = 0;
    set_hover_url("");
#ifdef DEBUG
    if (debug_draw())
        fprintf(stderr, "[trace] HtmlDocument::clear %p children=%zu\n",
                (void *)this, m_children.size());
#endif
    while (!m_children.empty())
        remove_child(m_children.back());
    m_has_remote = false;
    m_reflow_budget = kReflowBudgetMax;
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
    std::function<void(Widget *,int)> walk = [&](Widget *w, int depth) {
        if (depth > 80) return;
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
            walk(c, depth+1);
    };
    walk(this, 0);
    if (n > 0)
        relayout();
    return n;
}

/* Gumbo has no nesting cap and is superlinear in depth: ~40k nested tags is a
 * 19s parse and ~100k exhausts memory.  Flat markup stays linear, so bound
 * both size and depth before the parser ever sees sender-controlled bytes. */
static const size_t kMaxHtmlBytes = 4u * 1024 * 1024;
static const int    kMaxHtmlDepth = 1024;

/* Void elements, plus those HTML5 auto-closes on the next sibling: neither
 * can stack, so counting them would reject ordinary unclosed <p>/<td> mail. */
static bool tag_holds_depth(const std::string &t) {
    static const char *kFlat[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr",
        "dd", "dt", "li", "option", "p", "td", "th", "thead", "tbody",
        "tfoot", "tr"
    };
    for (const char *f : kFlat)
        if (t == f) return false;
    return true;
}

bool html_is_parseable(const std::string &html) {
    if (html.size() > kMaxHtmlBytes)
        return false;
    int depth = 0;
    for (size_t i = 0; i + 1 < html.size(); ++i) {
        if (html[i] != '<')
            continue;
        if (html.compare(i, 4, "<!--") == 0) {   // markup in a comment is not structure
            size_t e = html.find("-->", i + 4);
            if (e == std::string::npos) break;
            i = e + 2;
            continue;
        }
        size_t j = i + 1;
        bool close = html[j] == '/';
        if (close) ++j;
        if (j >= html.size() || !std::isalpha((unsigned char)html[j]))
            continue;
        std::string name;
        while (j < html.size() && std::isalnum((unsigned char)html[j]))
            name += (char)std::tolower((unsigned char)html[j++]);
        if (close) {
            if (depth > 0) --depth;
        } else if (tag_holds_depth(name) && ++depth > kMaxHtmlDepth) {
            return false;
        }
    }
    return true;
}

static std::string html_esc(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default:  out += c; break;
        }
    }
    return out;
}

/* Inline runs. A link is underlined by the reader, so a link does not
 * also emit <u>. Newlines inside a run become <br>. */
static std::string inline_to_html(const Paragraph &para, bool in_heading) {
    std::string out;
    for (const Text &r : para.runs) {
        if (r.isImageRun) {
            out += "<img src=\"" + html_esc(r.image_src) + "\">";
            continue;
        }
        if (r.content.empty()) continue;
        std::string c = html_esc(r.content);
        for (size_t pos = 0; (pos = c.find('\n', pos)) != std::string::npos; ) {
            c.replace(pos, 1, "<br>");
            pos += 4;
        }
        std::string open, close;
        if (r.style.monospace) { open += "<code>"; close = "</code>" + close; }
        if (r.style.bold && !in_heading) { open += "<strong>"; close = "</strong>" + close; }
        if (r.style.italic) { open += "<em>"; close = "</em>" + close; }
        if (r.style.underline && r.linkUrl.empty()) {
            open += "<u>";
            close = "</u>" + close;
        }
        if (!r.linkUrl.empty()) {
            open += "<a href=\"" + html_esc(r.linkUrl) + "\">";
            close = "</a>" + close;
        }
        out += open + c + close;
    }
    return out;
}

/* Paragraphs, headings, lists, links, and images. Empty paragraphs are
 * kept (<p><br></p>) so a blank line the user typed survives. Tables
 * and CSS are not represented once this runs. */
static std::string leaves_to_html(const std::vector<HtmlText *> &leaves) {
    std::string out;
    int list_depth = 0;
    bool in_pre = false;
    /* A plain <p> stays open so following Paragraph::softBreak lines
     * rejoin it as <br>, the way they were parsed. */
    bool in_p = false;
    auto close_p = [&]() {
        if (in_p) { out += "</p>\n"; in_p = false; }
    };
    auto close_lists = [&]() {
        close_p();
        while (list_depth > 0) { out += "</ul>\n"; --list_depth; }
    };
    auto close_pre = [&]() {
        if (in_pre) { out += "</code></pre>\n"; in_pre = false; }
    };
    for (HtmlText *ht : leaves) {
        for (const auto &pp : ht->m_doc.paragraphs) {
            const Paragraph &para = *pp;
            if (para.isImage) {
                close_pre(); close_lists();
                out += "<p><img src=\"" + html_esc(para.image_src) + "\"></p>\n";
                continue;
            }
            if (para.isRule) {
                close_pre(); close_lists();
                out += "<hr>\n";
                continue;
            }
            bool is_code = !para.isBullet && para.headerLevel == 0 &&
                           para.byte_length() > 0;
            for (const Text &r : para.runs) {
                if (r.isImageRun || r.content.empty()) continue;
                if (!r.style.monospace) is_code = false;
            }
            if (is_code) {
                close_lists();
                if (!in_pre) { out += "<pre><code>"; in_pre = true; }
                else out += '\n';
                std::string t = para.plain_text();
                while (!t.empty() && t.back() == '\n') t.pop_back();
                out += html_esc(t);
                continue;
            }
            close_pre();
            if (para.isBullet) {
                close_p();
                int lvl = (int)(para.leftIndent / 16.f + 0.5f);
                if (lvl < 1) lvl = 1;
                while (list_depth < lvl) { out += "<ul>\n"; ++list_depth; }
                while (list_depth > lvl) { out += "</ul>\n"; --list_depth; }
                std::string inner = inline_to_html(para, false);
                if (inner.empty()) inner = "<br>";
                out += "<li>" + inner + "</li>\n";
                continue;
            }
            int level = para.headerLevel;
            if (level < 0 || level > 6) level = 0;
            std::string inner = inline_to_html(para, level > 0);
            if (!level && para.leftIndent <= 0.f && para.softBreak && in_p) {
                out += "<br>" + inner;
                continue;
            }
            close_lists();
            if (inner.empty()) inner = "<br>";
            if (level) {
                out += "<h" + std::to_string(level) + ">" + inner +
                       "</h" + std::to_string(level) + ">\n";
            } else if (para.leftIndent > 0.f) {
                out += "<blockquote><p>" + inner + "</p></blockquote>\n";
            } else {
                out += "<p>" + inner;
                in_p = true;
            }
        }
    }
    close_pre();
    close_lists();
    return out;
}

static int index_of_leaf(const std::vector<HtmlText *> &leaves, HtmlText *ht) {
    for (int i = 0; i < (int)leaves.size(); ++i)
        if (leaves[i] == ht) return i;
    return -1;
}

/* Anchor and focus ordered in document order. False when they coincide
 * or either leaf is no longer in the tree. */
static bool order_leaves(HtmlDocument *doc, Widget *anchor, size_t ap, size_t ac,
                         Widget *focus, size_t fp, size_t fc,
                         HtmlText *&lo, size_t &lp, size_t &lc,
                         HtmlText *&hi, size_t &hp, size_t &hc,
                         std::vector<HtmlText *> &leaves) {
    auto *a = dynamic_cast<HtmlText *>(anchor);
    auto *f = dynamic_cast<HtmlText *>(focus);
    if (!a || !f || (a == f && ap == fp && ac == fc)) return false;
    leaves.clear();
    collect_text_leaves(doc, leaves);
    int ia = index_of_leaf(leaves, a);
    int ib = index_of_leaf(leaves, f);
    if (ia < 0 || ib < 0) return false;
    lo = a; lp = ap; lc = ac;
    hi = f; hp = fp; hc = fc;
    if (ia > ib || (ia == ib && (ap > fp || (ap == fp && ac > fc)))) {
        std::swap(lo, hi);
        std::swap(lp, hp);
        std::swap(lc, hc);
    }
    return true;
}

static HtmlText *find_caret_leaf(const HtmlDocument *doc) {
    std::vector<HtmlText *> leaves;
    collect_text_leaves(const_cast<HtmlDocument *>(doc), leaves);
    for (HtmlText *ht : leaves)
        if (ht->m_have_caret) return ht;
    return nullptr;
}

static HtmlText *need_caret_leaf(HtmlDocument *doc) {
    if (HtmlText *ht = find_caret_leaf(doc)) return ht;
    std::vector<HtmlText *> leaves;
    collect_text_leaves(doc, leaves);
    if (leaves.empty()) return nullptr;
    leaves.front()->place_caret(0, 0);
    return leaves.front();
}

void HtmlDocument::set_editable(bool on) { m_editable = on; }

std::string HtmlDocument::edited_html() const {
    if (!m_edit_dirty) return m_source_html;
    std::vector<HtmlText *> leaves;
    collect_text_leaves(const_cast<HtmlDocument *>(this), leaves);
    return "<!DOCTYPE html>\n<html><body>\n" + leaves_to_html(leaves) +
           "</body></html>\n";
}

void HtmlDocument::toggle_inline_style(InlineStyle style) {
    if (!m_editable) return;
    HtmlText *ht = need_caret_leaf(this);
    if (!ht) return;
    if (style == InlineStyle::Bold) ht->toggle_inline(&Style::bold);
    else if (style == InlineStyle::Italic) ht->toggle_inline(&Style::italic);
    else ht->toggle_inline(&Style::underline);
}

void HtmlDocument::toggle_header(int level) {
    if (!m_editable) return;
    if (HtmlText *ht = need_caret_leaf(this)) ht->toggle_header_level(level);
}

void HtmlDocument::toggle_bullet() {
    if (!m_editable) return;
    if (HtmlText *ht = need_caret_leaf(this)) ht->toggle_bullet_para();
}

void HtmlDocument::toggle_code() {
    if (!m_editable) return;
    if (HtmlText *ht = need_caret_leaf(this)) ht->toggle_code_para();
}

Style HtmlDocument::caret_style() const {
    if (HtmlText *ht = find_caret_leaf(this)) return ht->style_for_insert();
    Style s;
    s.fontSize = 17.f;
    s.fgColor = m_text;
    return s;
}

int HtmlDocument::caret_header() const {
    HtmlText *ht = find_caret_leaf(this);
    if (!ht || ht->m_caret_para >= ht->m_doc.paragraphs.size()) return 0;
    return ht->m_doc.paragraphs[ht->m_caret_para]->headerLevel;
}

bool HtmlDocument::caret_bullet() const {
    HtmlText *ht = find_caret_leaf(this);
    if (!ht || ht->m_caret_para >= ht->m_doc.paragraphs.size()) return false;
    return ht->m_doc.paragraphs[ht->m_caret_para]->isBullet;
}

bool HtmlDocument::caret_code() const {
    HtmlText *ht = find_caret_leaf(this);
    if (!ht || ht->m_caret_para >= ht->m_doc.paragraphs.size()) return false;
    const Paragraph *p = ht->m_doc.paragraphs[ht->m_caret_para].get();
    if (ht->m_typing_on && p->byte_length() == 0) return ht->m_typing.monospace;
    bool any = false;
    for (const Text &r : p->runs) {
        if (r.isImageRun || r.content.empty()) continue;
        any = true;
        if (!r.style.monospace) return false;
    }
    return any;
}

void HtmlDocument::focus_editor() {
    HtmlText *ht = find_caret_leaf(this);
    if (!ht) {
        std::vector<HtmlText *> leaves;
        collect_text_leaves(this, leaves);
        if (leaves.empty()) return;
        ht = leaves.front();
        ht->place_caret(0, 0);
    }
    ht->request_focus();
}

void HtmlDocument::note_edit() { touch_edit(); }

void HtmlDocument::caret_moved() { notify_caret(); }

void HtmlDocument::touch_edit() {
    m_edit_dirty = true;
    notify_caret();
}

void HtmlDocument::notify_caret() {
    if (on_caret) on_caret();
}

void HtmlDocument::set_text_caret(Widget *leaf, size_t para, size_t col, bool extend) {
    if (!extend || !m_anchor_leaf) {
        m_anchor_leaf = leaf;
        m_anchor_para = para;
        m_anchor_col = col;
    }
    m_focus_leaf = leaf;
    m_focus_para = para;
    m_focus_col = col;
}

bool HtmlDocument::has_text_selection() const {
    auto *a = dynamic_cast<HtmlText *>(m_anchor_leaf);
    auto *f = dynamic_cast<HtmlText *>(m_focus_leaf);
    if (!a || !f) return false;
    return a != f || m_anchor_para != m_focus_para || m_anchor_col != m_focus_col;
}

bool HtmlDocument::leaf_selection(const Widget *leaf, size_t &p0, size_t &c0,
                                  size_t &p1, size_t &c1) const {
    HtmlText *lo = nullptr, *hi = nullptr;
    size_t lp = 0, lc = 0, hp = 0, hc = 0;
    std::vector<HtmlText *> leaves;
    if (!order_leaves(const_cast<HtmlDocument *>(this),
                      m_anchor_leaf, m_anchor_para, m_anchor_col,
                      m_focus_leaf, m_focus_para, m_focus_col,
                      lo, lp, lc, hi, hp, hc, leaves))
        return false;
    auto *me = dynamic_cast<HtmlText *>(const_cast<Widget *>(leaf));
    int im = index_of_leaf(leaves, me);
    int il = index_of_leaf(leaves, lo);
    int ih = index_of_leaf(leaves, hi);
    if (im < 0 || im < il || im > ih) return false;
    if (me->m_doc.paragraphs.empty()) {
        p0 = c0 = p1 = c1 = 0;
        return im != il && im != ih;
    }
    size_t last = me->m_doc.paragraphs.size() - 1;
    if (im == il && im == ih) {
        p0 = lp; c0 = lc; p1 = hp; c1 = hc;
    } else if (im == il) {
        p0 = lp; c0 = lc;
        p1 = last;
        c1 = me->m_doc.paragraphs[last]->byte_length();
    } else if (im == ih) {
        p0 = 0; c0 = 0; p1 = hp; c1 = hc;
    } else {
        p0 = 0; c0 = 0;
        p1 = last;
        c1 = me->m_doc.paragraphs[last]->byte_length();
        return true;
    }
    return p0 != p1 || c0 != c1;
}

void HtmlDocument::select_word_at(Widget *leaf, size_t para, size_t col) {
    auto *ht = dynamic_cast<HtmlText *>(leaf);
    if (!ht) return;
    if (ht->m_doc.paragraphs.empty() || para >= ht->m_doc.paragraphs.size()) {
        ht->place_caret(0, 0, false);
        return;
    }
    Paragraph *p = ht->m_doc.paragraphs[para].get();
    if (p->isImage || p->isRule || p->byte_length() == 0) {
        ht->place_caret(para, 0, false);
        return;
    }
    std::string text = p->plain_text();
    size_t c = col > text.size() ? text.size() : col;
    auto is_word = [](char ch) {
        return std::isalnum((unsigned char)ch) || ch == '_' || (ch & 0x80);
    };
    bool word = is_word(text[c == text.size() ? c - 1 : c]);
    size_t start = c, end = c;
    while (start > 0 && is_word(text[start - 1]) == word) --start;
    while (end < text.size() && is_word(text[end]) == word) ++end;
    m_anchor_leaf = ht;
    m_anchor_para = para;
    m_anchor_col = start;
    ht->place_caret(para, end, true);
}

void HtmlDocument::select_paragraph_at(Widget *leaf, size_t para) {
    auto *ht = dynamic_cast<HtmlText *>(leaf);
    if (!ht) return;
    if (ht->m_doc.paragraphs.empty()) {
        ht->place_caret(0, 0, false);
        return;
    }
    if (para >= ht->m_doc.paragraphs.size())
        para = ht->m_doc.paragraphs.size() - 1;
    m_anchor_leaf = ht;
    m_anchor_para = para;
    m_anchor_col = 0;
    ht->place_caret(para, ht->m_doc.paragraphs[para]->byte_length(), true);
}

void HtmlDocument::select_all_text() {
    std::vector<HtmlText *> leaves;
    collect_text_leaves(this, leaves);
    if (leaves.empty()) return;
    HtmlText *first = leaves.front();
    HtmlText *last = leaves.back();
    first->place_caret(0, 0, false);
    size_t para = 0, col = 0;
    if (!last->m_doc.paragraphs.empty()) {
        para = last->m_doc.paragraphs.size() - 1;
        col = last->m_doc.paragraphs[para]->byte_length();
    }
    if (first != last || para != 0 || col != 0)
        last->place_caret(para, col, true);
    last->request_focus();
    if (Screen *s = screen()) s->redraw();
}

void HtmlDocument::copy_selection() {
    HtmlText *lo = nullptr, *hi = nullptr;
    size_t lp = 0, lc = 0, hp = 0, hc = 0;
    std::vector<HtmlText *> leaves;
    if (!order_leaves(this, m_anchor_leaf, m_anchor_para, m_anchor_col,
                      m_focus_leaf, m_focus_para, m_focus_col,
                      lo, lp, lc, hi, hp, hc, leaves))
        return;
    int il = index_of_leaf(leaves, lo);
    int ih = index_of_leaf(leaves, hi);
    std::string out;
    bool started = false;
    auto append_para = [&](HtmlText *ht, size_t pi, size_t a, size_t b) {
        if (started) out.push_back('\n');
        started = true;
        if (!ht || pi >= ht->m_doc.paragraphs.size()) return;
        std::string t = ht->m_doc.paragraphs[pi]->plain_text();
        if (a > t.size()) a = t.size();
        if (b > t.size()) b = t.size();
        if (b > a) out.append(t, a, b - a);
    };
    for (int i = il; i <= ih; ++i) {
        HtmlText *ht = leaves[i];
        if (ht->m_doc.paragraphs.empty()) {
            append_para(ht, 0, 0, 0);
            continue;
        }
        size_t last = ht->m_doc.paragraphs.size() - 1;
        size_t p0, c0, p1, c1;
        if (i == il && i == ih) {
            p0 = lp; c0 = lc; p1 = hp; c1 = hc;
        } else if (i == il) {
            p0 = lp; c0 = lc; p1 = last;
            c1 = ht->m_doc.paragraphs[last]->byte_length();
        } else if (i == ih) {
            p0 = 0; c0 = 0; p1 = hp; c1 = hc;
        } else {
            p0 = 0; c0 = 0; p1 = last;
            c1 = ht->m_doc.paragraphs[last]->byte_length();
        }
        if (p0 > p1) continue;
        for (size_t pi = p0; pi <= p1 && pi < ht->m_doc.paragraphs.size(); ++pi) {
            size_t a = pi == p0 ? c0 : 0;
            size_t b = pi == p1 ? c1 : ht->m_doc.paragraphs[pi]->byte_length();
            append_para(ht, pi, a, b);
        }
    }
    if (Screen *s = screen())
        glfwSetClipboardString(s->glfw_window(), out.c_str());
}

void HtmlDocument::paste_clipboard() {
    if (!m_editable) return;
    Screen *s = screen();
    const char *cb = s ? glfwGetClipboardString(s->glfw_window()) : nullptr;
    if (!cb || !*cb) return;
    std::string norm;
    norm.reserve(std::strlen(cb));
    for (size_t i = 0; cb[i]; ++i) {
        if (cb[i] == '\r') {
            norm.push_back('\n');
            if (cb[i + 1] == '\n') ++i;
        } else {
            norm.push_back(cb[i]);
        }
    }
    if (norm.empty()) return;
    HtmlText *ht = dynamic_cast<HtmlText *>(m_focus_leaf);
    if (!ht) ht = find_caret_leaf(this);
    if (!ht) {
        std::vector<HtmlText *> leaves;
        collect_text_leaves(this, leaves);
        if (leaves.empty()) return;
        ht = leaves.front();
        ht->place_caret(0, 0, false);
    }
    ht->request_focus();
    ht->insert_text(norm);
}

Widget *HtmlDocument::delete_selection() {
    if (!m_editable || !has_text_selection())
        return m_focus_leaf;
    HtmlText *lo = nullptr, *hi = nullptr;
    size_t lp = 0, lc = 0, hp = 0, hc = 0;
    std::vector<HtmlText *> leaves;
    if (!order_leaves(this, m_anchor_leaf, m_anchor_para, m_anchor_col,
                      m_focus_leaf, m_focus_para, m_focus_col,
                      lo, lp, lc, hi, hp, hc, leaves))
        return m_focus_leaf;
    int il = index_of_leaf(leaves, lo);
    int ih = index_of_leaf(leaves, hi);
    if (il == ih) {
        lo->erase_span(lp, lc, hp, hc);
        lo->place_caret(lo->m_caret_para, lo->m_caret_col, false);
        lo->after_edit();
        return lo;
    }
    lo->erase_to_end(lp, lc);
    hi->erase_from_start(hp, hc);
    for (int i = il + 1; i < ih; ++i) {
        leaves[i]->m_doc.paragraphs.clear();
        leaves[i]->m_doc.addParagraph();
        leaves[i]->mark_dirty_leaf();
    }
    hi->mark_dirty_leaf();
    lo->place_caret(lo->m_caret_para, lo->m_caret_col, false);
    lo->after_edit();
    return lo;
}

void HtmlDocument::restyle_selection(bool Style::*flag) {
    if (!m_editable || !has_text_selection()) return;
    HtmlText *lo = nullptr, *hi = nullptr;
    size_t lp = 0, lc = 0, hp = 0, hc = 0;
    std::vector<HtmlText *> leaves;
    if (!order_leaves(this, m_anchor_leaf, m_anchor_para, m_anchor_col,
                      m_focus_leaf, m_focus_para, m_focus_col,
                      lo, lp, lc, hi, hp, hc, leaves))
        return;
    int il = index_of_leaf(leaves, lo);
    int ih = index_of_leaf(leaves, hi);

    auto range_for = [&](int i, HtmlText *ht, size_t &p0, size_t &c0,
                         size_t &p1, size_t &c1) -> bool {
        if (ht->m_doc.paragraphs.empty()) return false;
        size_t last = ht->m_doc.paragraphs.size() - 1;
        if (i == il && i == ih) {
            p0 = lp; c0 = lc; p1 = hp; c1 = hc;
        } else if (i == il) {
            p0 = lp; c0 = lc; p1 = last;
            c1 = ht->m_doc.paragraphs[last]->byte_length();
        } else if (i == ih) {
            p0 = 0; c0 = 0; p1 = hp; c1 = hc;
        } else {
            p0 = 0; c0 = 0; p1 = last;
            c1 = ht->m_doc.paragraphs[last]->byte_length();
        }
        return p0 < p1 || (p0 == p1 && c0 < c1);
    };
    auto touch_run = [&](const Paragraph *p, size_t lo_c, size_t hi_c,
                         bool &any, bool &all_set) {
        size_t acc = 0;
        for (const Text &r : p->runs) {
            if (r.isImageRun) continue;
            size_t n = r.content.size();
            size_t rlo = acc, rhi = acc + n;
            acc = rhi;
            if (rhi <= lo_c || rlo >= hi_c || r.content.empty()) continue;
            any = true;
            if (!(r.style.*flag)) all_set = false;
        }
    };

    bool any = false, all_set = true;
    for (int i = il; i <= ih; ++i) {
        size_t p0, c0, p1, c1;
        if (!range_for(i, leaves[i], p0, c0, p1, c1)) continue;
        for (size_t pi = p0; pi <= p1 && pi < leaves[i]->m_doc.paragraphs.size(); ++pi) {
            size_t a = pi == p0 ? c0 : 0;
            size_t b = pi == p1 ? c1 : leaves[i]->m_doc.paragraphs[pi]->byte_length();
            touch_run(leaves[i]->m_doc.paragraphs[pi].get(), a, b, any, all_set);
        }
    }
    bool value = any ? !all_set : true;
    for (int i = il; i <= ih; ++i) {
        HtmlText *ht = leaves[i];
        size_t p0, c0, p1, c1;
        if (!range_for(i, ht, p0, c0, p1, c1)) continue;
        for (size_t pi = p0; pi <= p1 && pi < ht->m_doc.paragraphs.size(); ++pi) {
            Paragraph *p = ht->m_doc.paragraphs[pi].get();
            size_t a = pi == p0 ? c0 : 0;
            size_t b = pi == p1 ? c1 : p->byte_length();
            if (a > b) continue;
            size_t ri_lo = p->split_run_at(a);
            size_t ri_hi = p->split_run_at(b);
            for (size_t ri = ri_lo; ri < ri_hi && ri < p->runs.size(); ++ri) {
                if (p->runs[ri].isImageRun) continue;
                p->runs[ri].style.*flag = value;
            }
        }
        ht->m_doc.markLayoutDirty();
        ht->m_measured_w = -1;
        ht->m_natural_w = -1;
    }
    note_edit();
    request_reflow();
    if (auto *f = dynamic_cast<HtmlText *>(m_focus_leaf))
        f->scroll_caret();
    if (Screen *s = screen()) s->redraw();
}

bool HtmlDocument::keyboard_event(int key, int, int action, int mods) {
    if ((mods & SYSTEM_COMMAND_MOD) == 0) return false;
    if (action != GLFW_PRESS &&
        !(action == GLFW_REPEAT && key == GLFW_KEY_V))
        return false;
    switch (key) {
    case GLFW_KEY_A:
        select_all_text();
        return true;
    case GLFW_KEY_C:
        copy_selection();
        return true;
    case GLFW_KEY_X:
        if (m_editable) {
            copy_selection();
            delete_selection();
        }
        return true;
    case GLFW_KEY_V:
        if (m_editable) paste_clipboard();
        return true;
    default:
        return false;
    }
}

void HtmlDocument::set_html(const std::string &html) {
    /* Kept until the first keystroke so an unedited draft round-trips. */
    m_source_html = html;
    m_edit_dirty = false;
    clear();

    if (!html_is_parseable(html)) {
        set_plain("This message could not be displayed: its formatting is "
                  "too large or too deeply nested to render safely.");
        return;
    }

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
    if (!out || !out->root) { relayout(); return; }
    std::string css;
    collect_style_text(out->root, css);
    parse_stylesheet(css, B.sheet);
    Style base;
    base.fontSize = 17.0f;
    base.fgColor  = m_text;
    elem_memo_clear(); // memo scoped to this set_html build
    build_children(this, &out->root->v.document.children, base, B, 0,
                   TextAlignment::Left);
    elem_memo_clear();
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
            if (!hb->m_link_url.empty())
                out += "  href=\"" + hb->m_link_url + "\"";
        }
        if (auto *ht = dynamic_cast<const HtmlText *>(w)) {
            std::string t;
            for (const auto &p : ht->m_doc.paragraphs) {
                if (p->isRule) { t += "<hr>"; continue; }
                if (p->isImage) { t += "<img>"; continue; }
                if (p->isBullet) t += "* ";
                t += p->plain_text();
                for (const Text &r : p->runs) {
                    char rs[200];
                    std::snprintf(rs, sizeof(rs),
                        " [fg=%.2f,%.2f,%.2f,%.2f bg=%.2f,%.2f,%.2f,%.2f %s%s%s%s%s sz=%.1f pad=%.0f,%.0f brd=%.1f]",
                        r.style.fgColor.r, r.style.fgColor.g,
                        r.style.fgColor.b, r.style.fgColor.a,
                        r.style.bgColor.r, r.style.bgColor.g,
                        r.style.bgColor.b, r.style.bgColor.a,
                        r.style.bold ? "b" : "", r.style.italic ? "i" : "",
                        r.style.underline ? "u" : "",
                        r.style.monospace ? "m" : "",
                        !r.linkUrl.empty() ? "L" : "",
                        r.style.fontSize, r.style.padX, r.style.padY,
                        r.style.borderWidth);
                    if (r.style.superscript) t += " sup";
                    t += rs;
                    if (!r.linkUrl.empty())
                        t += " href=\"" + r.linkUrl + "\"";
                }
                t += " | ";
            }
            if (t.size() > 2000) t = t.substr(0, 2000) + "...";
            out += "  \"" + t + "\"";
        }
        out += "\n";
        for (const Widget *c : w->children()) walk(c, d + 1);
    };
    walk(this, 0);
    return out;
}

void HtmlDocument::request_reflow() {
    // Coalesce: if already pending, don't spam redraw
    if (m_reflow_pending) return;
    // Oscillating content (see kReflowBudgetMax comment): stop asking for
    // more relayouts once the budget for this width/content is spent.
    if (m_reflow_budget <= 0) return;
    m_reflow_pending = true;
    if (Screen *s = screen())
        s->redraw();
}

void HtmlDocument::draw(NVGcontext *ctx) {
    if (nvgIsRecordingDisplayList(ctx))
        return;
    trace_draw("HtmlDocument", this);
    if (m_size.x() != m_reflow_budget_w) {
        // A real width change (resize/zoom) — give self-correction a
        // fresh budget to settle into the new width.
        m_reflow_budget_w = m_size.x();
        m_reflow_budget = kReflowBudgetMax;
    }
    if (m_reflow_pending) {
#ifdef DEBUG
        if (debug_draw()) fprintf(stderr, "[trace] HtmlDocument reflow\n");
#endif
        /* A leaf's measured height changed during the last paint.  Re-run
         * the ScrollPanel's layout BEFORE painting our subtree so every
         * widget below paints with consistent geometry (never relayout
         * mid-paint). */
        m_reflow_pending = false;
        --m_reflow_budget;
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
