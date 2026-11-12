/*
 * pagemade/plugin/svg_place.cpp — see svg_place.h.
 *
 * Placing a decoded graphic: one frame, or pieces (pictures as picture
 * frames, the drawing around them as graphics), or Paste as Text's text
 * frames. Pieces are written back out as SVG slices (scene_svg) so a saved
 * publication reopens them through the same decoder.
 */
#include "plugin/svg_place.h"

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

void shift_path(Path &p, float dx, float dy);

float measure_text(const FontLibrary *fonts, const VectorTextRun &style, const std::string &text) {
    if (text.empty() || !(style.size > 0.f))
        return 0.f;
    const float guess = (float) text.size() * style.size * 0.5f;
    if (!fonts)
        return guess;
    const Font *font = vector_run_face(*fonts, style);
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
                            size_t page, const Point *at, bool paste, int pdf_page) {
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
    asset.page = pdf_page;
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
    r.note = vector_place_note(pdf_page > 0 ? leaf + " page " + std::to_string(pdf_page + 1) : leaf,
                               scene.skipped, scene.remarks);
    r.ok = true;
    return r;
}

/* Move a slice so its ink starts at the origin. The frame can then be
 * that ink, and a click on the drawing is not a click on the whole page. */
bool crop_to_ink(VectorScene &scene, float &bx, float &by, float &bw, float &bh,
                 const FontLibrary *fonts) {
    Bounds box;
    float pad = 0.5f;
    for (const VectorShape &sh : scene.shapes) {
        box.add(path_bounds(sh.path));
        if (!sh.stroke.none())
            pad = std::max(pad, sh.stroke.width);
    }
    for (const VectorText &tx : scene.texts) {
        float pen = 0.f;
        for (const VectorTextRun &run : tx.runs) {
            if (run.text.empty())
                continue;
            if (run.has_x)
                pen = run.x;
            pen += run.dx;
            const float y = (run.has_y ? run.y : 0.f) + run.dy;
            const float size = run.size > 0.f ? run.size : 12.f;
            const float w = std::max(measure_text(fonts, run, run.text), size * 0.25f);
            const float x0 = pen, x1 = pen + w;
            const float y0 = y - size * 0.85f, y1 = y + size * 0.3f;
            for (Point p : {Point{x0, y0}, Point{x1, y0}, Point{x0, y1}, Point{x1, y1}})
                box.add(tx.xf.apply(p));
            pen += w;
        }
    }
    if (box.empty() || !(box.x1 > box.x0) || !(box.y1 > box.y0))
        return false;
    bx = box.x0 - pad;
    by = box.y0 - pad;
    bw = box.x1 - box.x0 + pad * 2.f;
    bh = box.y1 - box.y0 + pad * 2.f;
    const Transform shift = Transform::translate(-bx, -by);
    std::vector<ClipLayers> kept;
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
        clips[c] = (int) kept.size();
        kept.push_back(std::move(layers));
        return clips[c];
    };
    for (VectorShape &sh : scene.shapes) {
        shift_path(sh.path, -bx, -by);
        if (sh.fill.gradient())
            sh.fill.xf = shift * sh.fill.xf;
        if (sh.stroke.paint.gradient())
            sh.stroke.paint.xf = shift * sh.stroke.paint.xf;
        sh.clip = clip_into(sh.clip);
        sh.windings.reset();
    }
    for (VectorText &tx : scene.texts) {
        tx.xf = shift * tx.xf;
        tx.clip = clip_into(tx.clip);
    }
    scene.clips = std::move(kept);
    scene.width = bw;
    scene.height = bh;
    return true;
}

/* Pictures become picture items. Runs of shapes, and runs of text, become
 * their own graphics fitted to their ink, so each can be grabbed. The
 * shapes are one group, the text is another, and both sit in the paste's
 * group with the pictures. False leaves the document alone: the caller
 * places the file as one graphic. */
