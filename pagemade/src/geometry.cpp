/*
 * pagemade/geometry.cpp — see geometry.h.
 */
#include "geometry.h"

#include <algorithm>
#include <cmath>

namespace pagemade {

namespace {

constexpr float kKappa = 0.5522847498f;     // cubic approximation of a quarter circle
constexpr float kPi = 3.14159265358979f;

PathCmd move(float x, float y) { return {PathCmd::Move, 0, 0, 0, 0, x, y}; }
PathCmd line(float x, float y) { return {PathCmd::Line, 0, 0, 0, 0, x, y}; }
PathCmd cubic(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    return {PathCmd::Cubic, c1x, c1y, c2x, c2y, x, y};
}
PathCmd close() { return {PathCmd::Close, 0, 0, 0, 0, 0, 0}; }

float dist(Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); }

} // namespace

void Bounds::add(Point p) {
    x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
}

void Bounds::add(const Bounds &b) {
    if (b.empty())
        return;
    add(Point{b.x0, b.y0});
    add(Point{b.x1, b.y1});
}

/* ---- Outlines ----------------------------------------------------------- */

Path rect_path(float x, float y, float w, float h, float r) {
    r = std::max(0.f, std::min({r, std::fabs(w) * 0.5f, std::fabs(h) * 0.5f}));
    if (r <= 0.f)
        return {move(x, y), line(x + w, y), line(x + w, y + h), line(x, y + h), close()};
    const float k = r * (1 - kKappa);
    const float x1 = x + w, y1 = y + h;
    return {
        move(x + r, y),
        line(x1 - r, y),  cubic(x1 - k, y, x1, y + k, x1, y + r),
        line(x1, y1 - r), cubic(x1, y1 - k, x1 - k, y1, x1 - r, y1),
        line(x + r, y1),  cubic(x + k, y1, x, y1 - k, x, y1 - r),
        line(x, y + r),   cubic(x, y + k, x + k, y, x + r, y),
        close(),
    };
}

Path ellipse_path(float cx, float cy, float rx, float ry) {
    const float kx = rx * kKappa, ky = ry * kKappa;
    return {
        move(cx, cy - ry),
        cubic(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy),
        cubic(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry),
        cubic(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy),
        cubic(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry),
        close(),
    };
}

Path polygon_path(float w, float h, int sides, float star_inset) {
    sides = std::max(3, sides);
    const bool star = star_inset > 0.f;
    const int n = star ? sides * 2 : sides;
    const float inner = 1.f - std::clamp(star_inset, 0.f, 100.f) * 0.01f;
    const float cx = w * 0.5f, cy = h * 0.5f;
    Path p;
    for (int k = 0; k < n; ++k) {
        const float a = -kPi * 0.5f + 2.f * kPi * (float) k / (float) n;
        const float s = (star && (k & 1)) ? inner : 1.f;
        const float x = cx + cx * s * std::cos(a), y = cy + cy * s * std::sin(a);
        p.push_back(k == 0 ? move(x, y) : line(x, y));
    }
    p.push_back(close());
    return p;
}

Path shape_path(const Item &it) {
    const Shape *sh = it.shape();
    if (!sh)
        return {};
    switch (sh->kind) {
    case Shape::Kind::Rect:    return rect_path(0, 0, it.w, it.h, sh->corner_radius);
    case Shape::Kind::Ellipse: return ellipse_path(it.w * 0.5f, it.h * 0.5f, it.w * 0.5f, it.h * 0.5f);
    case Shape::Kind::Line:    return {move(0, 0), line(it.w, it.h)};
    case Shape::Kind::Polygon: return polygon_path(it.w, it.h, sh->sides, sh->star_inset);
    }
    return {};
}

Bounds item_bounds(const Item &it) {
    Bounds b;
    b.add(it.xf.apply({0, 0}));
    b.add(it.xf.apply({it.w, it.h}));
    const Shape *sh = it.shape();
    if (!sh || sh->kind != Shape::Kind::Line) {
        b.add(it.xf.apply({it.w, 0}));
        b.add(it.xf.apply({0, it.h}));
    }
    return b;
}

Bounds path_bounds(const Path &p) {
    Bounds b;
    for (const PathCmd &c : p) {
        if (c.kind == PathCmd::Close)
            continue;
        b.add(Point{c.x, c.y});
        if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic)
            b.add(Point{c.cx1, c.cy1});
        if (c.kind == PathCmd::Cubic)
            b.add(Point{c.cx2, c.cy2});
    }
    return b;
}

/* ---- Flattening --------------------------------------------------------- */

