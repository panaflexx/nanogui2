/*
 * pagemade/drawlist.cpp — see drawlist.h.
 */
#include "drawlist.h"
#include "vector.h"

#include <algorithm>

namespace pagemade {

namespace {

bool keep_item(const std::vector<ItemId> *which, bool only, ItemId id) {
    if (!which)
        return true;
    for (ItemId x : *which)
        if (x == id)
            return only;
    return !only;
}

/* A stroke along `path`. A compound style is its bands, each a solid
 * stroke along the outline moved sideways, so backends never see it. */
void add_stroke(DrawList &out, const PageDoc &doc, const Path &path, const Stroke &st,
                const Transform &xf) {
    DrawStroke s;
    s.color = doc.resolve(st.paint);
    s.join = LineJoin::Miter;
    s.xf = xf;
    const std::vector<StrokeBand> bands = stroke_bands(st.style, st.weight);
    if (bands.empty()) {
        s.path = path;
        s.width = st.weight;
        s.dash = dash_pattern(st.style, st.weight);
        s.cap = line_style_round_caps(st.style) ? LineCap::Round : LineCap::Butt;
        out.push_back(std::move(s));
        return;
    }
    /* Fine enough to print, and to stay smooth on screen at high zoom */
    const std::vector<Polyline> outline = flatten(path, 0.02f);
    for (const StrokeBand &b : bands) {
        DrawStroke band = s;
        band.path = polyline_path(offset_polylines(outline, b.offset));
        band.width = b.width;
        out.push_back(std::move(band));
    }
}

} // namespace

DrawList build_page(const PageDoc &doc, size_t page, const std::vector<Composition> &comps,
                    const std::vector<ItemId> *which, bool only, const VectorStore *vectors) {
    DrawList out;
    if (page >= doc.pages.size())
        return out;
    for (const Item &it : doc.pages[page].items) {
        if (!keep_item(which, only, it.id) || !doc.item_visible(it))
            continue;                    // a hidden layer is neither shown nor printed
        if (const Shape *sh = it.shape()) {
            const Path path = shape_path(it);
            if (!sh->fill.none() && sh->kind != Shape::Kind::Line)
                out.push_back(DrawFill{path, doc.resolve(sh->fill), it.xf});
            if (!sh->stroke.none())
                add_stroke(out, doc, path, sh->stroke, it.xf);
            continue;
        }
        if (const PlacedImage *im = it.image()) {
            DrawImage d;
            d.asset = im->asset;
            d.x = im->x;
            d.y = im->y;
            d.w = im->w;
            d.h = im->h;
            d.clip_w = it.w;
            d.clip_h = it.h;
            d.opacity = std::clamp(im->opacity, 0.f, 100.f) / 100.f;
            d.xf = it.xf;
            out.push_back(d);
            if (!im->stroke.none())      // the border, over the picture
                add_stroke(out, doc, rect_path(0, 0, it.w, it.h), im->stroke, it.xf);
            continue;
        }
        if (const PlacedVector *pv = it.vector()) {
            if (vectors)
                if (const VectorScene *scene = vectors->scene(pv->asset))
                    append_placed_vector(out, doc, it, *scene);
            if (!pv->stroke.none())      // the frame's border, not clipped with the graphic
                add_stroke(out, doc, rect_path(0, 0, it.w, it.h), pv->stroke, it.xf);
            continue;
        }
        size_t ti = 0;
        const StoryEntry *se = doc.story_of(it.id, &ti);
        const size_t si = se ? doc.story_index(se->id) : SIZE_MAX;
        if (si >= comps.size())
            continue;
        for (const ComposedLine &l : comps[si].lines)
            if (l.frame == ti)
                for (const GlyphRun &r : l.runs)
                    out.push_back(DrawGlyphs{&r, it.xf, doc.text_color(r.swatch, r.tint, r.color)});
    }
    return out;
}

} // namespace pagemade
