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

#include <functional>

struct NVGcontext;

namespace pagemade {

/* In `color` (the draw list's resolved swatch), or the run's own RGB. */
void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run, const Color &color);
void draw_glyph_run(NVGcontext *ctx, const GlyphRun &run);
/* `texture` returns a NanoVG image for a picture, 0 for the missing-picture
 * stand-in, or -1 for a plain tile while it loads. The id is the display
 * view, not the source file. An empty picture frame (asset 0) draws nothing
 * unless `texture` returns kEmptyFramePlaceholder for it: then the screen's
 * placeholder, a light frame with a cross. */
constexpr int kEmptyFramePlaceholder = -2;
void draw_list(NVGcontext *ctx, const DrawList &list, float px,
               const std::function<int(const DrawImage &)> &texture = {});
void apply_transform(NVGcontext *ctx, const Transform &t);
/* Gradient fills draw from small ramp images kept per context. Call once a
 * frame, before any draw_list in it: ramps not drawn for a while are freed. */
void release_unused_ramps(NVGcontext *ctx);

} // namespace pagemade
