/*
 * pagemade/render_nvg.h — draw composed text with NanoVG.
 *
 * Glyphs are filled as outlines at their composed positions, so they stay
 * sharp and exactly placed at any zoom. Coordinates are page points; the
 * caller's transform maps points to the screen.
 *
 * Each run is one path and one fill. NanoVG forces every subpath to the
 * same winding unless told otherwise, so every contour is tagged solid or
 * hole from GlyphOutline::hole; otherwise counters (the inside of 'o')
 * would fill in.
 */
#pragma once

#include "composer/composer.h"

struct NVGcontext;

namespace pagemade {

void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run);
void draw_composition(NVGcontext *ctx, const Composition &comp);

} // namespace pagemade
