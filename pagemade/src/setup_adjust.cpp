/*
 * pagemade/setup_adjust.cpp — see setup_adjust.h.
 */
#include "setup_adjust.h"

#include "geometry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace pagemade {

namespace {

/* Old -> new along one axis: straight lines between matching guides, and
 * a plain shift past the first and last (the pasteboard keeps its offset). */
struct AxisMap {
    std::vector<std::pair<float, float>> pts;   // (old, new), old increasing

    void add(float from, float to) {
        if (pts.empty() || from > pts.back().first + 1e-3f)
            pts.push_back({from, to});
    }
    float operator()(float v) const {
        if (pts.empty())
            return v;
        if (v <= pts.front().first)
            return v - pts.front().first + pts.front().second;
        for (size_t i = 1; i < pts.size(); ++i)
            if (v <= pts[i].first) {
                const auto &a = pts[i - 1], &b = pts[i];
                const float t = (v - a.first) / (b.first - a.first);
                return a.second + t * (b.second - a.second);
            }
        return v - pts.back().first + pts.back().second;
    }
};

/* The column edges of a page, left to right, inside its margins. */
std::vector<float> column_edges(const PageSetup &s, size_t page) {
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    const int n = std::max(1, s.columns);
    const float live = std::max(1.f, s.width - left - right);
    const float gutter = n > 1 ? std::min(s.gutter, live / (float) (n - 1) * 0.5f) : 0.f;
    const float col = (live - gutter * (float) (n - 1)) / (float) n;
    std::vector<float> e;
    for (int k = 0; k < n; ++k) {
        e.push_back(left + (float) k * (col + gutter));
        e.push_back(left + (float) k * (col + gutter) + col);
    }
    return e;
}

void page_maps(const PageSetup &from, const PageSetup &to, size_t page, AxisMap &mx, AxisMap &my) {
    mx.add(0.f, 0.f);
    const std::vector<float> a = column_edges(from, page), b = column_edges(to, page);
    if (a.size() == b.size()) {
        for (size_t i = 0; i < a.size(); ++i)
            mx.add(a[i], b[i]);
    } else {
        /* Different column counts: the margins still match */
        mx.add(a.front(), b.front());
        mx.add(a.back(), b.back());
    }
    mx.add(from.width, to.width);

    my.add(0.f, 0.f);
    my.add(from.margin_top, to.margin_top);
    my.add(from.height - from.margin_bottom, to.height - to.margin_bottom);
    my.add(from.height, to.height);
}

/* Items that move as one: a group, or a single item. */
std::vector<std::vector<size_t>> units(const Page &p) {
    std::vector<std::vector<size_t>> out;
    std::map<uint32_t, size_t> group;
    for (size_t i = 0; i < p.items.size(); ++i) {
        const uint32_t g = p.items[i].group;
        if (g && group.count(g)) {
            out[group[g]].push_back(i);
            continue;
        }
        if (g)
            group[g] = out.size();
        out.push_back({i});
    }
    return out;
}

/* Place a unit's items in the box `to`, from `from`, stretched by sx and
 * sy along the page axes: as a group is resized. Turned items keep their
 * angle. */
void refit(Page &p, const std::vector<size_t> &unit, const Bounds &from, float nx0, float ny0,
           float sx, float sy, bool scale_strokes) {
    auto stretch = [&](Point v) {
        const float len = std::hypot(v.x, v.y);
        return len > 0.f ? std::hypot(v.x * sx, v.y * sy) / len : 1.f;
    };
    for (size_t i : unit) {
        Item &it = p.items[i];
        const Item o = it;
        it.w = o.w * stretch(o.xf.apply_vector({1.f, 0.f}));
        it.h = o.h * stretch(o.xf.apply_vector({0.f, 1.f}));
        const Point origin = o.xf.apply({0.f, 0.f});
        it.xf.e = nx0 + (origin.x - from.x0) * sx;
        it.xf.f = ny0 + (origin.y - from.y0) * sy;
        if (const PlacedImage *im = o.image())
            it.content = scale_placement(*im, o.w, o.h, it.w, it.h);
        if (scale_strokes) {
            const float k = std::sqrt(sx * sy);
            if (Shape *sh = it.shape()) {
                sh->stroke.weight *= k;
                sh->corner_radius *= k;
            }
            if (PlacedImage *im = it.image())
                im->stroke.weight *= k;
        }
    }
}

bool has_picture(const Page &p, const std::vector<size_t> &unit) {
    for (size_t i : unit)
        if (p.items[i].image())
            return true;
    return false;
}

void map_guides(std::vector<Guide> &gs, const AxisMap &mx, const AxisMap &my) {
    for (Guide &g : gs)
        g.pos = g.vertical ? mx(g.pos) : my(g.pos);
}

} // namespace

