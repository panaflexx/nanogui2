/*
 * pagemade/vector.cpp — the vector loader, the store, and placement.
 * The SVG decoder itself is plugin/svg.cpp; this file only calls it.
 */
#include "vector.h"

#include "composer/font.h"
#include "image.h"

#include <hb.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

namespace pagemade {

namespace {

float clamp01(float a) {
    if (a < 0.f)
        return 0.f;
    if (a > 1.f)
        return 1.f;
    return a;
}

} // namespace

void VectorLoader::add(std::unique_ptr<VectorDecoder> decoder) {
    if (decoder)
        m_decoders.push_back(std::move(decoder));
}

const VectorDecoder *VectorLoader::match(const uint8_t *data, size_t n) const {
    for (const auto &d : m_decoders)
        if (d->sniff(data, n))
            return d.get();
    return nullptr;
}

bool VectorLoader::decode(const uint8_t *data, size_t n, VectorScene *out, std::string *error) const {
    const VectorDecoder *d = match(data, n);
    if (!d) {
        if (error)
            *error = "no vector decoder accepted this file";
        return false;
    }
    return d->decode(data, n, out, error);
}

VectorLoader &vector_loader() {
    static VectorLoader loader;
    static bool ready = (register_svg_decoder(loader), true);
    (void) ready;
    return loader;
}

std::vector<std::pair<std::string, std::string>> vector_file_types() {
    return {{"svg", "SVG"}};
}

std::string vector_place_note(const std::string &name, const std::vector<std::string> &skipped) {
    std::string s = "placed " + (name.empty() ? std::string("graphic") : name);
    if (skipped.empty())
        return s;
    s += " (";
    for (size_t i = 0; i < skipped.size(); ++i) {
        if (i)
            s += ", ";
        s += skipped[i];
    }
    s += " skipped)";
    return s;
}

VectorStore::VectorStore(VectorStore &&o) noexcept
    : m_entries(std::move(o.m_entries)), m_next_id(o.m_next_id) {
    o.m_next_id = 0;
}

VectorStore &VectorStore::operator=(VectorStore &&o) noexcept {
    if (this != &o) {
        m_entries = std::move(o.m_entries);
        m_next_id = o.m_next_id;
        o.m_next_id = 0;
    }
    return *this;
}

VectorStore VectorStore::snapshot() const {
    VectorStore out;
    out.m_entries = m_entries;
    out.m_next_id = m_next_id;
    return out;
}

void VectorStore::realize(const FontLibrary &fonts) {
    for (auto &kv : m_entries)
        realize_vector_text(kv.second.scene, fonts);
}

void VectorStore::clear() {
    m_entries.clear();
    m_next_id = 0;
}

uint32_t VectorStore::take_id() { return ++m_next_id; }

void VectorStore::note_id(uint32_t id) {
    if (id > m_next_id)
        m_next_id = id;
}

bool VectorStore::add(uint32_t id, std::vector<uint8_t> bytes, VectorScene scene) {
    if (id == 0)
        return false;
    Entry e;
    e.source = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    e.scene = std::move(scene);
    note_id(id);
    m_entries[id] = std::move(e);
    return true;
}

bool VectorStore::add(uint32_t id, std::vector<uint8_t> bytes, std::string *error) {
    if (id == 0) {
        if (error)
            *error = "graphic id is missing";
        return false;
    }
    VectorScene scene;
    if (!vector_loader().decode(bytes.data(), bytes.size(), &scene, error))
        return false;
    return add(id, std::move(bytes), std::move(scene));
}

void VectorStore::set_path(uint32_t id, const std::string &path) {
    auto it = m_entries.find(id);
    if (it != m_entries.end())
        it->second.path = path;
}

const VectorStore::Entry *VectorStore::find(uint32_t id) const {
    auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : &it->second;
}

VectorStore::Bytes VectorStore::source_of(uint32_t id) const {
    const Entry *e = find(id);
    return e ? e->source : Bytes();
}

const VectorScene *VectorStore::scene(uint32_t id) const {
    const Entry *e = find(id);
    return e && e->loaded() ? &e->scene : nullptr;
}

void vector_image_view(const VectorImage &im, float &x, float &y, float &w, float &h) {
    x = im.x;
    y = im.y;
    w = im.w;
    h = im.h;
    if (im.px_w <= 0 || im.px_h <= 0 || !(im.w > 0.f) || !(im.h > 0.f))
        return;                          // size unknown: the viewport
    std::string p;
    for (char c : im.preserve)
        if (!std::isspace((unsigned char) c) || (!p.empty() && p.back() != ' '))
            p += std::isspace((unsigned char) c) ? ' ' : (char) std::tolower((unsigned char) c);
    if (p.rfind("defer", 0) == 0)
        p = p.size() > 6 ? p.substr(6) : std::string();
    if (p.rfind("none", 0) == 0)
        return;                          // stretched to the viewport
    float ax = 0.5f, ay = 0.5f;          // xMidYMid
    if (p.size() >= 8 && p[0] == 'x' && p[4] == 'y') {
        const std::string hx = p.substr(1, 3), hy = p.substr(5, 3);
        ax = hx == "min" ? 0.f : hx == "max" ? 1.f : 0.5f;
        ay = hy == "min" ? 0.f : hy == "max" ? 1.f : 0.5f;
    }
    const bool slice = p.find("slice") != std::string::npos;
    const float kx = im.w / (float) im.px_w, ky = im.h / (float) im.px_h;
    const float k = slice ? std::max(kx, ky) : std::min(kx, ky);
    w = (float) im.px_w * k;
    h = (float) im.px_h * k;
    x = im.x + (im.w - w) * ax;
    y = im.y + (im.h - h) * ay;
}

void vector_frame(const PageSetup &s, size_t page, float gw, float gh, const Point *at,
                  float &x, float &y, float &w, float &h, bool to_column) {
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    const int n = std::max(1, s.columns);
    const float live = std::max(1.f, s.width - left - right);
    const float gutter = n > 1 ? std::min(s.gutter, live / (float) (n - 1) * 0.5f) : 0.f;
    const float col_w = std::max(1.f, (live - gutter * (float) (n - 1)) / (float) n);
    int col = 0;
    if (at) {
        float best = 1e30f;
        for (int k = 0; k < n; ++k) {
            const float mid = left + (float) k * (col_w + gutter) + col_w * 0.5f;
            const float d = std::fabs(at->x - mid);
            if (d < best) {
                best = d;
                col = k;
            }
        }
    }
    const float col_x = left + (float) col * (col_w + gutter);
    w = gw > 0.f ? gw : 36.f;
    h = gh > 0.f ? gh : 36.f;
    const float stated_w = w;
    /* A paste shrinks only past the page's edge (a drawing padded for its
     * strokes may be a little wider than the live area) */
    const float fit_w = to_column ? col_w : std::max(live, s.width - (at ? std::max(0.f, at->x) : left));
    if (w > fit_w && w > 0.f) {
        h *= fit_w / w;
        w = fit_w;
    }
    const float live_h = to_column ? std::max(36.f, s.height - s.margin_top - s.margin_bottom)
                                   : std::max(36.f, s.height - s.margin_top);
    if (h > live_h && h > 0.f) {
        w *= live_h / h;
        h = live_h;
    }
    const bool fitted = w < stated_w - 0.01f;
    x = !fitted ? (at ? at->x : left) : to_column ? col_x + (col_w - w) * 0.5f : left + (live - w) * 0.5f;
    const float y0 = at ? at->y : s.margin_top;
    const float lowest = s.height - s.margin_bottom - h;
    y = std::max(s.margin_top, std::min(y0, std::max(s.margin_top, lowest)));
}

namespace {

const Font *face_for_run(const FontLibrary &fonts, const VectorTextRun &run);
float measure_text(const FontLibrary *fonts, const VectorTextRun &style, const std::string &text);
std::string compact_family(const std::string &s);
std::string resolve_family(const FontLibrary *fonts, const VectorTextRun &run);

std::string svg_num(double v) {
    if (!std::isfinite(v))
        v = 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

std::string svg_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default:
            if (c >= 0x20 || c == '\t' || c == '\n')
                out += (char) c;
            break;
        }
    }
    return out;
}

