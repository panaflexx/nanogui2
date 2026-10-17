/*
 * pagemade/drawlist.h — a page as a list of drawing operations.
 *
 * build_page() walks a page's items bottom to top and emits fills, strokes
 * and glyph runs in points, each with the item's transform. Paints are
 * already resolved to colors and line styles to dash patterns, so a backend
 * (NanoVG on screen, later PDF and SVG) only has to draw what it's given.
 *
 * Glyph ops point into the compositions passed in; keep those alive (and
 * unchanged) while the list is in use.
 */
#pragma once

#include "geometry.h"

#include <variant>
#include <vector>

namespace pagemade {

enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };
/* Nonzero is what shapes and glyphs already use. Even-odd is for SVG. */
enum class FillRule { NonZero, EvenOdd };

struct DrawFill {
    Path      path;
    Color     color;
    Transform xf;
    FillRule  rule = FillRule::NonZero;
    /* The frame in item space. 0 leaves the path unclipped (shapes). */
    float     clip_w = 0, clip_h = 0;
};

struct DrawStroke {
    Path               path;
    Color              color;
    float              width = 1.f;
    std::vector<float> dash;            // on/off lengths; empty = solid
    LineCap            cap = LineCap::Butt;
    LineJoin           join = LineJoin::Miter;
    float              dash_offset = 0;
    Transform          xf;
    float              clip_w = 0, clip_h = 0;
};

struct DrawGlyphs {
    const GlyphRun *run = nullptr;
    Transform       xf;
    Color           color;           // the run's swatch and tint, resolved
};

/* A placed picture. The frame (0, 0)–(clip_w, clip_h) in item space clips
 * the picture rectangle (x, y, w, h). The pixels are not here: the screen
 * asks the ImageStore for a display view, and PDF asks it for the source. */
struct DrawImage {
    uint32_t  asset = 0;
    float     x = 0, y = 0, w = 0, h = 0;
    float     clip_w = 0, clip_h = 0;
    float     opacity = 1.f;             // 0 to 1
    Transform xf;
};

using DrawOp = std::variant<DrawFill, DrawStroke, DrawGlyphs, DrawImage>;
using DrawList = std::vector<DrawOp>;

/* `comps` holds one composition per story, in PageDoc::stories order.
 * `which`, when set, skips those items, or draws only them when `only`
 * is set. A null list draws the whole page. */
class VectorStore;

/* `vectors` supplies placed SVG scenes. Null draws those frames as nothing. */
DrawList build_page(const PageDoc &doc, size_t page, const std::vector<Composition> &comps,
                    const std::vector<ItemId> *which = nullptr, bool only = false,
                    const VectorStore *vectors = nullptr);

} // namespace pagemade
