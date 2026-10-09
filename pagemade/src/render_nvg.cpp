/*
 * pagemade/render_nvg.cpp — see render_nvg.h.
 */
#include "render_nvg.h"

#include <nanovg.h>

namespace pagemade {

void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run) {
    if (!run.font || run.glyphs.empty())
        return;
    const float sy = run.size / (float) run.font->units_per_em();
    const float sx = sy * run.hscale * 0.01f;

    bool any = false;
    nvgBeginPath(ctx);
    for (const PlacedGlyph &g : run.glyphs) {
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

void draw_composition(NVGcontext *ctx, const Composition &comp) {
    for (const ComposedLine &line : comp.lines)
        for (const GlyphRun &run : line.runs)
            draw_glyph_run(ctx, run);
}

} // namespace pagemade
