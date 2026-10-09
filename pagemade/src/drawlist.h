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

struct DrawFill {
    Path      path;
    Color     color;
    Transform xf;
};

struct DrawStroke {
    Path               path;
    Color              color;
    float              width = 1.f;
    std::vector<float> dash;            // on/off lengths; empty = solid
    LineCap            cap = LineCap::Butt;
    LineJoin           join = LineJoin::Miter;
    Transform          xf;
};

struct DrawGlyphs {
    const GlyphRun *run = nullptr;
    Transform       xf;
};

using DrawOp = std::variant<DrawFill, DrawStroke, DrawGlyphs>;
using DrawList = std::vector<DrawOp>;

/* `comps` holds one composition per story, in PageDoc::stories order. */
DrawList build_page(const PageDoc &doc, size_t page, const std::vector<Composition> &comps);

} // namespace pagemade
