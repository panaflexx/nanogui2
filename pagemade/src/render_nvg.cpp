/*
 * pagemade/render_nvg.cpp — see render_nvg.h.
 */
#include "render_nvg.h"

#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace pagemade {

void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run) {
    draw_glyph_run(ctx, run, run.color);
}

void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run, const Color &color) {
    if (!run.font || run.glyphs.empty())
        return;
    const float sy = run.size / (float) run.font->units_per_em();
    const float sx = sy * run.hscale * 0.01f;

    bool any = false;
    nvgBeginPath(ctx);
    for (const PlacedGlyph &g : run.glyphs) {
        if (g.flags & PlacedGlyph::Invisible)
            continue;                    // tabs, breaks, hanging spaces
        const GlyphOutline &o = run.font->outline(g.gid);
        if (o.empty())
            continue;
        any = true;
        /* Design units are y-up; the page is y-down. */
        auto X = [&](float x) { return g.x + x * sx; };
        auto Y = [&](float y) { return g.y - y * sy; };
        int contour = -1;
        bool open = false;
        auto end_contour = [&] {
            if (!open)
                return;
            bool hole = contour < (int) o.hole.size() && o.hole[contour];
            nvgPathWinding(ctx, hole ? NVG_HOLE : NVG_SOLID);
            open = false;
        };
        for (const PathCmd &c : o.cmds) {
            switch (c.kind) {
            case PathCmd::Move:
                end_contour();
                ++contour;
                nvgMoveTo(ctx, X(c.x), Y(c.y));
                open = true;
                break;
            case PathCmd::Line:
                nvgLineTo(ctx, X(c.x), Y(c.y));
                break;
            case PathCmd::Quad:
                nvgQuadTo(ctx, X(c.cx1), Y(c.cy1), X(c.x), Y(c.y));
                break;
            case PathCmd::Cubic:
                nvgBezierTo(ctx, X(c.cx1), Y(c.cy1), X(c.cx2), Y(c.cy2), X(c.x), Y(c.y));
                break;
            case PathCmd::Close:
                nvgClosePath(ctx);
                end_contour();
                break;
            }
        }
        end_contour();
    }
    if (any) {
        nvgFillColor(ctx, nvgRGBAf(color.r, color.g, color.b, color.a));
        /* A placed SVG may have left even-odd on the context. Glyph holes
         * are nonzero winding, and a leftover rule would punch them out. */
        nvgFillRule(ctx, NVG_FILLRULE_NONZERO);
        nvgFill(ctx);
    }
    std::vector<RuleSpan> rules;
    glyph_run_rules(run, rules);
    for (const RuleSpan &rule : rules) {
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, rule.x0, rule.y);
        nvgLineTo(ctx, rule.x1, rule.y);
        nvgStrokeColor(ctx, nvgRGBAf(color.r, color.g, color.b, color.a));
        nvgStrokeWidth(ctx, rule.thickness);
        nvgStroke(ctx);
    }
}

void apply_transform(NVGcontext *ctx, const Transform &t) {
    nvgTransform(ctx, t.a, t.b, t.c, t.d, t.e, t.f);
}

