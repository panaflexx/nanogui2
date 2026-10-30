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

#include <memory>
#include <variant>
#include <vector>

namespace pagemade {

enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };
/* Nonzero is what shapes and glyphs already use. Even-odd is for SVG. */
enum class FillRule { NonZero, EvenOdd };

/* A clip path (SVG clip-path), in the same space as the op it clips. Each
 * layer is the union of its paths; the layers intersect. Ops that share a
 * clip share the pointer, so a backend can set it up once for a run. */
struct ClipPath {
    Path     path;
    FillRule rule = FillRule::NonZero;
};
using ClipLayers = std::vector<std::vector<ClipPath>>;
using DrawClip = std::shared_ptr<const ClipLayers>;

struct GradientStop {
    float offset = 0;                   // 0 to 1, not decreasing along the list
    Color color;                        // straight (not premultiplied) alpha
};

/* What a gradient shows past its ends. */
enum class Spread { Pad, Reflect, Repeat };

/* A gradient paint (SVG). `xf` maps gradient space to the op's space. In
 * gradient space a linear gradient has t = x, from (0,0) to (1,0); a radial
 * one has t = 1 on the unit circle at the origin and t = 0 at `focal`.
 * The op's color is the fallback where gradients aren't drawn. */
struct DrawGradient {
    bool radial = false;
    std::vector<GradientStop> stops;    // two or more
    Transform xf;
    Point focal{0, 0};
    Spread spread = Spread::Pad;
};
using DrawGradientRef = std::shared_ptr<const DrawGradient>;

struct DrawFill {
    Path      path;
    Color     color;
    Transform xf;
    FillRule  rule = FillRule::NonZero;
    /* The frame in item space. 0 leaves the path unclipped (shapes). */
    float     clip_w = 0, clip_h = 0;
    DrawClip  clip{};                   // null: no clip path
    DrawGradientRef gradient{};         // null: `color` is the paint
    /* fill_windings(path), when worked out ahead (placed SVG); null has
     * the renderer work it out. */
    std::shared_ptr<const std::vector<uint8_t>> windings{};
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
    DrawClip           clip{};
    DrawGradientRef    gradient{};
};

struct DrawGlyphs {
    const GlyphRun *run = nullptr;
    Transform       xf;
    Color           color;           // the run's swatch and tint, resolved
};

/* A placed picture. The frame (0, 0)–(clip_w, clip_h) in item space clips
 * the picture rectangle (x, y, w, h). An asset's pixels live in the
 * ImageStore. An SVG <image> carries its own bytes (a data URI) and a clip
 * path in the same space as the rectangle. */
struct DrawImage {
    uint32_t  asset = 0;
    float     x = 0, y = 0, w = 0, h = 0;
    float     clip_w = 0, clip_h = 0;
    float     opacity = 1.f;             // 0 to 1
    Transform xf;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    uint64_t  bytes_key = 0;             // hash of bytes; 0 when there are none
    DrawClip  clip{};
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
