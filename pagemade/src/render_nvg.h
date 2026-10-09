/*
 * pagemade/render_nvg.h — draw page content with NanoVG.
 *
 * draw_list() draws a page's draw list (drawlist.h): fills, strokes and
 * glyph runs, each under its item's transform. Coordinates are page
 * points; the caller's transform maps points to the screen, and `px` is one
 * screen pixel in points (it sets how finely dashed curves are flattened).
 *
 * Glyphs are filled as outlines at their composed positions, so they stay
 * sharp and exactly placed at any zoom. Each run is one path and one fill.
 * NanoVG forces every subpath to the same winding unless told otherwise,
 * so every contour is tagged solid or hole from GlyphOutline::hole;
 * otherwise counters (the inside of 'o') would fill in.
 */
#pragma once

#include "drawlist.h"

struct NVGcontext;

namespace pagemade {

void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run);
void draw_list(NVGcontext *ctx, const DrawList &list, float px);
void apply_transform(NVGcontext *ctx, const Transform &t);

} // namespace pagemade