std::string svg_hex(const Color &c) {
    auto byte = [](float v) { return (int) std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

std::string svg_matrix(const Transform &t) {
    return svg_num(t.a) + " " + svg_num(t.b) + " " + svg_num(t.c) + " " + svg_num(t.d) + " " +
           svg_num(t.e) + " " + svg_num(t.f);
}

std::string svg_path(const Path &p) {
    std::string d;
    for (const PathCmd &c : p) {
        switch (c.kind) {
        case PathCmd::Move: d += "M" + svg_num(c.x) + " " + svg_num(c.y); break;
        case PathCmd::Line: d += "L" + svg_num(c.x) + " " + svg_num(c.y); break;
        case PathCmd::Quad:
            d += "Q" + svg_num(c.cx1) + " " + svg_num(c.cy1) + " " + svg_num(c.x) + " " + svg_num(c.y);
            break;
        case PathCmd::Cubic:
            d += "C" + svg_num(c.cx1) + " " + svg_num(c.cy1) + " " + svg_num(c.cx2) + " " +
                 svg_num(c.cy2) + " " + svg_num(c.x) + " " + svg_num(c.y);
            break;
        case PathCmd::Close: d += "Z"; break;
        }
    }
    return d;
}

/* A slice of a decoded graphic, written so a later open draws that slice.
 * Paths and clips are already in the graphic's box. Text keeps its own
 * transform, which is how the decoder stored it. */
std::string scene_svg(const VectorScene &scene) {
    struct Out {
        std::string defs, body;
        int n = 0;
        std::string id(const char *p) { return std::string(p) + std::to_string(++n); }
    } o;
    auto gradient = [&](const VectorPaint &p) {
        const bool radial = p.kind == PaintKind::Radial;
        const std::string id = o.id(radial ? "r" : "l");
        std::string g = radial ? "<radialGradient id=\"" + id +
                                    "\" gradientUnits=\"userSpaceOnUse\" cx=\"0\" cy=\"0\" r=\"1\" fx=\"" +
                                    svg_num(p.focal.x) + "\" fy=\"" + svg_num(p.focal.y) + "\""
                              : "<linearGradient id=\"" + id +
                                    "\" gradientUnits=\"userSpaceOnUse\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\"";
        if (!(p.xf == Transform()))
            g += " gradientTransform=\"matrix(" + svg_matrix(p.xf) + ")\"";
        if (p.spread == Spread::Reflect)
            g += " spreadMethod=\"reflect\"";
        else if (p.spread == Spread::Repeat)
            g += " spreadMethod=\"repeat\"";
        g += ">";
        for (const GradientStop &s : p.stops) {
            g += "<stop offset=\"" + svg_num(s.offset) + "\" stop-color=\"" + svg_hex(s.color) + "\"";
            if (s.color.a < 0.999f)
                g += " stop-opacity=\"" + svg_num(s.color.a) + "\"";
            g += "/>";
        }
        g += radial ? "</radialGradient>" : "</linearGradient>";
        o.defs += g;
        return "url(#" + id + ")";
    };
    auto paint_attr = [&](const char *which, const VectorPaint &p) {
        if (p.none())
            return std::string(" ") + which + "=\"none\"";
        if (p.gradient() && p.stops.size() >= 2)
            return std::string(" ") + which + "=\"" + gradient(p) + "\"";
        std::string a = std::string(" ") + which + "=\"" + svg_hex(p.color) + "\"";
        if (p.color.a < 0.999f)
            a += std::string(" ") + which + "-opacity=\"" + svg_num(p.color.a) + "\"";
        return a;
    };
    auto open_clip = [&](int clip) {
        if (clip < 0 || (size_t) clip >= scene.clips.size())
            return 0;
        int opened = 0;
        const ClipLayers &layers = scene.clips[(size_t) clip];
        for (int i = (int) layers.size() - 1; i >= 0; --i) {
            const std::string id = o.id("c");
            o.defs += "<clipPath id=\"" + id + "\">";
            if (layers[(size_t) i].empty())
                o.defs += "<rect width=\"0\" height=\"0\"/>";
            for (const ClipPath &cp : layers[(size_t) i]) {
                o.defs += "<path d=\"" + svg_path(cp.path) + "\"";
                if (cp.rule == FillRule::EvenOdd)
                    o.defs += " clip-rule=\"evenodd\"";
                o.defs += "/>";
            }
            o.defs += "</clipPath>";
            o.body += "<g clip-path=\"url(#" + id + ")\">";
            ++opened;
        }
        return opened;
    };
    auto close_clip = [&](int n) {
        while (n-- > 0)
            o.body += "</g>";
    };
    auto emit_shape = [&](const VectorShape &sh) {
        if (sh.path.empty() || (sh.fill.none() && sh.stroke.none()))
            return;
        const int n = open_clip(sh.clip);
        o.body += "<path d=\"" + svg_path(sh.path) + "\"";
        if (sh.rule == FillRule::EvenOdd)
            o.body += " fill-rule=\"evenodd\"";
        o.body += paint_attr("fill", sh.fill);
        o.body += paint_attr("stroke", sh.stroke.paint);
        if (!sh.stroke.none()) {
            o.body += " stroke-width=\"" + svg_num(sh.stroke.width) + "\"";
            if (sh.stroke.cap == LineCap::Round)
                o.body += " stroke-linecap=\"round\"";
            else if (sh.stroke.cap == LineCap::Square)
                o.body += " stroke-linecap=\"square\"";
            if (sh.stroke.join == LineJoin::Round)
                o.body += " stroke-linejoin=\"round\"";
            else if (sh.stroke.join == LineJoin::Bevel)
                o.body += " stroke-linejoin=\"bevel\"";
            if (sh.stroke.miter_limit != 4.f)
                o.body += " stroke-miterlimit=\"" + svg_num(sh.stroke.miter_limit) + "\"";
            if (!sh.stroke.dash.empty()) {
                o.body += " stroke-dasharray=\"";
                for (size_t i = 0; i < sh.stroke.dash.size(); ++i) {
                    if (i)
                        o.body += " ";
                    o.body += svg_num(sh.stroke.dash[i]);
                }
                o.body += "\"";
                if (sh.stroke.dash_offset != 0.f)
                    o.body += " stroke-dashoffset=\"" + svg_num(sh.stroke.dash_offset) + "\"";
            }
        }
        o.body += "/>";
        close_clip(n);
    };
    auto emit_text = [&](const VectorText &tx) {
        if (tx.runs.empty())
            return;
        int n = open_clip(tx.clip);
        if (!(tx.xf == Transform())) {
            o.body += "<g transform=\"matrix(" + svg_matrix(tx.xf) + ")\">";
            ++n;
        }
        o.body += "<text xml:space=\"preserve\">";
        for (const VectorTextRun &run : tx.runs) {
            o.body += "<tspan";
            if (run.has_x)
                o.body += " x=\"" + svg_num(run.x) + "\"";
            if (run.has_y)
                o.body += " y=\"" + svg_num(run.y) + "\"";
            if (run.dx != 0.f)
                o.body += " dx=\"" + svg_num(run.dx) + "\"";
            if (run.dy != 0.f)
                o.body += " dy=\"" + svg_num(run.dy) + "\"";
            if (!run.families.empty()) {
                o.body += " font-family=\"";
                for (size_t i = 0; i < run.families.size(); ++i) {
                    if (i)
                        o.body += ", ";
                    /* Double quotes (&quot;) survive a family named with an apostrophe */
                    o.body += "&quot;" + svg_escape(run.families[i]) + "&quot;";
                }
                o.body += "\"";
            }
            o.body += " font-size=\"" + svg_num(run.size) + "\"";
            if (run.weight != 400)
                o.body += " font-weight=\"" + std::to_string(run.weight) + "\"";
            if (run.italic)
                o.body += " font-style=\"italic\"";
            if (run.small_caps)
                o.body += " font-variant=\"small-caps\"";
            if (run.letter_spacing != 0.f)
                o.body += " letter-spacing=\"" + svg_num(run.letter_spacing) + "\"";
            if (run.underline || run.strike) {
                o.body += " text-decoration=\"";
                if (run.underline)
                    o.body += "underline";
                if (run.underline && run.strike)
                    o.body += " ";
                if (run.strike)
                    o.body += "line-through";
                o.body += "\"";
            }
            o.body += " fill=\"" + svg_hex(run.fill) + "\"";
            if (run.fill.a < 0.999f)
                o.body += " fill-opacity=\"" + svg_num(run.fill.a) + "\"";
            o.body += ">" + svg_escape(run.text) + "</tspan>";
        }
        o.body += "</text>";
        close_clip(n);
    };
    /* A picture that stays in the drawing (turned, or not a picture this
     * slice can place on its own) keeps its data URI */
    auto emit_image = [&](const VectorImage &im) {
        if (im.href.empty() || !(im.w > 0.f) || !(im.h > 0.f))
            return;
        int n = open_clip(im.clip);
        o.body += "<image x=\"" + svg_num(im.x) + "\" y=\"" + svg_num(im.y) + "\" width=\"" +
                  svg_num(im.w) + "\" height=\"" + svg_num(im.h) + "\"";
        if (!(im.xf == Transform()))
            o.body += " transform=\"matrix(" + svg_matrix(im.xf) + ")\"";
        if (!im.preserve.empty())
            o.body += " preserveAspectRatio=\"" + svg_escape(im.preserve) + "\"";
        if (im.opacity < 0.999f)
            o.body += " opacity=\"" + svg_num(im.opacity) + "\"";
        o.body += " xlink:href=\"" + svg_escape(im.href) + "\"/>";
        close_clip(n);
    };
    if (scene.order.empty()) {
        for (const VectorShape &sh : scene.shapes)
            emit_shape(sh);
        for (const VectorText &tx : scene.texts)
            emit_text(tx);
        for (const VectorImage &im : scene.images)
            emit_image(im);
    } else {
        for (const VectorSlotRef &ref : scene.order) {
            if (ref.index < 0)
                continue;
            if (ref.slot == VectorSlot::Shape && (size_t) ref.index < scene.shapes.size())
                emit_shape(scene.shapes[(size_t) ref.index]);
            else if (ref.slot == VectorSlot::Text && (size_t) ref.index < scene.texts.size())
                emit_text(scene.texts[(size_t) ref.index]);
            else if (ref.slot == VectorSlot::Image && (size_t) ref.index < scene.images.size())
                emit_image(scene.images[(size_t) ref.index]);
        }
    }
    const std::string w = svg_num(scene.width), h = svg_num(scene.height);
    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                      "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" + w + "pt\" height=\"" + h +
                      "pt\" viewBox=\"0 0 " + w + " " + h + "\" preserveAspectRatio=\"none\">";
    if (!o.defs.empty())
        svg += "<defs>" + o.defs + "</defs>";
    svg += o.body + "</svg>";
    return svg;
}

bool cut_rect(float &x, float &y, float &w, float &h, float x2, float y2, float w2, float h2) {
    const float x1 = std::max(x, x2), y1 = std::max(y, y2);
    const float xh = std::min(x + w, x2 + w2), yh = std::min(y + h, y2 + h2);
    if (!(xh > x1) || !(yh > y1))
        return false;
    x = x1;
    y = y1;
    w = xh - x1;
    h = yh - y1;
    return true;
}

/* A user-space rectangle in the graphic's box, when it stays upright and
 * axis-aligned there. */
bool upright_rect(const Transform &xf, float x, float y, float w, float h, float &bx, float &by,
                  float &bw, float &bh) {
    const Point p0 = xf.apply({x, y});
    const Point p1 = xf.apply({x + w, y});
    const Point p2 = xf.apply({x + w, y + h});
    const Point p3 = xf.apply({x, y + h});
    const float eps = 0.05f;
    if (std::fabs(p0.y - p1.y) > eps || std::fabs(p2.y - p3.y) > eps || std::fabs(p0.x - p3.x) > eps ||
        std::fabs(p1.x - p2.x) > eps)
        return false;
    if (!(p1.x > p0.x + eps) || !(p3.y > p0.y + eps))
        return false;
    bx = p0.x;
    by = p0.y;
    bw = p1.x - p0.x;
    bh = p3.y - p0.y;
    return true;
}

/* The picture in the graphic's box, when it is upright and axis-aligned:
 * where its pixels go (i), and the part of that its viewport shows (v; a
 * "slice" overflows the viewport). A rotation or a flip stays in a graphic. */
bool image_box(const VectorImage &im, float &ix, float &iy, float &iw, float &ih, float &vx,
               float &vy, float &vw, float &vh) {
    if (!(im.w > 0.f) || !(im.h > 0.f))
        return false;
    float dx, dy, dw, dh;
    vector_image_view(im, dx, dy, dw, dh);
    if (!upright_rect(im.xf, dx, dy, dw, dh, ix, iy, iw, ih) ||
        !upright_rect(im.xf, im.x, im.y, im.w, im.h, vx, vy, vw, vh))
        return false;
    return cut_rect(vx, vy, vw, vh, ix, iy, iw, ih);
}

bool clip_box(const ClipLayers &layers, float &x, float &y, float &w, float &h) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool any = false;
    auto grow = [&](float px, float py) {
        any = true;
        x0 = std::min(x0, px);
        y0 = std::min(y0, py);
        x1 = std::max(x1, px);
        y1 = std::max(y1, py);
    };
    for (const auto &layer : layers)
        for (const ClipPath &cp : layer)
            for (const PathCmd &c : cp.path) {
                if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic)
                    grow(c.cx1, c.cy1);
                if (c.kind == PathCmd::Cubic)
                    grow(c.cx2, c.cy2);
                if (c.kind != PathCmd::Close)
                    grow(c.x, c.y);
            }
    if (!any || !(x1 > x0) || !(y1 > y0))
        return false;
    x = x0;
    y = y0;
    w = x1 - x0;
    h = y1 - y0;
    return true;
}