bool place_grouped(PageDoc &doc, VectorStore &store, ImageStore &images, const VectorScene &scene,
                   const std::string &name, size_t page, const Point *at, bool paste,
                   const FontLibrary *fonts, VectorPlacement &r) {
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
        bool text = false;
        Pic pic;
        VectorScene scene;
    };
    std::vector<Part> parts;
    Part cur;
    cur.scene = slice_shell(scene);
    bool used = false;
    int run = 0;                         // 1 shapes, 2 text
    auto flush = [&] {
        if (!used)
            return;
        parts.push_back(std::move(cur));
        cur = Part();
        cur.scene = slice_shell(scene);
        used = false;
        run = 0;
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
            if (run == 2)
                flush();
            run = 1;
            cur.scene.shapes.push_back(scene.shapes[(size_t) ref.index]);
            cur.scene.order.push_back({VectorSlot::Shape, (int) cur.scene.shapes.size() - 1});
            used = true;
        } else if (ref.slot == VectorSlot::Text) {
            if (ref.index < 0 || (size_t) ref.index >= scene.texts.size())
                continue;
            if (run == 1)
                flush();
            run = 2;
            cur.text = true;
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
    std::vector<ItemId> shape_ids, text_ids;
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
        VectorScene slice = p.scene;
        float bx = 0, by = 0, bw = 0, bh = 0;
        if (!crop_to_ink(slice, bx, by, bw, bh, fonts))
            continue;
        const std::string svg = scene_svg(slice);
        std::vector<uint8_t> bytes(svg.begin(), svg.end());
        const uint32_t id = store.take_id();
        VectorAsset asset;
        asset.id = id;
        asset.name = vectors > 1 ? stem + "-" + std::to_string(++vector_n) + ".svg" : stem + ".svg";
        asset.width = bw;
        asset.height = bh;
        asset.skipped = scene.skipped;
        asset.source_size = bytes.size();
        asset.hash = image_bytes_hash(bytes.data(), bytes.size());
        if (!store.add(id, std::move(bytes), std::move(slice))) {
            r.error = "couldn't keep the SVG";
            return true;
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
        const ItemId placed_id = doc.add_item(page, std::move(it));
        r.items.push_back(placed_id);
        (p.text ? text_ids : shape_ids).push_back(placed_id);
    }
    /* The shapes are a group, the type another, and both go in the
     * paste's group with the pictures (in paint order, unmoved) */
    if (shape_ids.size() >= 2)
        doc.group_in_place(shape_ids);
    if (text_ids.size() >= 2)
        doc.group_in_place(text_ids);
    if (r.items.size() >= 2)
        r.group = doc.group_in_place(r.items);
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
    bool positioned = false;             // its runs are where a PDF put them (no justification implied)
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
/* A swatch for an imported color: Black or Paper when it is one, else an
 * existing swatch of that color, else a new one named for it */
SwatchId swatch_for(PageDoc &doc, const Color &c) {
    auto near = [](const Color &a, const Color &b) {
        return std::fabs(a.r - b.r) < 0.5f / 255.f && std::fabs(a.g - b.g) < 0.5f / 255.f &&
               std::fabs(a.b - b.b) < 0.5f / 255.f && std::fabs(a.a - b.a) < 0.5f / 255.f;
    };
    for (const Swatch &sw : doc.swatches)
        if (sw.id != kNoPaint && sw.id != kRegistration && near(sw.rgb, c))
            return sw.id;
    char name[64];
    std::snprintf(name, sizeof name, "R=%d G=%d B=%d", (int) std::lround(c.r * 255.f), (int) std::lround(c.g * 255.f),
                  (int) std::lround(c.b * 255.f));
    std::string n = name;
    if (c.a < 0.999f)
        n += " " + std::to_string((int) std::lround(c.a * 100.f)) + "%";
    return doc.add_swatch(n, c);
}

/* A drawn box or rule as an object of its own: an axis-aligned rectangle,
 * or a straight line, painted in plain colors and not clipped. Its place
 * and size are in the scene's space. False leaves it to the drawing. */
bool simple_shape(const VectorShape &sh, Shape::Kind &kind, float &x0, float &y0, float &x1, float &y1) {
    if (sh.clip >= 0 || sh.fill.gradient() || sh.stroke.paint.gradient() || !sh.stroke.dash.empty())
        return false;
    const bool filled = sh.fill.kind == PaintKind::Solid && sh.fill.color.a > 0.f;
    const bool stroked = !sh.stroke.none() && sh.stroke.paint.kind == PaintKind::Solid && sh.stroke.paint.color.a > 0.f;
    if (!filled && !stroked)
        return false;
    if (path_axis_rect(sh.path, x0, y0, x1, y1)) {
        kind = Shape::Kind::Rect;
        return true;
    }
    if (!filled && stroked && sh.path.size() == 2 && sh.path[0].kind == PathCmd::Move &&
        sh.path[1].kind == PathCmd::Line) {
        kind = Shape::Kind::Line;
        x0 = sh.path[0].x;
        y0 = sh.path[0].y;
        x1 = sh.path[1].x;
        y1 = sh.path[1].y;
        return std::hypot(x1 - x0, y1 - y0) > 1e-3f;
    }
    return false;
}

/* How Import takes a page apart: the pieces not grouped; the scene put
 * where `frame` says (its box at fx, fy, fw by fh, rather than fitted to a
 * column); and anything wholly outside `keep` (scene space: a print PDF's
 * crop marks, past its bleed) left out. */
struct PlaceApart {
    bool group = true;
    bool have_frame = false;
    float fx = 0, fy = 0, fw = 0, fh = 0;
    bool have_keep = false;
    float kx0 = 0, ky0 = 0, kx1 = 0, ky1 = 0;
};

bool place_as_text(PageDoc &doc, VectorStore &store, ImageStore *images, const FontLibrary *fonts,
                   const VectorScene &scene, const std::string &name, size_t page, const Point *at,
                   bool paste, VectorPlacement &r, const PlaceApart *apart = nullptr) {
    const bool group_pieces = !apart || apart->group;
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
    if (apart && apart->have_frame) {
        fx = apart->fx;
        fy = apart->fy;
        fw = apart->fw;
        fh = apart->fh;
    }
    const float sx = fw / scene.width, sy = fh / scene.height;
    const Transform page_map{sx, 0, 0, sy, fx, fy};
    /* Wholly outside the keep box (printer's marks): left out */
    auto kept = [&](const Bounds &b) {
        if (!apart || !apart->have_keep || b.empty())
            return true;
        return b.x1 >= apart->kx0 && b.x0 <= apart->kx1 && b.y1 >= apart->ky0 && b.y0 <= apart->ky1;
    };
    auto slot_bounds = [&](const VectorSlotRef &ref) {
        Bounds b;
        const size_t i = (size_t) ref.index;
        if (ref.slot == VectorSlot::Shape && i < scene.shapes.size()) {
            b = path_bounds(scene.shapes[i].path);
        } else if (ref.slot == VectorSlot::Text && i < scene.texts.size()) {
            const VectorText &tx = scene.texts[i];
            for (const VectorTextRun &run : tx.runs)
                if (run.ink_set && !run.ink.empty())
                    b.add(path_bounds(run.ink));
            if (b.empty()) {
                const Point p = tx.xf.apply({0.f, 0.f});
                b.add(Bounds{p.x, p.y, p.x, p.y});
            }
        } else if (ref.slot == VectorSlot::Image && i < scene.images.size()) {
            const VectorImage &im = scene.images[i];
            for (Point p : {Point{im.x, im.y}, Point{im.x + im.w, im.y + im.h}, Point{im.x + im.w, im.y},
                            Point{im.x, im.y + im.h}}) {
                const Point q = im.xf.apply(p);
                b.add(Bounds{q.x, q.y, q.x, q.y});
            }
        }
        return b;
    };
    const std::string stem = file_stem(name);
    int drawing_n = 0, picture_n = 0;

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
            } else if (ref.slot == VectorSlot::Text) {
                const VectorText &tx = scene.texts[(size_t) ref.index];
                float pen = 0.f;
                for (const VectorTextRun &run : tx.runs) {
                    if (run.text.empty())
                        continue;
                    if (run.has_x)
                        pen = run.x;
                    pen += run.dx;
                    const float y = (run.has_y ? run.y : 0.f) + run.dy;
                    const float size = run.size > 0.f ? run.size : 12.f;
                    const float w = std::max(measure_text(fonts, run, run.text), size * 0.25f);
                    const float x0 = pen, x1 = pen + w, y0 = y - size * 0.85f, y1 = y + size * 0.3f;
                    for (Point p : {Point{x0, y0}, Point{x1, y0}, Point{x0, y1}, Point{x1, y1}})
                        box.add(tx.xf.apply(p));
                    pen += w;
                }
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
            } else if (ref.slot == VectorSlot::Text) {
                VectorText tx = scene.texts[(size_t) ref.index];
                tx.xf = shift * tx.xf;
                tx.clip = clip_into(tx.clip);
                slice.order.push_back({VectorSlot::Text, (int) slice.texts.size()});
                slice.texts.push_back(std::move(tx));
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
            const CharStyle st = text_style(run, L.k, vector_run_family(fonts, run));
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
        L.positioned = tx.placed_runs;
        L.justified = placed > 1 && L.tabs.empty() && !tx.placed_runs;
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
        /* Lines placed where a PDF put them say nothing about how they were
         * set: three or more whose lines (but the last) mostly end at the
         * same right edge were justified. Their widths are measured in the
         * installed face, so "the same" is within a point and a half; the
         * frame is then as wide as that edge, not the widest line. */
        if (!justified && block.size() >= 3 && first.positioned) {
            std::vector<float> rights;
            for (size_t i = 0; i + 1 < block.size(); ++i)
                rights.push_back(block[i].qr);
            std::sort(rights.begin(), rights.end());
            const float edge = rights[rights.size() / 2];
            const float tol = std::max(1.5f, (edge - block_left) * 0.006f);
            size_t flush = 0;
            for (float r : rights)
                flush += std::fabs(r - edge) <= tol;
            justified = flush * 10 >= rights.size() * 7;
            if (justified)
                right = edge;
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
            para.style.leading_method = doc.setup.leading_method;   // the document's default
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
                /* Join with the line before. A hyphen stays, so "well-" and
                 * "known" stay "well-known"; a space separates other lines. */
                std::string &prev = para.runs.back().text;
                const bool hyphen = !prev.empty() && (prev.back() == '-' || prev.back() == '\xAD') &&
                                    !L.text.empty() && std::islower((unsigned char) L.text[0]);
                if (!hyphen && !prev.empty() && prev.back() != ' ')
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
                /* A first-line indent: in justified text, or in a PDF's
                 * placed lines, where justification isn't known yet */
                const bool prose = L.justified || prev.justified || L.positioned;
                const bool under_indent = block.size() == 1 && prose && L.qx < block_left &&
                                          block_left - L.qx <= size * 4.f;
                const bool indented = prose && L.qx > block_left && L.qx - block_left <= size * 4.f;
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
        int pictures = 0;
        for (char c : pull)
            pictures += c ? 1 : 0;
        ImageAsset asset;
        asset.id = id;
        asset.name = stem + (pictures > 1 ? "-" + std::to_string(++picture_n) : "") + ext;
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

    /* ---- Boxes and rules, as rectangle and line objects ---- */
    const float ss = std::sqrt(std::fabs(sx * sy));
    auto add_shape = [&](const VectorShape &vs, Shape::Kind kind, float x0, float y0, float x1, float y1) {
        Shape sh;
        sh.kind = kind;
        sh.fill = vs.fill.kind == PaintKind::Solid && vs.fill.color.a > 0.f && kind != Shape::Kind::Line
                      ? Paint{swatch_for(doc, vs.fill.color), 100.f}
                      : Paint{kNoPaint, 100.f};
        if (!vs.stroke.none() && vs.stroke.paint.kind == PaintKind::Solid && vs.stroke.paint.color.a > 0.f)
            sh.stroke = Stroke{{swatch_for(doc, vs.stroke.paint.color), 100.f}, vs.stroke.width * ss, LineStyle::Solid};
        else
            sh.stroke = Stroke{{kNoPaint, 100.f}, 0.f, LineStyle::Solid};
        /* A rectangle from its top-left corner; a line from its start, to
         * (w, h), which may be negative */
        Item it;
        it.xf = Transform::translate(fx + x0 * sx, fy + y0 * sy);
        it.w = (x1 - x0) * sx;
        it.h = (y1 - y0) * sy;
        it.content = sh;
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
        if (ref.index < 0 || !kept(slot_bounds(ref)))
            continue;
        const size_t i = (size_t) ref.index;
        if (ref.slot == VectorSlot::Text) {
            if (i >= scene.texts.size())
                continue;
            /* A text frame has no clip of its own. Clipped type stays in
             * the drawing, where the clip still applies. */
            if (scene.texts[i].clip >= 0) {
                flush_block();
                drawing.push_back(ref);
            } else {
                flush_drawing();
                PasteLine L;
                if (read_line(scene.texts[i], L))
                    add_line(std::move(L));
            }
        } else if (ref.slot == VectorSlot::Shape) {
            if (i >= scene.shapes.size())
                continue;
            flush_block();
            Shape::Kind kind;
            float x0, y0, x1, y1;
            if (simple_shape(scene.shapes[i], kind, x0, y0, x1, y1)) {
                flush_drawing();             // what's under it first
                add_shape(scene.shapes[i], kind, x0, y0, x1, y1);
            } else {
                drawing.push_back(ref);
            }
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
    if (group_pieces && r.items.size() >= 2)
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
                                const FontLibrary *fonts, bool paste, int pdf_page) {
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
                            : vector_loader().decode(bytes.data(), bytes.size(), &scene, &r.error, pdf_page);
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
    /* Pictures come out of an SVG as frames; a PDF page stays one placed page */
    if (images && scene.format != "pdf" &&
        place_grouped(doc, store, *images, scene, name, page, at, paste, fonts, r))
        return r;
    return place_whole(doc, store, std::move(bytes), std::move(scene), name, source, page, at, paste,
                       pdf_page);
}


ImportPages read_import(std::vector<uint8_t> bytes, const std::function<bool(size_t, size_t)> &progress) {
    ImportPages p;
    const int pages = bytes.empty() ? 0 : vector_loader().page_count(bytes.data(), bytes.size());
    VectorScene first;
    std::string err;
    if (pages < 1 || !vector_loader().decode(bytes.data(), bytes.size(), &first, &err, 0)) {
        p.error = err.empty() ? "pagemade can't read this kind of file" : err;
        return p;
    }
    if (!(first.width > 0.f) || !(first.height > 0.f)) {
        p.error = "the file's page has no size";
        return p;
    }
    p.pages_in_file = pages;
    const int count = std::min(pages, 5000);   // a sane page count
    p.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    p.format = first.format;
    /* The first page is read; the rest on worker threads */
    std::vector<VectorDecodeJob> jobs;
    for (int i = 1; i < count; ++i)
        jobs.push_back({p.bytes, i});
    std::vector<std::optional<VectorScene>> rest;
    const bool finished = decode_vector_pages(jobs, rest, nullptr, [&](size_t done, size_t) {
        return !progress || progress(done + 1, (size_t) count);
    });
    if (!finished) {
        p.cancelled = true;
        p.error = "cancelled";
        return p;
    }
    p.scenes.reserve((size_t) count);
    p.scenes.push_back(std::move(first));
    for (std::optional<VectorScene> &sc : rest)
        p.scenes.push_back(sc && sc->width > 0.f && sc->height > 0.f ? std::move(sc) : std::nullopt);
    p.ok = true;
    return p;
}

ImportedPublication build_import(ImportPages &pages, const std::string &name, const std::string &source,
                                 const FontLibrary *fonts, ImportMode mode,
                                 const std::function<bool(size_t, size_t)> &progress) {
    ImportedPublication r;
    r.images = std::make_unique<ImageStore>();
    if (!pages.ok || pages.scenes.empty() || !pages.scenes[0]) {
        r.cancelled = pages.cancelled;
        r.error = pages.error.empty() ? "nothing to import" : pages.error;
        return r;
    }
    const int count = (int) pages.scenes.size();
    const VectorScene &first = *pages.scenes[0];
    /* A print PDF's page is its trim, inside a larger sheet: the page is
     * that size, the sheet shifted so the trim's corner is the page's, and
     * what bleeds runs off the page as it does in a layout */
    auto trimmed = [](const VectorScene &sc, float &x, float &y, float &w, float &h) {
        if (sc.trim_w > 0.f && sc.trim_h > 0.f) {
            x = sc.trim_x;
            y = sc.trim_y;
            w = sc.trim_w;
            h = sc.trim_h;
        } else {
            x = y = 0.f;
            w = sc.width;
            h = sc.height;
        }
    };
    float tx0, ty0, tw0, th0;
    trimmed(first, tx0, ty0, tw0, th0);

    /* The pages are the file's, sized like its first. While placing, the
     * live area is the whole page, so each drawing goes in at full size at
     * the corner; ordinary margins and one column follow. */
    PageSetup setup;
    setup.width = tw0;
    setup.height = th0;
    if (first.bleed_w > 0.f)
        setup.bleed = std::max(0.f, std::min({first.trim_x - first.bleed_x, first.trim_y - first.bleed_y}));
    setup.facing = false;
    setup.columns = 1;
    PageSetup placing = setup;
    placing.margin_top = placing.margin_bottom = 0.f;
    placing.margin_inside = placing.margin_outside = 0.f;
    r.doc = new_publication(placing, count);

    const std::string leaf = name.empty() ? std::string("graphic") : name;
    std::vector<std::string> skipped, remarks;
    auto note_scene = [&](const VectorScene &sc) {
        for (const std::string &s : sc.skipped)
            if (std::find(skipped.begin(), skipped.end(), s) == skipped.end())
                skipped.push_back(s);
        for (const std::string &s : sc.remarks)
            if (std::find(remarks.begin(), remarks.end(), s) == remarks.end())
                remarks.push_back(s);
    };

    if (mode == ImportMode::Editable) {
        /* Each page taken apart: pictures in their own frames, text in text
         * frames, boxes and rules as objects, the rest as drawings. Not
         * grouped, so each piece moves on its own. */
        size_t placed = 0;
        for (int i = 0; i < count; ++i) {
            if (progress && !progress((size_t) i, (size_t) count)) {
                r.cancelled = true;
                r.error = "cancelled";
                return r;
            }
            if (!pages.scenes[(size_t) i])
                continue;                // a page that can't be read stays empty
            const VectorScene &scene = *pages.scenes[(size_t) i];
            note_scene(scene);
            VectorPlacement pr;
            const std::string page_name = count > 1 ? file_stem(leaf) + "-p" + std::to_string(i + 1) : leaf;
            float tx, ty, tw, th;
            trimmed(scene, tx, ty, tw, th);
            PlaceApart apart;
            apart.group = false;
            apart.have_frame = true;             // at full size, the trim's corner at the page's
            apart.fx = -tx;
            apart.fy = -ty;
            apart.fw = scene.width;
            apart.fh = scene.height;
            if (scene.bleed_w > 0.f) {           // printer's marks lie past the bleed
                apart.have_keep = true;
                apart.kx0 = scene.bleed_x;
                apart.ky0 = scene.bleed_y;
                apart.kx1 = scene.bleed_x + scene.bleed_w;
                apart.ky1 = scene.bleed_y + scene.bleed_h;
            }
            if (place_as_text(r.doc, r.vectors, r.images.get(), fonts, scene, page_name, (size_t) i, nullptr,
                              false, pr, &apart)) {
                placed += pr.items.size();
                if (!pr.error.empty())
                    remarks.push_back("page " + std::to_string(i + 1) + ": " + pr.error);
            }
        }
        if (progress)
            progress((size_t) count, (size_t) count);
        if (placed == 0) {
            r.error = "the file has nothing pagemade could place";
            return r;
        }
    } else if (count == 1 && pages.format != "pdf") {
        /* One page (an SVG), exact: placed as Place does, so its pictures
         * come out as frames of their own */
        VectorPlacement pl = place_svg_bytes(r.doc, r.vectors, std::vector<uint8_t>(*pages.bytes), leaf, source, 0,
                                             nullptr, r.images.get(), SvgPaste::Drawing, fonts);
        if (!pl.ok) {
            r.error = pl.error;
            return r;
        }
        note_scene(first);
    } else {
        /* Exact: each page one graphic, the pages sharing the file's bytes */
        const VectorStore::Bytes shared = pages.bytes;
        const uint64_t hash = image_bytes_hash(shared->data(), shared->size());
        for (int i = 0; i < count; ++i) {
            if (!pages.scenes[(size_t) i])
                continue;
            VectorScene scene = std::move(*pages.scenes[(size_t) i]);
            note_scene(scene);
            const uint32_t id = r.vectors.take_id();
            VectorAsset asset;
            asset.id = id;
            asset.name = leaf;
            asset.source = source;
            asset.width = scene.width;
            asset.height = scene.height;
            asset.skipped = scene.skipped;
            asset.page = i;
            asset.source_size = shared->size();
            asset.hash = hash;
            /* The frame shows the bleed (or the whole box), the trim's
             * corner at the page's; the marks past the bleed are cropped */
            float tx, ty, tw, th;
            trimmed(scene, tx, ty, tw, th);
            float bx = 0.f, by = 0.f, bw = scene.width, bh = scene.height;
            if (scene.bleed_w > 0.f) {
                bx = scene.bleed_x;
                by = scene.bleed_y;
                bw = scene.bleed_w;
                bh = scene.bleed_h;
            }
            const float sw = scene.width, sh = scene.height;
            if (!r.vectors.add(id, shared, std::move(scene)))
                continue;
            r.doc.add_vector(asset);
            Item it;
            it.w = bw;
            it.h = bh;
            it.xf = Transform::translate(bx - tx, by - ty);
            PlacedVector pv;
            pv.asset = id;
            pv.x = -bx;
            pv.y = -by;
            pv.w = sw;
            pv.h = sh;
            it.content = pv;
            r.doc.add_item((size_t) i, std::move(it));
        }
        if (r.doc.vectors.empty()) {
            r.error = "none of the file's pages could be read";
            return r;
        }
    }
    r.doc.setup = setup;                 // margins back once the drawings are in
    r.note = vector_place_note(leaf, skipped, remarks);
    r.note.replace(0, 6, "imported");    // "placed x" -> "imported x"
    if (count > 1)
        r.note += " (" + std::to_string(count) + " pages" +
                  (pages.pages_in_file > count ? " of " + std::to_string(pages.pages_in_file) : std::string()) + ")";
    r.ok = true;
    return r;
}

ImportedPublication import_publication(std::vector<uint8_t> bytes, const std::string &name,
                                       const std::string &source, const FontLibrary *fonts,
                                       const std::function<bool(size_t done, size_t total)> &progress,
                                       ImportMode mode) {
    ImportPages pages = read_import(std::move(bytes), progress);
    return build_import(pages, name, source, fonts, mode);
}

} // namespace pagemade