namespace {

float transform_scale(const Transform &t) {
    float s = std::sqrt(std::fabs(t.a * t.d - t.b * t.c));
    return s > 1e-6f ? s : 1.f;
}

void add_path(NVGcontext *ctx, const Path &p) {
    for (const PathCmd &c : p) {
        switch (c.kind) {
        case PathCmd::Move:  nvgMoveTo(ctx, c.x, c.y); break;
        case PathCmd::Line:  nvgLineTo(ctx, c.x, c.y); break;
        case PathCmd::Quad:  nvgQuadTo(ctx, c.cx1, c.cy1, c.x, c.y); break;
        case PathCmd::Cubic: nvgBezierTo(ctx, c.cx1, c.cy1, c.cx2, c.cy2, c.x, c.y); break;
        case PathCmd::Close: nvgClosePath(ctx); break;
        }
    }
}

/* A path to fill. NanoVG turns every subpath counter-clockwise unless told
 * its winding, which fills in a nonzero path's holes (an SVG drawn as one
 * path with reversed inner contours), so each subpath is tagged as
 * fill_windings() says. `windings` is that list when it was worked out
 * ahead; null works it out here. */
void add_fill_path(NVGcontext *ctx, const Path &p, const std::vector<uint8_t> *windings) {
    std::vector<uint8_t> own;
    if (!windings) {
        own = fill_windings(p);
        windings = &own;
    }
    size_t index = 0;
    bool open = false;
    auto end_subpath = [&] {
        if (!open)
            return;
        if (index < windings->size())
            nvgPathWinding(ctx, (*windings)[index] ? NVG_CCW : NVG_CW);
        ++index;
        open = false;
    };
    for (const PathCmd &c : p) {
        switch (c.kind) {
        case PathCmd::Move:
            end_subpath();
            nvgMoveTo(ctx, c.x, c.y);
            open = true;
            break;
        case PathCmd::Line:
            nvgLineTo(ctx, c.x, c.y);
            open = true;
            break;
        case PathCmd::Quad:
            nvgQuadTo(ctx, c.cx1, c.cy1, c.x, c.y);
            open = true;
            break;
        case PathCmd::Cubic:
            nvgBezierTo(ctx, c.cx1, c.cy1, c.cx2, c.cy2, c.x, c.y);
            open = true;
            break;
        case PathCmd::Close:
            nvgClosePath(ctx);
            end_subpath();
            break;
        }
    }
    end_subpath();
}

NVGcolor nvg_color(const Color &c) { return nvgRGBAf(c.r, c.g, c.b, c.a); }

/* The clip path, under the transform already on the context. Layers
 * intersect; the paths in a layer make a union. */
void set_clip(NVGcontext *ctx, const ClipLayers &clip) {
    for (const auto &layer : clip) {
        for (const ClipPath &cp : layer) {
            nvgBeginPath(ctx);
            add_fill_path(ctx, cp.path, nullptr);
            nvgFillRule(ctx, cp.rule == FillRule::EvenOdd ? NVG_FILLRULE_EVENODD : NVG_FILLRULE_NONZERO);
            nvgClipPath(ctx);
        }
        nvgClipEnd(ctx);
    }
}

/* Gradient ramps: one 256 x 1 premultiplied image per set of stops, per
 * context, kept while it is drawn (see release_unused_ramps). */
constexpr int kRampWidth = 256;          // nanovg_gl.h's ramp shader assumes this
constexpr uint64_t kRampKeepFrames = 120;

struct Ramp {
    int image = 0;
    uint64_t used = 0;
};

struct RampCache {
    std::map<std::string, Ramp> ramps;
    uint64_t frame = 1;
};

std::map<NVGcontext *, RampCache> &ramp_caches() {
    static std::map<NVGcontext *, RampCache> caches;
    return caches;
}

std::string ramp_key(const std::vector<GradientStop> &stops) {
    std::string key(stops.size() * sizeof(float) * 5, '\0');
    char *at = &key[0];
    for (const GradientStop &s : stops) {
        const float v[5] = {s.offset, s.color.r, s.color.g, s.color.b, s.color.a};
        std::memcpy(at, v, sizeof v);
        at += sizeof v;
    }
    return key;
}

/* Straight-alpha interpolation between stops (SVG), then premultiplied.
 * Before the first stop and after the last, their colors. */
void fill_ramp(const std::vector<GradientStop> &stops, uint8_t *rgba) {
    const size_t n = stops.size();
    for (int i = 0; i < kRampWidth; ++i) {
        const float t = (float) i / (float) (kRampWidth - 1);
        Color c = stops.back().color;
        if (t <= stops.front().offset) {
            c = stops.front().color;
        } else {
            size_t j = 0;
            while (j + 1 < n && stops[j + 1].offset <= t)
                ++j;
            if (j + 1 < n) {
                const GradientStop &a = stops[j], &b = stops[j + 1];
                const float u = (t - a.offset) / (b.offset - a.offset);
                c = {a.color.r + (b.color.r - a.color.r) * u, a.color.g + (b.color.g - a.color.g) * u,
                     a.color.b + (b.color.b - a.color.b) * u, a.color.a + (b.color.a - a.color.a) * u};
            }
        }
        const float al = std::clamp(c.a, 0.f, 1.f);
        auto byte = [](float v) { return (uint8_t) std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
        rgba[i * 4 + 0] = byte(c.r * al);
        rgba[i * 4 + 1] = byte(c.g * al);
        rgba[i * 4 + 2] = byte(c.b * al);
        rgba[i * 4 + 3] = byte(al);
    }
}

int ramp_image(NVGcontext *ctx, const std::vector<GradientStop> &stops) {
    RampCache &cache = ramp_caches()[ctx];
    Ramp &r = cache.ramps[ramp_key(stops)];
    int w = 0, h = 0;
    /* A context made at a dead one's address finds stale ids: remake them. */
    if (r.image) {
        nvgImageSize(ctx, r.image, &w, &h);   // leaves 0 x 0 for a missing image
        if (w != kRampWidth || h != 1)
            r.image = 0;
    }
    if (!r.image) {
        uint8_t rgba[kRampWidth * 4];
        fill_ramp(stops, rgba);
        r.image = nvgCreateImageRGBA(ctx, kRampWidth, 1, NVG_IMAGE_PREMULTIPLIED, rgba);
    }
    r.used = cache.frame;
    return r.image;
}

NVGpaint gradient_paint(NVGcontext *ctx, const DrawGradient &g, const Color &fallback) {
    const float xf[6] = {g.xf.a, g.xf.b, g.xf.c, g.xf.d, g.xf.e, g.xf.f};
    const int spread = g.spread == Spread::Reflect  ? NVG_SPREAD_REFLECT
                       : g.spread == Spread::Repeat ? NVG_SPREAD_REPEAT
                                                    : NVG_SPREAD_PAD;
    return nvgRampGradient(ctx, xf, g.radial ? NVG_RAMP_RADIAL : NVG_RAMP_LINEAR, g.focal.x,
                           g.focal.y, spread, ramp_image(ctx, g.stops), 1.f, nvg_color(fallback));
}

void draw_stroke(NVGcontext *ctx, const DrawStroke &s, float px) {
    if (s.gradient)
        nvgStrokePaint(ctx, gradient_paint(ctx, *s.gradient, s.color));
    else
        nvgStrokeColor(ctx, nvg_color(s.color));
    nvgStrokeWidth(ctx, s.width);
    nvgLineCap(ctx, s.cap == LineCap::Round ? NVG_ROUND : s.cap == LineCap::Square ? NVG_SQUARE : NVG_BUTT);
    nvgLineJoin(ctx, s.join == LineJoin::Round ? NVG_ROUND : s.join == LineJoin::Bevel ? NVG_BEVEL : NVG_MITER);
    if (s.dash.empty()) {
        nvgBeginPath(ctx);
        add_path(ctx, s.path);
        nvgStroke(ctx);
        return;
    }
    /* NanoVG has no dashes: cut the flattened path here. Flatten to a
     * quarter pixel at the current zoom so curves stay smooth. */
    const float tol = 0.25f * px / transform_scale(s.xf);
    std::vector<Point> dots;
    bool any = false;
    nvgBeginPath(ctx);
    for (const Polyline &d : dash(flatten(s.path, tol), s.dash, s.dash_offset)) {
        float len = 0;
        for (size_t i = 1; i < d.pts.size(); ++i)
            len += std::hypot(d.pts[i].x - d.pts[i - 1].x, d.pts[i].y - d.pts[i - 1].y);
        if (len < 1e-3f) {
            dots.push_back(d.pts.front());   // a zero-length dash: a round-capped dot
            continue;
        }
        nvgMoveTo(ctx, d.pts[0].x, d.pts[0].y);
        for (size_t i = 1; i < d.pts.size(); ++i)
            nvgLineTo(ctx, d.pts[i].x, d.pts[i].y);
        any = true;
    }
    if (any)
        nvgStroke(ctx);
    if (!dots.empty() && s.cap != LineCap::Butt) {
        nvgBeginPath(ctx);
        for (const Point &d : dots)
            nvgCircle(ctx, d.x, d.y, s.width * 0.5f);
        if (s.gradient)
            nvgFillPaint(ctx, gradient_paint(ctx, *s.gradient, s.color));
        else
            nvgFillColor(ctx, nvg_color(s.color));
        nvgFillRule(ctx, NVG_FILLRULE_NONZERO);
        nvgFill(ctx);
    }
}

/* Type this small on screen (its size in pixels) is greeked: each word is
 * a soft bar along its x-height, the way layout programs draw small text.
 * Outlines that small read as gray anyway, and filling them is most of the
 * cost of a zoomed-out spread. */
constexpr float kGreekBelowPx = 4.f;

void draw_greeked_run(NVGcontext *ctx, const GlyphRun &run, const Color &color) {
    if (!run.font || run.glyphs.empty())
        return;
    const float top = -0.5f * run.size, h = 0.42f * run.size;
    bool any = false, open = false;
    float x0 = 0, x1 = 0, base = 0;
    nvgBeginPath(ctx);
    auto close_word = [&] {
        if (open && x1 > x0) {
            nvgRect(ctx, x0, base + top, x1 - x0, h);
            any = true;
        }
        open = false;
    };
    for (const PlacedGlyph &g : run.glyphs) {
        const bool ink = !(g.flags & PlacedGlyph::Invisible) && !run.font->outline(g.gid).empty();
        if (!ink || (open && g.y != base)) {
            close_word();
            if (!ink)
                continue;
        }
        if (!open) {
            x0 = g.x;
            base = g.y;
            open = true;
        }
        x1 = g.x + g.adv;
    }
    close_word();
    if (!any)
        return;
    nvgFillColor(ctx, nvgRGBAf(color.r, color.g, color.b, color.a * 0.45f));
    nvgFillRule(ctx, NVG_FILLRULE_NONZERO);
    nvgFill(ctx);
}

/* A picture still being read: the placeholder box with a gray cross (the
 * missing picture's cross is red). */
void draw_loading_image(NVGcontext *ctx, const DrawImage &im) {
    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, im.clip_w, im.clip_h);
    nvgFillColor(ctx, nvgRGB(236, 236, 236));
    nvgFill(ctx);
    nvgStrokeColor(ctx, nvgRGB(170, 170, 170));
    nvgStrokeWidth(ctx, 1.f);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, 0, 0);
    nvgLineTo(ctx, im.clip_w, im.clip_h);
    nvgMoveTo(ctx, im.clip_w, 0);
    nvgLineTo(ctx, 0, im.clip_h);
    nvgStroke(ctx);
}