std::string file_stem(std::string name) {
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0)
        name = name.substr(0, dot);
    return name.empty() ? std::string("graphic") : name;
}

VectorScene slice_shell(const VectorScene &src) {
    VectorScene s;
    s.width = src.width;
    s.height = src.height;
    s.clips = src.clips;
    s.skipped = src.skipped;
    return s;
}

/* The whole graphic as one item. `bytes` are the file that was decoded. */
VectorPlacement place_whole(PageDoc &doc, VectorStore &store, std::vector<uint8_t> bytes,
                            VectorScene scene, const std::string &name, const std::string &source,
                            size_t page, const Point *at, bool paste) {
    VectorPlacement r;
    const uint32_t id = store.take_id();
    const std::string leaf = name.empty() ? std::string("graphic.svg") : name;
    if (!store.add(id, std::move(bytes), scene)) {
        r.error = "couldn't keep the SVG";
        return r;
    }
    VectorAsset asset;
    asset.id = id;
    asset.name = leaf;
    asset.source = source;
    asset.width = scene.width;
    asset.height = scene.height;
    asset.skipped = scene.skipped;
    asset.source_size = store.source_of(id) ? store.source_of(id)->size() : 0;
    asset.hash = 0;
    if (VectorStore::Bytes kept = store.source_of(id))
        asset.hash = image_bytes_hash(kept->data(), kept->size());
    doc.add_vector(asset);
    float x, y, w, h;
    vector_frame(doc.setup, page, scene.width, scene.height, at, x, y, w, h, !paste);
    Item it;
    it.w = w;
    it.h = h;
    it.xf = Transform::translate(x, y);
    PlacedVector pv;
    pv.asset = id;
    pv.w = w;
    pv.h = h;
    it.content = pv;
    r.item = doc.add_item(page, std::move(it));
    r.items = {r.item};
    r.asset = id;
    r.note = vector_place_note(leaf, scene.skipped);
    r.ok = true;
    return r;
}

/* Pictures become picture items. Shapes and text on either side of one stay
 * a graphic, in the same paint order, and the pieces are one group. False
 * leaves the document alone: the caller places the file as one graphic. */