void scale_char_style(CharStyle &cs, float s) {
    cs.size *= s;
    if (cs.leading > 0.f)
        cs.leading *= s;
    cs.baseline_shift *= s;
}

void scale_para_style(ParaStyle &ps, float s) {
    ps.left_indent *= s;
    ps.right_indent *= s;
    ps.first_indent *= s;
    ps.last_indent *= s;
    ps.space_before *= s;
    ps.space_after *= s;
    ps.extra_spacing *= s;
    for (TabStop &t : ps.tabs)
        t.pos *= s;
    ps.default_tab *= s;
    ps.hyphen_zone *= s;
}

void change_setup(PageDoc &doc, const PageSetup &to, SetupAdjust how) {
    const PageSetup from = doc.setup;
    doc.setup = to;
    if (how == SetupAdjust::Leave)
        return;

    if (how == SetupAdjust::Scale) {
        const float s = std::min(to.width / std::max(1.f, from.width),
                                 to.height / std::max(1.f, from.height));
        const float cx = (to.width - from.width * s) * 0.5f;
        const float cy = (to.height - from.height * s) * 0.5f;
        AxisMap mx, my;
        mx.add(0.f, cx);
        mx.add(from.width, cx + from.width * s);
        my.add(0.f, cy);
        my.add(from.height, cy + from.height * s);
        /* The old page's corner goes to (cx, cy); everything scales from it */
        Bounds old_page;
        old_page.add(Point{0.f, 0.f});
        old_page.add(Point{from.width, from.height});
        for (Page &p : doc.pages) {
            std::vector<size_t> all(p.items.size());
            for (size_t i = 0; i < all.size(); ++i)
                all[i] = i;
            refit(p, all, old_page, cx, cy, s, s, true);
            map_guides(p.guides, mx, my);
        }
        map_guides(doc.guides, mx, my);
        /* The type, everywhere it is set or defined */
        for (StoryEntry &se : doc.stories)
            for (Paragraph &para : se.story.paragraphs) {
                scale_para_style(para.style, s);
                for (Run &r : para.runs)
                    scale_char_style(r.style, s);
            }
        for (StyleDef &d : doc.styles) {
            scale_para_style(d.para, s);
            scale_char_style(d.type, s);
        }
        for (CharStyleDef &d : doc.char_styles)
            scale_char_style(d.type, s);
        return;
    }

    /* Adjust: page by page, as the margins of facing pages differ */
    for (size_t pi = 0; pi < doc.pages.size(); ++pi) {
        Page &p = doc.pages[pi];
        AxisMap mx, my;
        page_maps(from, to, pi, mx, my);
        for (const std::vector<size_t> &unit : units(p)) {
            Bounds b;
            for (size_t i : unit)
                b.add(item_bounds(p.items[i]));
            if (b.empty())
                continue;
            const float nx0 = mx(b.x0), nx1 = mx(b.x1), ny0 = my(b.y0), ny1 = my(b.y1);
            float sx = b.x1 - b.x0 > 0.01f ? (nx1 - nx0) / (b.x1 - b.x0) : 1.f;
            float sy = b.y1 - b.y0 > 0.01f ? (ny1 - ny0) / (b.y1 - b.y0) : 1.f;
            if (has_picture(p, unit))
                sy = sx;                 // a picture keeps its proportions
            refit(p, unit, b, nx0, ny0, sx, sy, false);
        }
        if (pi == 0)
            map_guides(doc.guides, mx, my);
        map_guides(p.guides, mx, my);
    }
}

} // namespace pagemade