void draw_missing_image(NVGcontext *ctx, const DrawImage &im) {
    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, im.clip_w, im.clip_h);
    nvgFillColor(ctx, nvgRGB(230, 230, 230));
    nvgFill(ctx);
    nvgStrokeColor(ctx, nvgRGB(160, 50, 50));
    nvgStrokeWidth(ctx, 1.f);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, 0, 0);
    nvgLineTo(ctx, im.clip_w, im.clip_h);
    nvgMoveTo(ctx, im.clip_w, 0);
    nvgLineTo(ctx, 0, im.clip_h);
    nvgStroke(ctx);
}

} // namespace

void draw_list(NVGcontext *ctx, const DrawList &list, float px,
               const std::function<int(const DrawImage &)> &texture) {
    /* A clip path stays set while consecutive ops share it (one placed
     * SVG's shapes under one <clipPath>), and is reset before anything else. */
    const ClipLayers *active = nullptr;
    auto use_clip = [&](const DrawClip &want) {
        if (want.get() == active)
            return;
        if (active)
            nvgResetClip(ctx);
        active = want.get();
        if (active)
            set_clip(ctx, *active);
    };
    for (const DrawOp &op : list) {
        nvgSave(ctx);
        if (const auto *g = std::get_if<DrawGlyphs>(&op)) {
            use_clip(nullptr);
            apply_transform(ctx, g->xf);
            if (g->run->size * transform_scale(g->xf) < kGreekBelowPx * px)
                draw_greeked_run(ctx, *g->run, g->color);
            else
                draw_glyph_run(ctx, *g->run, g->color);
        } else if (const auto *f = std::get_if<DrawFill>(&op)) {
            apply_transform(ctx, f->xf);
            use_clip(f->clip);
            if (f->clip_w > 0.f && f->clip_h > 0.f)
                nvgIntersectScissor(ctx, 0, 0, f->clip_w, f->clip_h);
            nvgBeginPath(ctx);
            add_fill_path(ctx, f->path, f->windings.get());
            if (f->gradient)
                nvgFillPaint(ctx, gradient_paint(ctx, *f->gradient, f->color));
            else
                nvgFillColor(ctx, nvg_color(f->color));
            nvgFillRule(ctx, f->rule == FillRule::EvenOdd ? NVG_FILLRULE_EVENODD
                                                          : NVG_FILLRULE_NONZERO);
            nvgFill(ctx);
        } else if (const auto *s = std::get_if<DrawStroke>(&op)) {
            apply_transform(ctx, s->xf);
            use_clip(s->clip);
            if (s->clip_w > 0.f && s->clip_h > 0.f)
                nvgIntersectScissor(ctx, 0, 0, s->clip_w, s->clip_h);
            draw_stroke(ctx, *s, px);
        } else if (const auto *im = std::get_if<DrawImage>(&op)) {
            use_clip(nullptr);
            apply_transform(ctx, im->xf);
            /* Intersect, so the pasteboard's window clip stays in force.
             * The frame itself is exact, including when the picture is rotated. */
            nvgIntersectScissor(ctx, 0, 0, im->clip_w, im->clip_h);
            int img = texture ? texture(*im) : 0;
            if (img > 0 && im->w != 0.f && im->h != 0.f) {
                nvgBeginPath(ctx);
                nvgRect(ctx, im->x, im->y, im->w, im->h);
                NVGpaint paint = nvgImagePattern(ctx, im->x, im->y, im->w, im->h, 0, img,
                                                 im->opacity);
                nvgFillPaint(ctx, paint);
                nvgFill(ctx);
            } else if (img < 0) {
                draw_loading_image(ctx, *im);
            } else {
                draw_missing_image(ctx, *im);
            }
        }
        nvgRestore(ctx);
    }
    use_clip(nullptr);
}

void release_unused_ramps(NVGcontext *ctx) {
    auto found = ramp_caches().find(ctx);
    if (found == ramp_caches().end())
        return;
    RampCache &cache = found->second;
    for (auto it = cache.ramps.begin(); it != cache.ramps.end();) {
        if (it->second.used + kRampKeepFrames < cache.frame) {
            if (it->second.image)
                nvgDeleteImage(ctx, it->second.image);
            it = cache.ramps.erase(it);
        } else {
            ++it;
        }
    }
    ++cache.frame;
}

} // namespace pagemade