bool place_grouped(PageDoc &doc, VectorStore &store, ImageStore &images, const VectorScene &scene,
                   const std::string &name, size_t page, const Point *at, bool paste,
                   VectorPlacement &r) {
    struct Pic {
        int index = -1;
        float ix = 0, iy = 0, iw = 0, ih = 0;   // the whole picture, graphic box
        float vx = 0, vy = 0, vw = 0, vh = 0;   // the part the frame shows
        ImageMetadata meta;
    };
    std::vector<char> pull(scene.images.size(), 0);
    std::vector<Pic> pics;
    int embedded = 0;
    for (size_t i = 0; i < scene.images.size(); ++i) {
        const VectorImage &im = scene.images[i];
        if (!im.bytes || im.bytes->empty())
            continue;
        ++embedded;
        float ix, iy, iw, ih, vx, vy, vw, vh;
        if (!image_box(im, ix, iy, iw, ih, vx, vy, vw, vh))
            return false;
        ImageMetadata meta;
        std::string err;
        if (!image_loader().probe(im.bytes->data(), im.bytes->size(), &meta, &err))
            return false;
        if (im.clip >= 0 && (size_t) im.clip < scene.clips.size()) {
            float cx, cy, cw, ch;
            if (!clip_box(scene.clips[(size_t) im.clip], cx, cy, cw, ch) ||
                !cut_rect(vx, vy, vw, vh, cx, cy, cw, ch))
                continue;                // clipped away: nothing to place
        }
        if (!cut_rect(vx, vy, vw, vh, 0.f, 0.f, scene.width, scene.height))
            continue;
        pull[i] = 1;
        Pic p;
        p.index = (int) i;
        p.ix = ix;
        p.iy = iy;
        p.iw = iw;
        p.ih = ih;
        p.vx = vx;
        p.vy = vy;
        p.vw = vw;
        p.vh = vh;
        p.meta = meta;
        pics.push_back(p);
    }
    if (embedded == 0 || pics.empty() || (int) pics.size() != embedded)
        return false;

    std::vector<VectorSlotRef> order = scene.order;
    if (order.empty()) {
        for (size_t i = 0; i < scene.shapes.size(); ++i)
            order.push_back({VectorSlot::Shape, (int) i});
        for (size_t i = 0; i < scene.texts.size(); ++i)
            order.push_back({VectorSlot::Text, (int) i});
        for (size_t i = 0; i < scene.images.size(); ++i)
            order.push_back({VectorSlot::Image, (int) i});
    }
    struct Part {
        bool picture = false;
        Pic pic;
        VectorScene scene;
    };
    std::vector<Part> parts;
    Part cur;
    cur.scene = slice_shell(scene);
    bool used = false;
    auto flush = [&] {
        if (!used)
            return;
        parts.push_back(std::move(cur));
        cur = Part();
        cur.scene = slice_shell(scene);
        used = false;
    };
    for (const VectorSlotRef &ref : order) {
        if (ref.slot == VectorSlot::Image) {
            if (ref.index < 0 || (size_t) ref.index >= pull.size() || !pull[(size_t) ref.index])
                continue;
            flush();
            Part p;
            p.picture = true;
            for (const Pic &pic : pics)
                if (pic.index == ref.index)
                    p.pic = pic;
            parts.push_back(std::move(p));
            continue;
        }
        if (ref.slot == VectorSlot::Shape) {
            if (ref.index < 0 || (size_t) ref.index >= scene.shapes.size())
                continue;
            cur.scene.shapes.push_back(scene.shapes[(size_t) ref.index]);
            cur.scene.order.push_back({VectorSlot::Shape, (int) cur.scene.shapes.size() - 1});
            used = true;
        } else if (ref.slot == VectorSlot::Text) {
            if (ref.index < 0 || (size_t) ref.index >= scene.texts.size())
                continue;
            cur.scene.texts.push_back(scene.texts[(size_t) ref.index]);
            cur.scene.order.push_back({VectorSlot::Text, (int) cur.scene.texts.size() - 1});
            used = true;
        }
    }
    flush();
    bool any_pic = false;
    for (const Part &p : parts)
        any_pic = any_pic || p.picture;
    /* A fully clipped picture was dropped. With nothing left to separate,
     * the whole file stays one graphic and keeps that clip. */
    if (!any_pic)
        return false;

    float fx, fy, fw, fh;
    vector_frame(doc.setup, page, scene.width, scene.height, at, fx, fy, fw, fh, !paste);
    const float sx = fw / scene.width, sy = fh / scene.height;
    const std::string stem = file_stem(name);
    int vectors = 0, pictures = 0;
    for (const Part &p : parts) {
        vectors += p.picture ? 0 : 1;
        pictures += p.picture ? 1 : 0;
    }
    int vector_n = 0, picture_n = 0;
    for (const Part &p : parts) {
        if (p.picture) {
            const VectorImage &im = scene.images[(size_t) p.pic.index];
            const uint32_t id = images.take_id();
            std::string err;
            if (!images.add(id, std::vector<uint8_t>(*im.bytes), nullptr, &err)) {
                r.error = err.empty() ? "couldn't keep the picture" : err;
                return true;             // the page may hold earlier pieces; the caller keeps it
            }
            const std::string ext = p.pic.meta.format == "jpeg" ? ".jpg"
                                    : p.pic.meta.format == "png" ? ".png"
                                    : p.pic.meta.format == "tiff" ? ".tif"
                                                                  : ".img";
            ImageAsset asset;
            asset.id = id;
            asset.name = stem + (pictures > 1 ? "-" + std::to_string(++picture_n) : "") + ext;
            asset.meta = p.pic.meta;
            asset.source_size = im.bytes->size();
            asset.hash = image_bytes_hash(im.bytes->data(), im.bytes->size());
            doc.add_image(asset);
            Item it;
            it.w = p.pic.vw * sx;
            it.h = p.pic.vh * sy;
            it.xf = Transform::translate(fx + p.pic.vx * sx, fy + p.pic.vy * sy);
            PlacedImage pi;
            pi.asset = id;
            pi.x = (p.pic.ix - p.pic.vx) * sx;
            pi.y = (p.pic.iy - p.pic.vy) * sy;
            pi.w = p.pic.iw * sx;
            pi.h = p.pic.ih * sy;
            pi.opacity = std::clamp(im.opacity, 0.f, 1.f) * 100.f;
            it.content = pi;
            r.items.push_back(doc.add_item(page, std::move(it)));
            continue;
        }
        const std::string svg = scene_svg(p.scene);
        std::vector<uint8_t> bytes(svg.begin(), svg.end());
        const uint32_t id = store.take_id();
        VectorAsset asset;
        asset.id = id;
        asset.name = vectors > 1 ? stem + "-" + std::to_string(++vector_n) + ".svg" : stem + ".svg";
        asset.width = p.scene.width;
        asset.height = p.scene.height;
        asset.skipped = scene.skipped;
        asset.source_size = bytes.size();
        asset.hash = image_bytes_hash(bytes.data(), bytes.size());
        if (!store.add(id, std::move(bytes), p.scene)) {
            r.error = "couldn't keep the SVG";
            return true;
        }
        doc.add_vector(asset);
        if (r.asset == 0)
            r.asset = id;
        Item it;
        it.w = fw;
        it.h = fh;
        it.xf = Transform::translate(fx, fy);
        PlacedVector pv;
        pv.asset = id;
        pv.w = fw;
        pv.h = fh;
        it.content = pv;
        r.items.push_back(doc.add_item(page, std::move(it)));
    }
    if (r.items.size() >= 2)
        r.group = doc.group_items(r.items);
    r.item = r.items.empty() ? 0 : r.items.front();
    r.note = vector_place_note(name.empty() ? std::string("graphic.svg") : name, scene.skipped);
    r.ok = r.item != 0;
    if (!r.ok && r.error.empty())
        r.error = "couldn't place the SVG";
    return true;
}

void shift_path(Path &p, float dx, float dy) {
    for (PathCmd &c : p) {
        if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic) {
            c.cx1 += dx;
            c.cy1 += dy;
        }
        if (c.kind == PathCmd::Cubic) {
            c.cx2 += dx;
            c.cy2 += dy;
        }
        if (c.kind != PathCmd::Close) {
            c.x += dx;
            c.y += dy;
        }
    }
}

/* The type of one run at page scale `k` (the text's transform taken out of
 * the frame and into the sizes), in `family`. */
CharStyle text_style(const VectorTextRun &run, float k, const std::string &family) {
    CharStyle st;
    st.family = family;
    st.bold = run.weight >= 600;
    st.italic = run.italic;
    st.caps = run.small_caps ? Caps::Small : Caps::Normal;
    st.underline = run.underline;
    st.strike = run.strike;
    const float size = run.size > 0.f ? run.size : 12.f;
    st.size = size * k;
    st.tracking = run.letter_spacing / size * 1000.f;   // a ratio: the scale cancels
    st.color = run.fill;
    st.swatch = 0;
    return st;
}

bool same_linear(const Transform &a, const Transform &b) {
    const float tol = 1e-3f * std::max(1.f, std::fabs(a.a) + std::fabs(a.b) + std::fabs(a.c) + std::fabs(a.d));
    return std::fabs(a.a - b.a) <= tol && std::fabs(a.b - b.b) <= tol && std::fabs(a.c - b.c) <= tol &&
           std::fabs(a.d - b.d) <= tol;
}

/* One SVG text element, read for Paste as Text */
struct PasteLine {
    Transform M;                         // its text space -> the page
    float k = 1.f;                       // M's uniform scale
    float left = 0, right = 0, baseline = 0, size = 0;   // text space
    bool justified = false;              // its words were placed one by one
    std::vector<Run> runs;               // the type, at page scale
    std::string text;
    float qx = 0, qy = 0, qr = 0;        // left, baseline, right in the block's space
    /* A jump in the pen, or a run of only leader dots, is a tab: its stop,
     * at page scale from the line's left (or from the right, for text that
     * ends the line) */
    std::vector<TabStop> tabs;
};

/* A run that is only leader characters (". . . .", "____") */
bool leader_run(const std::string &t, std::string &leader) {
    size_t n = 0;
    std::string first;
    for (size_t i = 0; i < t.size();) {
        const unsigned char c = (unsigned char) t[i];
        if (c == ' ') {
            ++i;
            continue;
        }
        std::string ch;
        if (c == '.' || c == '_' || c == '-')
            ch = t.substr(i, 1);
        else if (c == 0xC2 && i + 1 < t.size() && (unsigned char) t[i + 1] == 0xB7)
            ch = t.substr(i, 2);         // middle dot
        else if (c == 0xE2 && i + 2 < t.size() && (unsigned char) t[i + 1] == 0x80 &&
                 (unsigned char) t[i + 2] == 0xA6)
            ch = t.substr(i, 3);         // ellipsis
        else
            return false;
        if (first.empty())
            first = ch;
        i += ch.size();
        ++n;
    }
    if (n < 3)
        return false;
    leader = first;
    return true;
}

/* Editable text frames, a picture for each embedded image, and the paths
 * in a graphic fitted to themselves. Consecutive lines that share a left
 * edge, a line spacing and their type become one frame, its paragraphs
 * broken where a justified line falls short or the next line is indented.
 * Each frame is composed and widened until nothing in it is overset, and
 * set so its first baseline lands where the SVG put it. False leaves the
 * document alone. */
