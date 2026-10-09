/*
 * pagemade/drawlist.cpp — see drawlist.h.
 */
#include "drawlist.h"

namespace pagemade {

DrawList build_page(const PageDoc &doc, size_t page, const std::vector<Composition> &comps) {
    DrawList out;
    if (page >= doc.pages.size())
        return out;
    for (const Item &it : doc.pages[page].items) {
        if (const Shape *sh = it.shape()) {
            const Path path = shape_path(it);
            if (!sh->fill.none() && sh->kind != Shape::Kind::Line)
                out.push_back(DrawFill{path, doc.resolve(sh->fill), it.xf});
            if (!sh->stroke.none()) {
                DrawStroke s;
                s.path = path;
                s.color = doc.resolve(sh->stroke.paint);
                s.width = sh->stroke.weight;
                s.dash = dash_pattern(sh->stroke.style, sh->stroke.weight);
                s.cap = line_style_round_caps(sh->stroke.style) ? LineCap::Round : LineCap::Butt;
                s.join = LineJoin::Miter;
                s.xf = it.xf;
                out.push_back(std::move(s));
            }
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
            d.xf = it.xf;
            out.push_back(d);
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
                    out.push_back(DrawGlyphs{&r, it.xf});
    }
    return out;
}

} // namespace pagemade
