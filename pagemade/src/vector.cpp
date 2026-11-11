/*
 * pagemade/vector.cpp — the vector loader, the store, and placement.
 * The SVG decoder itself is plugin/svg.cpp; this file only calls it.
 */
#include "vector.h"
#include "plugin/plugin.h"

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

std::vector<std::pair<std::string, std::string>> VectorLoader::types() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &d : m_decoders)
        for (const auto &t : d->types())
            out.push_back(t);
    return out;
}

int VectorLoader::page_count(const uint8_t *data, size_t n) const {
    const VectorDecoder *d = match(data, n);
    return d ? d->page_count(data, n) : 0;
}

bool VectorLoader::decode(const uint8_t *data, size_t n, VectorScene *out, std::string *error,
                          int page) const {
    const VectorDecoder *d = match(data, n);
    if (!d) {
        if (error)
            *error = "no vector decoder accepted this file";
        return false;
    }
    return page == 0 ? d->decode(data, n, out, error) : d->decode_page(data, n, page, out, error);
}

VectorLoader &vector_loader() {
    static VectorLoader loader;
    static bool ready = (register_vector_plugins(loader), true);
    (void) ready;
    return loader;
}

std::vector<std::pair<std::string, std::string>> vector_file_types() {
    return vector_loader().types();
}

std::string vector_place_note(const std::string &name, const std::vector<std::string> &skipped,
                              const std::vector<std::string> &remarks) {
    std::string s = "placed " + (name.empty() ? std::string("graphic") : name);
    if (!skipped.empty()) {
        s += " (";
        for (size_t i = 0; i < skipped.size(); ++i) {
            if (i)
                s += ", ";
            s += skipped[i];
        }
        s += " skipped)";
    }
    for (const std::string &r : remarks)
        s += "; " + r;
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
    return add(id, std::make_shared<const std::vector<uint8_t>>(std::move(bytes)), std::move(scene));
}

bool VectorStore::add(uint32_t id, Bytes bytes, VectorScene scene) {
    if (id == 0 || !bytes)
        return false;
    Entry e;
    e.source = std::move(bytes);
    e.scene = std::move(scene);
    note_id(id);
    m_entries[id] = std::move(e);
    return true;
}

bool VectorStore::add(uint32_t id, Bytes bytes, std::string *error, int page) {
    if (id == 0 || !bytes) {
        if (error)
            *error = "graphic id is missing";
        return false;
    }
    VectorScene scene;
    if (!vector_loader().decode(bytes->data(), bytes->size(), &scene, error, page))
        return false;
    return add(id, std::move(bytes), std::move(scene));
}

bool VectorStore::add(uint32_t id, std::vector<uint8_t> bytes, std::string *error, int page) {
    if (id == 0) {
        if (error)
            *error = "graphic id is missing";
        return false;
    }
    VectorScene scene;
    if (!vector_loader().decode(bytes.data(), bytes.size(), &scene, error, page))
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

} // namespace

/* The family a run's text names here: the first of its families the
 * library has, else the pagemade family a well-known name stands for
 * (P052 for Serif, and so on). Paste as Text names this family in its
 * frames and measures with it, so the frame widths and the composer agree. */
std::string vector_run_family(const FontLibrary *fonts, const VectorTextRun &run) {
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

const Font *vector_run_face(const FontLibrary &fonts, const VectorTextRun &run) {
    const std::string family = vector_run_family(&fonts, run);
    if (fonts.has_family(family))
        if (const Font *f = fonts.match(family, run.weight, run.italic, ""))
            return f;
    return fonts.find("Serif", run.weight >= 600, run.italic);
}

namespace {

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
            const Font *font = vector_run_face(fonts, run);
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