bool place_as_text(PageDoc &doc, VectorStore &store, ImageStore *images, const FontLibrary *fonts,
                   const VectorScene &scene, const std::string &name, size_t page, const Point *at,
                   bool paste, VectorPlacement &r) {
    if (scene.texts.empty() && scene.shapes.empty() && scene.images.empty())
        return false;
    struct Pic {
        float ix = 0, iy = 0, iw = 0, ih = 0;
        float vx = 0, vy = 0, vw = 0, vh = 0;
        ImageMetadata meta;
    };
    std::vector<Pic> pics(scene.images.size());
    std::vector<char> pull(scene.images.size(), 0);
    if (images) {
        for (size_t i = 0; i < scene.images.size(); ++i) {
            const VectorImage &im = scene.images[i];
            if (!im.bytes || im.bytes->empty())
                continue;
            float ix, iy, iw, ih, vx, vy, vw, vh;
            if (!image_box(im, ix, iy, iw, ih, vx, vy, vw, vh))
                continue;                // turned or flipped: it stays in the drawing
            ImageMetadata meta;
            std::string err;
            if (!image_loader().probe(im.bytes->data(), im.bytes->size(), &meta, &err))
                continue;                // not a picture pagemade reads: in the drawing
            if (im.clip >= 0 && (size_t) im.clip < scene.clips.size()) {
                float cx, cy, cw, ch;
                if (!clip_box(scene.clips[(size_t) im.clip], cx, cy, cw, ch) ||
                    !cut_rect(vx, vy, vw, vh, cx, cy, cw, ch))
                    continue;
            }
            if (!cut_rect(vx, vy, vw, vh, 0.f, 0.f, scene.width, scene.height))
                continue;
            pics[i] = {ix, iy, iw, ih, vx, vy, vw, vh, meta};
            pull[i] = 1;
        }
    }

    float fx, fy, fw, fh;
    vector_frame(doc.setup, page, scene.width, scene.height, at, fx, fy, fw, fh, !paste);
    const float sx = fw / scene.width, sy = fh / scene.height;
    const Transform page_map{sx, 0, 0, sy, fx, fy};
    const std::string stem = file_stem(name);
    int drawing_n = 0;

    /* ---- The drawing: shapes, and pictures that can't stand alone ---- */
    std::vector<VectorSlotRef> drawing;
    auto flush_drawing = [&] {
        if (drawing.empty())
            return;
        Bounds box;
        float pad = 0.5f;
        for (const VectorSlotRef &ref : drawing) {
            if (ref.slot == VectorSlot::Shape) {
                const VectorShape &sh = scene.shapes[(size_t) ref.index];
                box.add(path_bounds(sh.path));
                if (!sh.stroke.none())
                    pad = std::max(pad, sh.stroke.width);
            } else {
                const VectorImage &im = scene.images[(size_t) ref.index];
                for (Point p : {Point{im.x, im.y}, Point{im.x + im.w, im.y}, Point{im.x + im.w, im.y + im.h},
                                Point{im.x, im.y + im.h}})
                    box.add(Bounds{im.xf.apply(p).x, im.xf.apply(p).y, im.xf.apply(p).x, im.xf.apply(p).y});
            }
        }
        if (box.empty() || !(box.x1 > box.x0) || !(box.y1 > box.y0)) {
            drawing.clear();
            return;
        }
        const float bx = box.x0 - pad, by = box.y0 - pad;
        const float bw = box.x1 - box.x0 + pad * 2.f, bh = box.y1 - box.y0 + pad * 2.f;
        const Transform shift = Transform::translate(-bx, -by);
        VectorScene slice;
        slice.width = bw;
        slice.height = bh;
        std::map<int, int> clips;
        auto clip_into = [&](int c) {
            if (c < 0 || (size_t) c >= scene.clips.size())
                return -1;
            auto have = clips.find(c);
            if (have != clips.end())
                return have->second;
            ClipLayers layers = scene.clips[(size_t) c];
            for (auto &layer : layers)
                for (ClipPath &cp : layer)
                    shift_path(cp.path, -bx, -by);
            clips[c] = (int) slice.clips.size();
            slice.clips.push_back(std::move(layers));
            return clips[c];
        };
        for (const VectorSlotRef &ref : drawing) {
            if (ref.slot == VectorSlot::Shape) {
                VectorShape sh = scene.shapes[(size_t) ref.index];
                shift_path(sh.path, -bx, -by);
                if (sh.fill.gradient())
                    sh.fill.xf = shift * sh.fill.xf;
                if (sh.stroke.paint.gradient())
                    sh.stroke.paint.xf = shift * sh.stroke.paint.xf;
                sh.clip = clip_into(sh.clip);
                sh.windings.reset();
                slice.order.push_back({VectorSlot::Shape, (int) slice.shapes.size()});
                slice.shapes.push_back(std::move(sh));
            } else {
                VectorImage im = scene.images[(size_t) ref.index];
                im.xf = shift * im.xf;
                im.clip = clip_into(im.clip);
                slice.order.push_back({VectorSlot::Image, (int) slice.images.size()});
                slice.images.push_back(std::move(im));
            }
        }
        drawing.clear();
        const std::string svg = scene_svg(slice);
        std::vector<uint8_t> bytes(svg.begin(), svg.end());
        const uint32_t id = store.take_id();
        VectorAsset asset;
        asset.id = id;
        asset.name = stem + "-drawing" + (drawing_n ? "-" + std::to_string(drawing_n + 1) : "") + ".svg";
        ++drawing_n;
        asset.width = bw;
        asset.height = bh;
        asset.source_size = bytes.size();
        asset.hash = image_bytes_hash(bytes.data(), bytes.size());
        if (!store.add(id, std::move(bytes), std::move(slice))) {
            r.error = "couldn't keep the drawing";
            return;
        }
        doc.add_vector(asset);
        if (r.asset == 0)
            r.asset = id;
        Item it;
        it.w = bw * sx;
        it.h = bh * sy;
        it.xf = Transform::translate(fx + bx * sx, fy + by * sy);
        PlacedVector pv;
        pv.asset = id;
        pv.w = it.w;
        pv.h = it.h;
        it.content = pv;
        r.items.push_back(doc.add_item(page, std::move(it)));
    };

    /* ---- Text ---- */
    auto read_line = [&](const VectorText &tx, PasteLine &L) {
        L.M = page_map * tx.xf;
        L.k = std::sqrt(std::fabs(L.M.a * L.M.d - L.M.b * L.M.c));
        if (!(L.k > 1e-6f))
            return false;
        /* Runs in reading order: when every run is placed, by position
         * (a writer may put leader dots after the number they lead to) */
        std::vector<const VectorTextRun *> runs;
        bool all_x = true;
        for (const VectorTextRun &run : tx.runs)
            if (!run.text.empty()) {
                runs.push_back(&run);
                all_x = all_x && run.has_x;
            }
        if (all_x)
            std::stable_sort(runs.begin(), runs.end(), [](const VectorTextRun *a, const VectorTextRun *b) {
                return a->x < b->x;
            });
        float pen = 0.f;
        bool have_left = false, have_y = false, tab = false;
        std::string leader;
        int placed = 0;
        for (const VectorTextRun *rp : runs) {
            const VectorTextRun &run = *rp;
            const float before = pen;
            if (run.has_x) {
                pen = run.x;
                ++placed;
            }
            pen += run.dx;
            if (!have_left || pen < L.left) {
                L.left = pen;
                have_left = true;
            }
            if (run.has_y && !have_y) {
                L.baseline = run.y + run.dy;
                have_y = true;
            }
            const float w = measure_text(fonts, run, run.text);
            L.right = std::max(L.right, pen + w);
            L.size = std::max(L.size, run.size);
            /* A jump of more than an em and a half after text is a tab */
            if (!L.text.empty() && L.text.back() != ' ' && pen - before > run.size * 1.5f)
                tab = true;
            std::string dots;
            if (leader_run(run.text, dots)) {
                tab = true;              // the dots become the tab's leader
                leader = dots;
                pen += w;
                continue;
            }
            const float start = pen;
            pen += w;
            if (run.fill.a <= 0.f)
                continue;                // fill="none": nothing to see, nothing to edit
            std::string text = run.text;
            if (tab && !L.text.empty()) {
                TabStop stop;
                stop.leader = leader;
                stop.pos = (start - L.left) * L.k;
                L.tabs.push_back(stop);
                text = "\t" + text;
                tab = false;
                leader.clear();
            }
            const CharStyle st = text_style(run, L.k, resolve_family(fonts, run));
            if (!L.runs.empty() && L.runs.back().style == st)
                L.runs.back().text += text;
            else
                L.runs.push_back(Run{st, text});
            L.text += text;
        }
        /* A tab whose text ends the line sits against the right edge: a
         * right stop measured from the right, as a contents line has */
        if (!L.tabs.empty()) {
            const size_t last_tab = L.text.rfind('\t');
            const std::string after = last_tab == std::string::npos ? std::string() : L.text.substr(last_tab + 1);
            if (!after.empty() && after.size() <= 8) {
                TabStop &stop = L.tabs.back();
                stop.align = TabAlign::Right;
                stop.from_right = true;
                stop.pos = 0.f;
            }
        }
        L.justified = placed > 1 && L.tabs.empty();
        return !L.runs.empty() && L.size > 0.f;
    };

    const StyleDef *normal = doc.find_style("Normal");
    std::vector<PasteLine> block;        // lines of one frame, in the first line's space
    Transform block_space, block_inv;
    float block_left = 0, block_gap = 0;
    auto flush_block = [&] {
        if (block.empty())
            return;
        const PasteLine &first = block.front();
        bool justified = false;
        float right = first.qr;
        for (const PasteLine &L : block) {
            justified = justified || L.justified;
            right = std::max(right, L.qr);
        }
        const float measure = right - block_left;
        /* Paragraphs: one ends where the next line is indented, or (justified
         * text) where a line falls short of the measure */
        Story story;
        Paragraph para;
        auto start_para = [&] {
            para = Paragraph();
            if (normal)
                para.style = normal->para;
            para.style.name = normal ? normal->name : std::string();
            para.style.align = justified ? Align::Justify : Align::Left;
            para.style.hyphenate = false;
            para.style.first_indent = para.style.left_indent = para.style.right_indent = 0.f;
            para.style.space_before = para.style.space_after = 0.f;
        };
        start_para();
        for (size_t i = 0; i < block.size(); ++i) {
            const PasteLine &L = block[i];
            if (!L.tabs.empty() && !para.runs.empty()) {
                story.paragraphs.push_back(std::move(para));   // a tabbed line stands alone
                start_para();
            }
            if (!L.tabs.empty()) {
                para.style.tabs = L.tabs;
                para.style.align = Align::Left;
            }
            if (para.runs.empty() && L.qx > block_left + 1.f)
                para.style.first_indent = L.qx - block_left;
            std::vector<Run> runs = L.runs;
            if (block.size() > 1)
                for (Run &run : runs)
                    run.style.leading = block_gap;     // the SVG's own line spacing
            if (!para.runs.empty()) {
                /* Join with the line before: a break hyphen goes, else a space */
                std::string &prev = para.runs.back().text;
                const bool hyphen = !prev.empty() && prev.back() == '-' && !L.text.empty() &&
                                    std::islower((unsigned char) L.text[0]);
                if (hyphen)
                    prev.pop_back();
                else if (!prev.empty() && prev.back() != ' ')
                    prev += ' ';
            }
            for (Run &run : runs) {
                if (!para.runs.empty() && para.runs.back().style == run.style)
                    para.runs.back().text += run.text;
                else
                    para.runs.push_back(std::move(run));
            }
            const bool last = i + 1 == block.size();
            const bool next_indented = !last && block[i + 1].qx > block_left + 1.f;
            const bool short_line = justified && (L.qr - L.qx) < (right - L.qx) * 0.95f;
            const bool next_tabbed = !last && !block[i + 1].tabs.empty();
            if (last || next_indented || short_line || !L.tabs.empty() || next_tabbed) {
                while (!para.runs.empty() && !para.runs.back().text.empty() &&
                       para.runs.back().text.back() == ' ')
                    para.runs.back().text.pop_back();
                story.paragraphs.push_back(std::move(para));
                start_para();
            }
        }
        /* The frame: as wide as the lines (a little more for ragged text),
         * widened until the composer sets it all; as deep as what it set */
        float w = std::max(4.f, measure + (justified ? 0.5f : 2.f));
        float h = 0.f, baseline0 = 0.f;
        if (fonts) {
            Composition c;
            for (int tries = 0; tries < 16; ++tries) {
                c = compose(story, {Frame{0.f, 0.f, w, 100000.f}}, *fonts);
                if (!c.overset && c.lines.size() <= block.size())
                    break;
                w = w * 1.02f + 1.f;
            }
            if (!c.lines.empty()) {
                const ComposedLine &l0 = c.lines.front(), &ln = c.lines.back();
                h = ln.top + ln.leading - l0.top + 0.5f;
                baseline0 = l0.baseline;
            }
        }
        if (!(h > 0.f)) {
            const float lead = block.size() > 1 ? block_gap : first.size * first.k * 1.2f;
            h = lead * (float) block.size() + 0.5f;
            baseline0 = lead * (2.f / 3.f);
        }
        const Transform xf = block_space * Transform::translate(block_left, first.qy - baseline0);
        const StoryId sid = doc.add_story(std::move(story));
        r.items.push_back(doc.add_text_frame(page, sid, w, h, xf));
        block.clear();
    };
    auto add_line = [&](PasteLine L) {
        if (!block.empty()) {
            const PasteLine &first = block.front();
            const PasteLine &prev = block.back();
            bool joins = same_linear(L.M, first.M) &&
                         std::fabs(L.size * L.k - first.size * first.k) <= 0.5f &&
                         !L.runs.empty() && !first.runs.empty() &&
                         L.runs.front().style.family == first.runs.front().style.family;
            if (joins) {
                const Point a = block_inv.apply(L.M.apply({L.left, L.baseline}));
                const Point b = block_inv.apply(L.M.apply({L.right, L.baseline}));
                L.qx = a.x;
                L.qy = a.y;
                L.qr = b.x;
                const float size = L.size * L.k;
                const float gap = L.qy - prev.qy;
                joins = gap > size * 0.9f && gap < size * 2.2f &&
                        (block.size() < 2 || std::fabs(gap - block_gap) <= block_gap * 0.12f);
                /* The same left edge; or a justified paragraph whose first
                 * line (indented) began the block */
                const bool same_left = std::fabs(L.qx - block_left) <= 1.f;
                const bool under_indent = block.size() == 1 && (L.justified || prev.justified) &&
                                          L.qx < block_left && block_left - L.qx <= size * 4.f;
                const bool indented = (L.justified || prev.justified) && L.qx > block_left &&
                                      L.qx - block_left <= size * 4.f;
                joins = joins && (same_left || under_indent || indented);
                if (joins) {
                    if (block.size() == 1)
                        block_gap = gap;
                    block_left = std::min(block_left, L.qx);
                    block.push_back(std::move(L));
                    return;
                }
            }
            flush_block();
        }
        /* The first line sets the block's space: its own, the scale taken out */
        block_space = L.M * Transform::scale(1.f / L.k, 1.f / L.k);
        block_inv = block_space.inverse();
        const Point a = block_inv.apply(L.M.apply({L.left, L.baseline}));
        const Point b = block_inv.apply(L.M.apply({L.right, L.baseline}));
        L.qx = a.x;
        L.qy = a.y;
        L.qr = b.x;
        block_left = L.qx;
        block_gap = 0.f;
        block.push_back(std::move(L));
    };

    /* ---- Pictures ---- */
    auto add_picture = [&](int index) {
        const Pic &p = pics[(size_t) index];
        const VectorImage &im = scene.images[(size_t) index];
        const uint32_t id = images->take_id();
        std::string err;
        if (!images->add(id, std::vector<uint8_t>(*im.bytes), nullptr, &err)) {
            r.error = err.empty() ? "couldn't keep the picture" : err;
            return;
        }
        const std::string ext = p.meta.format == "jpeg" ? ".jpg"
                                : p.meta.format == "png" ? ".png"
                                : p.meta.format == "tiff" ? ".tif"
                                                          : ".img";
        ImageAsset asset;
        asset.id = id;
        asset.name = stem + ext;
        asset.meta = p.meta;
        asset.source_size = im.bytes->size();
        asset.hash = image_bytes_hash(im.bytes->data(), im.bytes->size());
        doc.add_image(asset);
        Item it;
        it.w = p.vw * sx;
        it.h = p.vh * sy;
        it.xf = Transform::translate(fx + p.vx * sx, fy + p.vy * sy);
        PlacedImage pi;
        pi.asset = id;
        pi.x = (p.ix - p.vx) * sx;
        pi.y = (p.iy - p.vy) * sy;
        pi.w = p.iw * sx;
        pi.h = p.ih * sy;
        pi.opacity = std::clamp(im.opacity, 0.f, 1.f) * 100.f;
        it.content = pi;
        r.items.push_back(doc.add_item(page, std::move(it)));
    };

    std::vector<VectorSlotRef> order = scene.order;
    if (order.empty()) {
        for (size_t i = 0; i < scene.shapes.size(); ++i)
            order.push_back({VectorSlot::Shape, (int) i});
        for (size_t i = 0; i < scene.texts.size(); ++i)
            order.push_back({VectorSlot::Text, (int) i});
        for (size_t i = 0; i < scene.images.size(); ++i)
            order.push_back({VectorSlot::Image, (int) i});
    }
    /* Paint order holds: a run of text lines becomes frames before the
     * next drawing or picture, and a drawing ends at the next text or
     * picture that stands alone */
    for (const VectorSlotRef &ref : order) {
        if (ref.index < 0)
            continue;
        const size_t i = (size_t) ref.index;
        if (ref.slot == VectorSlot::Text) {
            if (i >= scene.texts.size())
                continue;
            flush_drawing();
            PasteLine L;
            if (read_line(scene.texts[i], L))
                add_line(std::move(L));
        } else if (ref.slot == VectorSlot::Shape) {
            if (i >= scene.shapes.size())
                continue;
            flush_block();
            drawing.push_back(ref);
        } else if (i < scene.images.size()) {
            flush_block();
            if (pull[i]) {
                flush_drawing();
                add_picture(ref.index);
            } else if (scene.images[i].bytes && !scene.images[i].bytes->empty()) {
                drawing.push_back(ref);  // turned, or unreadable here: drawn as it was
            }
        }
        if (!r.error.empty())
            return true;
    }
    flush_block();
    flush_drawing();
    if (!r.error.empty())
        return true;
    if (r.items.empty())
        return false;
    if (r.items.size() >= 2)
        r.group = doc.group_items(r.items);
    r.item = r.items.front();
    r.note = "placed " + (name.empty() ? std::string("graphic") : name) + " as text";
    r.ok = true;
    return true;
}

} // namespace