std::vector<Polyline> flatten(const Path &path, float tol) {
    tol = std::max(tol, 1e-4f);
    std::vector<Polyline> out;
    Point cur{0, 0}, start{0, 0};
    auto open_line = [&]() -> Polyline & {
        if (out.empty() || out.back().closed || out.back().pts.empty())
            out.push_back({{cur}, false});
        return out.back();
    };
    for (const PathCmd &c : path) {
        switch (c.kind) {
        case PathCmd::Move:
            cur = start = {c.x, c.y};
            out.push_back({{cur}, false});
            break;
        case PathCmd::Line:
            open_line().pts.push_back({c.x, c.y});
            cur = {c.x, c.y};
            break;
        case PathCmd::Quad:
        case PathCmd::Cubic: {
            /* Wang's formula: enough segments that no point strays more than
             * tol from the curve. */
            const bool cub = c.kind == PathCmd::Cubic;
            const Point p0 = cur, p1{c.cx1, c.cy1};
            const Point p2 = cub ? Point{c.cx2, c.cy2} : Point{c.x, c.y};
            const Point p3{c.x, c.y};
            float dd = std::hypot(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
            if (cub)
                dd = std::max(dd, std::hypot(p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y));
            const float k = cub ? 0.75f : 0.25f;
            const int n = std::clamp((int) std::ceil(std::sqrt(k * dd / tol)), 1, 256);
            Polyline &pl = open_line();
            for (int i = 1; i <= n; ++i) {
                const float t = (float) i / (float) n, u = 1 - t;
                Point q;
                if (cub) {
                    const float a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
                    q = {a * p0.x + b * p1.x + cc * p2.x + d * p3.x,
                         a * p0.y + b * p1.y + cc * p2.y + d * p3.y};
                } else {
                    const float a = u * u, b = 2 * u * t, d = t * t;
                    q = {a * p0.x + b * p1.x + d * p3.x, a * p0.y + b * p1.y + d * p3.y};
                }
                pl.pts.push_back(q);
            }
            cur = p3;
            break;
        }
        case PathCmd::Close:
            if (!out.empty() && !out.back().closed) {
                out.back().closed = true;
                cur = start;
            }
            break;
        }
    }
    return out;
}

/* ---- Dashing ------------------------------------------------------------ */

std::vector<Polyline> dash(const std::vector<Polyline> &lines, const std::vector<float> &pattern,
                           float offset) {
    float period = 0;
    for (float v : pattern)
        period += std::max(0.f, v);
    if (pattern.empty() || period <= 1e-6f)
        return lines;
    std::vector<float> pat = pattern;
    if (pat.size() & 1)
        pat.insert(pat.end(), pattern.begin(), pattern.end());   // odd lists repeat, as in SVG

    std::vector<Polyline> out;
    for (const Polyline &pl : lines) {
        std::vector<Point> pts = pl.pts;
        if (pl.closed && !pts.empty())
            pts.push_back(pts.front());
        if (pts.empty())
            continue;

        /* Skip `offset` into the pattern. */
        size_t idx = 0;
        float rem = std::max(0.f, pat[0]);
        float skip = std::fmod(std::max(0.f, offset), period);
        while (skip > rem) {
            skip -= rem;
            idx = (idx + 1) % pat.size();
            rem = std::max(0.f, pat[idx]);
        }
        rem -= skip;

        Polyline cur;
        if ((idx & 1) == 0)
            cur.pts.push_back(pts[0]);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Point a = pts[i], b = pts[i + 1];
            const float len = dist(a, b);
            float t = 0;
            for (;;) {
                const float step = std::min(rem, len - t);
                t += step;
                rem -= step;
                const Point p = len > 0 ? Point{a.x + (b.x - a.x) * t / len, a.y + (b.y - a.y) * t / len}
                                        : a;
                const bool on = (idx & 1) == 0;
                if (on && step > 0)
                    cur.pts.push_back(p);
                if (rem > 1e-6f)
                    break;                       // the segment ends inside this dash or gap
                if (on)
                    out.push_back(std::move(cur));
                cur = Polyline();
                idx = (idx + 1) % pat.size();
                rem = std::max(0.f, pat[idx]);
                if ((idx & 1) == 0)
                    cur.pts.push_back(p);
                if (len - t <= 1e-6f && rem > 1e-6f)
                    break;
            }
        }
        /* A dash cut off by the end of the line. (Dots, zero-length on
         * dashes, were emitted in the loop; a lone point here is a dash
         * that would start exactly at the end.) */
        if ((idx & 1) == 0 && cur.pts.size() >= 2)
            out.push_back(std::move(cur));
    }
    return out;
}

std::vector<float> dash_pattern(LineStyle style, float w) {
    w = std::max(w, 0.25f);
    switch (style) {
    case LineStyle::Solid:   return {};
    case LineStyle::Dashed:  return {std::max(3.f, 4 * w), std::max(2.f, 2.5f * w)};
    case LineStyle::Dotted:  return {0.f, std::max(1.5f, 2.f * w)};
    case LineStyle::DashDot: return {std::max(3.f, 4 * w), std::max(1.5f, 2.f * w),
                                     0.f, std::max(1.5f, 2.f * w)};
    }
    return {};
}

bool line_style_round_caps(LineStyle style) {
    return style == LineStyle::Dotted || style == LineStyle::DashDot;
}

} // namespace pagemade
