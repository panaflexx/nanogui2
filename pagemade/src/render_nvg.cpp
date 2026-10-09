/*
 * pagemade/render_nvg.cpp — see render_nvg.h.
 */
#include "render_nvg.h"

#include <nanovg.h>

#include <cmath>

namespace pagemade {

void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run) {
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
    if (!any)
        return;
    nvgFillColor(ctx, nvgRGBAf(run.color.r, run.color.g, run.color.b, run.color.a));
    nvgFill(ctx);
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

NVGcolor nvg_color(const Color &c) { return nvgRGBAf(c.r, c.g, c.b, c.a); }

void draw_stroke(NVGcontext *ctx, const DrawStroke &s, float px) {
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
    for (const Polyline &d : dash(flatten(s.path, tol), s.dash)) {
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
        nvgFillColor(ctx, nvg_color(s.color));
        nvgFill(ctx);
    }
}

} // namespace

void draw_list(NVGcontext *ctx, const DrawList &list, float px) {
    for (const DrawOp &op : list) {
        nvgSave(ctx);
        if (const auto *g = std::get_if<DrawGlyphs>(&op)) {
            apply_transform(ctx, g->xf);
            draw_glyph_run(ctx, *g->run);
        } else if (const auto *f = std::get_if<DrawFill>(&op)) {
            apply_transform(ctx, f->xf);
            nvgBeginPath(ctx);
            add_path(ctx, f->path);
            nvgFillColor(ctx, nvg_color(f->color));
            nvgFill(ctx);
        } else if (const auto *s = std::get_if<DrawStroke>(&op)) {
            apply_transform(ctx, s->xf);
            draw_stroke(ctx, *s, px);
        }
        nvgRestore(ctx);
    }
}

} // namespace pagemade