VectorPlacement place_svg_bytes(PageDoc &doc, VectorStore &store, std::vector<uint8_t> bytes,
                                const std::string &name, const std::string &source,
                                size_t page, const Point *at, ImageStore *images, SvgPaste how,
                                const FontLibrary *fonts, bool paste) {
    VectorPlacement r;
    if (page >= doc.pages.size()) {
        r.error = "no such page";
        return r;
    }
    if (bytes.empty()) {
        r.error = "the SVG is empty";
        return r;
    }
    VectorScene scene;
    const bool read = paste ? decode_svg_paste(bytes.data(), bytes.size(), &scene, &r.error)
                            : vector_loader().decode(bytes.data(), bytes.size(), &scene, &r.error);
    if (!read) {
        if (r.error.empty())
            r.error = "couldn't read the SVG";
        return r;
    }
    if (!(scene.width > 0.f) || !(scene.height > 0.f)) {
        r.error = "the SVG has no size";
        return r;
    }
    if (how == SvgPaste::Text &&
        place_as_text(doc, store, images, fonts, scene, name, page, at, paste, r))
        return r;
    if (images && place_grouped(doc, store, *images, scene, name, page, at, paste, r))
        return r;
    return place_whole(doc, store, std::move(bytes), std::move(scene), name, source, page, at, paste);
}

