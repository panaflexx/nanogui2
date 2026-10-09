/*
 * pagemade/geometry.h — shape outlines, flattening and dashing.
 *
 * Paths are lists of the composer's PathCmd (move, line, quadratic, cubic,
 * close), the same form glyph outlines take, in item space. Everything here
 * is pure so the NanoVG, PDF and SVG backends and the tests share it.
 */
#pragma once

#include "page.h"

#include <vector>

namespace pagemade {

using Path = std::vector<PathCmd>;

struct Polyline {
    std::vector<Point> pts;
    bool closed = false;
};

struct Bounds {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    void add(Point p);
    void add(const Bounds &b);
    bool empty() const { return x0 > x1 || y0 > y1; }
    bool contains(const Bounds &b) const {
        return b.x0 >= x0 && b.x1 <= x1 && b.y0 >= y0 && b.y1 <= y1;
    }
};

/* Rounded corners are quarter ellipses of `radius`, clamped to the box. */
Path rect_path(float x, float y, float w, float h, float radius = 0.f);
Path ellipse_path(float cx, float cy, float rx, float ry);
/* A regular polygon (or star) inscribed in the box, first vertex at the top. */
Path polygon_path(float w, float h, int sides, float star_inset);
/* The outline of a shape item in its own space; empty for text frames. */
Path shape_path(const Item &it);

/* The item's box corners on the page (a line's two ends). */
Bounds item_bounds(const Item &it);
Bounds path_bounds(const Path &p);

/* Curves become line segments no farther than `tolerance` from the curve. */
std::vector<Polyline> flatten(const Path &p, float tolerance);

/* Cut polylines into dashes with an on/off pattern (lengths in the same
 * units). A zero-length "on" gives a one-point polyline: a dot, which a
 * round cap turns into a circle. */
std::vector<Polyline> dash(const std::vector<Polyline> &lines, const std::vector<float> &pattern,
                           float offset = 0.f);

/* On/off lengths in points for a line style at a weight; empty for solid.
 * Dots and dash-dot use round caps (see line_style_round_caps). */
std::vector<float> dash_pattern(LineStyle style, float weight);
bool line_style_round_caps(LineStyle style);

} // namespace pagemade