namespace {

std::string compact_family(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '-' || c == '_' || c == '\'' || c == '"')
            continue;
        o.push_back((char) std::tolower((unsigned char) c));
    }
    return o;
}

/* The family a run's text names here: the first of its families the
 * library has, else the pagemade family a well-known name stands for
 * (P052 for Serif, and so on). Paste as Text names this family in its
 * frames and measures with it, so the frame widths and the composer agree. */
std::string resolve_family(const FontLibrary *fonts, const VectorTextRun &run) {
    for (const std::string &fam : run.families) {
        if (fonts && fonts->has_family(fam))
            return fam;
        const std::string key = compact_family(fam);
        const char *alias = nullptr;
        if (key == "p052" || key == "p052roman" || key == "serif")
            alias = "Serif";
        else if (key == "nimbussans" || key == "nimbussansregular" || key == "sansserif" ||
                 key == "sans" || key == "monospace" || key == "mono")
            alias = "Sans";
        else if (key == "urwgothic" || key == "urwgothicbook" || key == "gothic" || key == "display")
            alias = "Display";
        if (alias && (!fonts || fonts->has_family(alias)))
            return alias;
    }
    return "Serif";
}

const Font *face_for_run(const FontLibrary &fonts, const VectorTextRun &run) {
    const std::string family = resolve_family(&fonts, run);
    if (fonts.has_family(family))
        if (const Font *f = fonts.match(family, run.weight, run.italic, ""))
            return f;
    return fonts.find("Serif", run.weight >= 600, run.italic);
}

float measure_text(const FontLibrary *fonts, const VectorTextRun &style, const std::string &text) {
    if (text.empty() || !(style.size > 0.f))
        return 0.f;
    const float guess = (float) text.size() * style.size * 0.5f;
    if (!fonts)
        return guess;
    const Font *font = face_for_run(*fonts, style);
    if (!font || !font->hb())
        return guess;
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.data(), (int) text.size(), 0, (int) text.size());
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));
    hb_shape(font->hb(), buf, nullptr, 0);
    unsigned n = 0;
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &n);
    const float em = style.size / (float) std::max(1, font->units_per_em());
    float adv = 0.f;
    for (unsigned i = 0; i < n; ++i)
        adv += pos[i].x_advance * em + style.letter_spacing;
    hb_buffer_destroy(buf);
    return adv > 0.f ? adv : guess;
}

void add_design_outline(Path &dst, const GlyphOutline &o, float gx, float gy, float em,
                        const Transform &xf) {
    auto pt = [&](float x, float y) { return xf.apply({gx + x * em, gy - y * em}); };
    for (const PathCmd &c : o.cmds) {
        PathCmd ocmd = c;
        if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic) {
            const Point p = pt(c.cx1, c.cy1);
            ocmd.cx1 = p.x;
            ocmd.cy1 = p.y;
        }
        if (c.kind == PathCmd::Cubic) {
            const Point p = pt(c.cx2, c.cy2);
            ocmd.cx2 = p.x;
            ocmd.cy2 = p.y;
        }
        if (c.kind != PathCmd::Close) {
            const Point p = pt(c.x, c.y);
            ocmd.x = p.x;
            ocmd.y = p.y;
        }
        dst.push_back(ocmd);
    }
}

void add_user_rect(Path &dst, float x0, float y0, float x1, float y1, const Transform &xf) {
    const Point a = xf.apply({x0, y0}), b = xf.apply({x1, y0}), c = xf.apply({x1, y1}),
                d = xf.apply({x0, y1});
    dst.push_back({PathCmd::Move, 0, 0, 0, 0, a.x, a.y});
    dst.push_back({PathCmd::Line, 0, 0, 0, 0, b.x, b.y});
    dst.push_back({PathCmd::Line, 0, 0, 0, 0, c.x, c.y});
    dst.push_back({PathCmd::Line, 0, 0, 0, 0, d.x, d.y});
    dst.push_back({PathCmd::Close, 0, 0, 0, 0, 0, 0});
}

} // namespace

void realize_vector_text(VectorScene &scene, const FontLibrary &fonts) {
    bool any = false;
    for (const VectorText &tx : scene.texts)
        for (const VectorTextRun &run : tx.runs)
            any = any || !run.ink_set;
    if (!any)
        return;
    hb_buffer_t *buf = hb_buffer_create();
    for (VectorText &tx : scene.texts) {
        bool need = false;
        for (const VectorTextRun &run : tx.runs)
            need = need || !run.ink_set;
        if (!need)
            continue;
        float pen_x = 0.f, pen_y = 0.f;
        for (VectorTextRun &run : tx.runs) {
            if (run.has_x)
                pen_x = run.x;
            if (run.has_y)
                pen_y = run.y;
            pen_x += run.dx;
            pen_y += run.dy;
            run.ink.clear();
            run.windings.reset();
            const Font *font = face_for_run(fonts, run);
            if (!font || run.text.empty() || !(run.size > 0.f)) {
                run.ink_set = true;
                continue;
            }
            hb_buffer_clear_contents(buf);
            hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
            hb_buffer_add_utf8(buf, run.text.data(), (int) run.text.size(), 0, (int) run.text.size());
            hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
            hb_buffer_set_language(buf, hb_language_from_string("en", -1));
            hb_buffer_guess_segment_properties(buf);
            hb_feature_t feat;
            unsigned nfeat = 0;
            if (run.small_caps && font->has_small_caps()) {
                feat = {HB_TAG('s', 'm', 'c', 'p'), 1, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
                nfeat = 1;
            }
            hb_shape(font->hb(), buf, nfeat ? &feat : nullptr, nfeat);
            unsigned n = 0;
            hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
            hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &n);
            const float em = run.size / (float) font->units_per_em();
            const float x0 = pen_x;
            const float base = pen_y;
            for (unsigned i = 0; i < n; ++i) {
                const float gx = pen_x + pos[i].x_offset * em;
                const float gy = pen_y - pos[i].y_offset * em;
                if (info[i].codepoint)
                    add_design_outline(run.ink, font->outline(info[i].codepoint), gx, gy, em, tx.xf);
                pen_x += pos[i].x_advance * em + run.letter_spacing;
                pen_y -= pos[i].y_advance * em;
            }
            const float x1 = n ? pen_x - run.letter_spacing : pen_x;
            if (run.underline) {
                const float th = std::max(0.25f, font->underline_thickness() * em);
                const float y = base - font->underline_position() * em;
                add_user_rect(run.ink, x0, y, x1, y + th, tx.xf);
            }
            if (run.strike) {
                const float th = std::max(0.25f, font->strike_thickness() * em);
                const float y = base - font->strike_position() * em;
                add_user_rect(run.ink, x0, y - th * 0.5f, x1, y + th * 0.5f, tx.xf);
            }
            if (!run.ink.empty())
                run.windings = std::make_shared<const std::vector<uint8_t>>(fill_windings(run.ink));
            run.ink_set = true;
        }
    }
    hb_buffer_destroy(buf);
}

void append_placed_vector(DrawList &out, const PageDoc &, const Item &it, const VectorScene &scene) {
    const PlacedVector *pv = it.vector();
    if (!pv || !(scene.width > 0.f) || !(scene.height > 0.f))
        return;
    const float sx = pv->w / scene.width;
    const float sy = pv->h / scene.height;
    const float ss = std::sqrt(std::fabs(sx * sy));
    const float opacity = std::clamp(pv->opacity, 0.f, 100.f) / 100.f;
    auto map_pt = [&](float x, float y) { return Point{pv->x + x * sx, pv->y + y * sy}; };
    auto map_path = [&](const Path &from) {
        Path path;
        path.reserve(from.size());
        for (PathCmd c : from) {
            if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic) {
                const Point c1 = map_pt(c.cx1, c.cy1);
                c.cx1 = c1.x;
                c.cy1 = c1.y;
            }
            if (c.kind == PathCmd::Cubic) {
                const Point c2 = map_pt(c.cx2, c.cy2);
                c.cx2 = c2.x;
                c.cy2 = c2.y;
            }
            if (c.kind != PathCmd::Close) {
                const Point p = map_pt(c.x, c.y);
                c.x = p.x;
                c.y = p.y;
            }
            path.push_back(c);
        }
        return path;
    };
    /* Each clip is mapped once, when a shape first names it. */
    std::vector<DrawClip> clips(scene.clips.size());
    auto clip_of = [&](int i) -> DrawClip {
        if (i < 0 || (size_t) i >= clips.size())
            return nullptr;
        if (!clips[(size_t) i]) {
            auto layers = std::make_shared<ClipLayers>();
            for (const auto &layer : scene.clips[(size_t) i]) {
                layers->emplace_back();
                for (const ClipPath &cp : layer)
                    layers->back().push_back({map_path(cp.path), cp.rule});
            }
            clips[(size_t) i] = std::move(layers);
        }
        return clips[(size_t) i];
    };
    const Transform box_to_item = Transform::translate(pv->x, pv->y) * Transform::scale(sx, sy);
    auto gradient_of = [&](const VectorPaint &p) -> DrawGradientRef {
        if (!p.gradient() || p.stops.size() < 2)
            return nullptr;
        auto g = std::make_shared<DrawGradient>();
        g->radial = p.kind == PaintKind::Radial;
        g->stops = p.stops;
        for (GradientStop &stop : g->stops)
            stop.color.a = clamp01(stop.color.a * opacity);
        g->xf = box_to_item * p.xf;
        g->focal = p.focal;
        g->spread = p.spread;
        return g;
    };
    auto emit_shape = [&](const VectorShape &sh) {
        const Path path = map_path(sh.path);
        const DrawClip clip = clip_of(sh.clip);
        if (!sh.fill.none()) {
            DrawFill f;
            f.path = path;
            f.color = sh.fill.color;
            f.color.a = clamp01(f.color.a * opacity);
            f.xf = it.xf;
            f.rule = sh.rule;
            f.clip_w = it.w;
            f.clip_h = it.h;
            f.clip = clip;
            f.gradient = gradient_of(sh.fill);
            f.windings = sh.windings;
            out.push_back(std::move(f));
        }
        if (!sh.stroke.none()) {
            DrawStroke s;
            s.path = path;
            s.color = sh.stroke.paint.color;
            s.color.a = clamp01(s.color.a * opacity);
            s.width = sh.stroke.width * (ss > 1e-8f ? ss : 1.f);
            s.dash = sh.stroke.dash;
            for (float &d : s.dash)
                d *= ss;
            s.dash_offset = sh.stroke.dash_offset * ss;
            s.cap = sh.stroke.cap;
            s.join = sh.stroke.join;
            s.xf = it.xf;
            s.clip_w = it.w;
            s.clip_h = it.h;
            s.clip = clip;
            s.gradient = gradient_of(sh.stroke.paint);
            out.push_back(std::move(s));
        }
    };
    auto emit_text = [&](const VectorText &tx) {
        const DrawClip clip = clip_of(tx.clip);
        for (const VectorTextRun &run : tx.runs) {
            if (!run.ink_set || run.ink.empty() || run.fill.a <= 0.f)
                continue;
            DrawFill f;
            f.path = map_path(run.ink);
            f.color = run.fill;
            f.color.a = clamp01(f.color.a * opacity);
            if (f.color.a <= 0.f)
                continue;
            f.xf = it.xf;
            f.clip_w = it.w;
            f.clip_h = it.h;
            f.clip = clip;
            f.windings = run.windings;
            out.push_back(std::move(f));
        }
    };
    auto emit_image = [&](const VectorImage &im) {
        if (!im.bytes || im.bytes->empty() || !(im.w > 0.f) || !(im.h > 0.f))
            return;
        /* The picture's own space: its viewport's top-left at the origin and
         * its axes, so a turn, a skew or a flip draws as one. The frame and
         * the SVG's clips come along as clip paths in that space. */
        const Transform T = box_to_item * im.xf * Transform::translate(im.x, im.y);
        if (!(std::fabs(T.a * T.d - T.b * T.c) > 1e-12f))
            return;
        const Transform inv = T.inverse();
        auto into = [&](const Path &p) {
            Path q;
            q.reserve(p.size());
            for (PathCmd c : p) {
                if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic) {
                    const Point c1 = inv.apply({c.cx1, c.cy1});
                    c.cx1 = c1.x;
                    c.cy1 = c1.y;
                }
                if (c.kind == PathCmd::Cubic) {
                    const Point c2 = inv.apply({c.cx2, c.cy2});
                    c.cx2 = c2.x;
                    c.cy2 = c2.y;
                }
                if (c.kind != PathCmd::Close) {
                    const Point p2 = inv.apply({c.x, c.y});
                    c.x = p2.x;
                    c.y = p2.y;
                }
                q.push_back(c);
            }
            return q;
        };
        auto layers = std::make_shared<ClipLayers>();
        Path frame;
        frame.push_back({PathCmd::Move, 0, 0, 0, 0, 0.f, 0.f});
        frame.push_back({PathCmd::Line, 0, 0, 0, 0, it.w, 0.f});
        frame.push_back({PathCmd::Line, 0, 0, 0, 0, it.w, it.h});
        frame.push_back({PathCmd::Line, 0, 0, 0, 0, 0.f, it.h});
        frame.push_back({PathCmd::Close, 0, 0, 0, 0, 0, 0});
        layers->push_back({ClipPath{into(frame), FillRule::NonZero}});
        if (const DrawClip c = clip_of(im.clip))
            for (const auto &layer : *c) {
                layers->emplace_back();
                for (const ClipPath &cp : layer)
                    layers->back().push_back({into(cp.path), cp.rule});
            }
        float dx, dy, dw, dh;
        vector_image_view(im, dx, dy, dw, dh);   // preserveAspectRatio
        DrawImage d;
        d.x = dx - im.x;
        d.y = dy - im.y;
        d.w = dw;
        d.h = dh;
        d.clip_w = im.w;                 // the viewport: a "slice" is cut to it
        d.clip_h = im.h;
        d.opacity = clamp01(im.opacity * opacity);
        d.xf = it.xf * T;
        d.bytes = im.bytes;
        d.bytes_key = image_bytes_hash(im.bytes->data(), im.bytes->size());
        if (!d.bytes_key)
            d.bytes_key = 1;
        d.clip = std::move(layers);
        out.push_back(std::move(d));
    };
    auto emit_ref = [&](VectorSlotRef ref) {
        if (ref.slot == VectorSlot::Shape) {
            if (ref.index >= 0 && (size_t) ref.index < scene.shapes.size())
                emit_shape(scene.shapes[(size_t) ref.index]);
        } else if (ref.slot == VectorSlot::Text) {
            if (ref.index >= 0 && (size_t) ref.index < scene.texts.size())
                emit_text(scene.texts[(size_t) ref.index]);
        } else if (ref.slot == VectorSlot::Image) {
            if (ref.index >= 0 && (size_t) ref.index < scene.images.size())
                emit_image(scene.images[(size_t) ref.index]);
        }
    };
    if (scene.order.empty()) {
        for (const VectorShape &sh : scene.shapes)
            emit_shape(sh);
        for (const VectorText &tx : scene.texts)
            emit_text(tx);
        for (const VectorImage &im : scene.images)
            emit_image(im);
    } else {
        for (const VectorSlotRef &ref : scene.order)
            emit_ref(ref);
    }
}

} // namespace pagemade
