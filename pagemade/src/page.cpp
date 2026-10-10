/*
 * pagemade/page.cpp — see page.h.
 */
#include "page.h"

#include <algorithm>
#include <cmath>

namespace pagemade {

/* ---- Transform ---------------------------------------------------------- */

Transform Transform::rotate(float r) {
    const float cs = std::cos(r), sn = std::sin(r);
    return {cs, sn, -sn, cs, 0, 0};
}

Transform Transform::rotate_about(const Transform &t, Point c, float r) {
    return translate(c.x, c.y) * rotate(r) * translate(-c.x, -c.y) * t;
}

Transform Transform::operator*(const Transform &o) const {
    return {a * o.a + c * o.b,     b * o.a + d * o.b,
            a * o.c + c * o.d,     b * o.c + d * o.d,
            a * o.e + c * o.f + e, b * o.e + d * o.f + f};
}

Transform Transform::inverse() const {
    const float det = a * d - b * c;
    if (std::fabs(det) < 1e-12f)
        return {};
    const float id = 1.f / det;
    return {d * id, -b * id, -c * id, a * id,
            (c * f - d * e) * id, (b * e - a * f) * id};
}

float Transform::rotation() const {
    return std::atan2(b, a);
}

/* ---- Swatches ----------------------------------------------------------- */

std::vector<Swatch> default_swatches() {
    return {
        {kPaper,        "[Paper]",        {1, 1, 1, 1}},
        {kBlack,        "[Black]",        {0, 0, 0, 1}},
        {kRegistration, "[Registration]", {0, 0, 0, 1}},
        {4, "Blue",    {0, 0, 1, 1}},
        {5, "Cyan",    {0, 1, 1, 1}},
        {6, "Green",   {0, 1, 0, 1}},
        {7, "Magenta", {1, 0, 1, 1}},
        {8, "Red",     {1, 0, 0, 1}},
        {9, "Yellow",  {1, 1, 0, 1}},
    };
}

/* ---- Paragraph styles --------------------------------------------------- */

std::vector<StyleDef> default_styles() {
    auto def = [](const char *name, const char *family, float size, float leading,
                  auto &&para, bool bold = false, bool italic = false) {
        StyleDef d;
        d.name = name;
        d.para.name = name;
        para(d.para);
        d.type.family = family;
        d.type.size = size;
        d.type.leading = leading;
        d.type.bold = bold;
        d.type.italic = italic;
        return d;
    };
    return {
        def("Normal", "Serif", 12, 0, [](ParaStyle &) {}),
        def("Body text", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.align = Align::Justify;
            p.first_indent = 12;
        }),
        def("Body first", "Serif", 10.5f, 13, [](ParaStyle &p) { p.align = Align::Justify; }),
        def("Drop cap", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.align = Align::Justify;
            p.drop_lines = 3;
        }),
        def("Headline", "Display", 34, 36, [](ParaStyle &p) {
            p.align = Align::Center;
            p.space_after = 6;
            p.hyphenate = false;
        }, true),
        def("Subhead 1", "Sans", 14, 17, [](ParaStyle &p) {
            p.space_before = 12;
            p.space_after = 3;
            p.hyphenate = false;
        }, true),
        def("Subhead 2", "Sans", 11, 13, [](ParaStyle &p) {
            p.space_before = 9;
            p.space_after = 2;
            p.hyphenate = false;
        }, true),
        def("Caption", "Sans", 8.5f, 10.5f, [](ParaStyle &p) { p.space_before = 4; }, false, true),
        def("Hanging indent", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.left_indent = 18;
            p.first_indent = -18;
            p.space_after = 4;
        }),
        def("Pull quote", "Serif", 16, 20, [](ParaStyle &p) {
            p.align = Align::Center;
            p.left_indent = p.right_indent = 18;
            p.space_before = p.space_after = 12;
            p.hyphenate = false;
        }, false, true),
        def("Byline", "Sans", 8.5f, 0, [](ParaStyle &p) {
            p.align = Align::Center;
            p.space_before = 12;
        }, true),
    };
}

const StyleDef *PageDoc::find_style(const std::string &name) const {
    for (const StyleDef &s : styles)
        if (s.name == name)
            return &s;
    return nullptr;
}

StyleDef *PageDoc::find_style(const std::string &name) {
    return const_cast<StyleDef *>(static_cast<const PageDoc *>(this)->find_style(name));
}

const Run *base_run(const Paragraph &p) {
    const Run *best = nullptr;
    for (const Run &r : p.runs)
        if (!best || r.text.size() > best->text.size())
            best = &r;
    return best;
}

namespace {

bool same_tabs(const std::vector<TabStop> &a, const std::vector<TabStop> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].pos != b[i].pos || a[i].align != b[i].align || a[i].leader != b[i].leader)
            return false;
    return true;
}

/* Everything but the name. */
bool same_format(const ParaStyle &a, const ParaStyle &b) {
    return a.align == b.align && a.left_indent == b.left_indent &&
           a.right_indent == b.right_indent && a.first_indent == b.first_indent &&
           a.last_indent == b.last_indent && a.space_before == b.space_before &&
           a.space_after == b.space_after && a.autoleading == b.autoleading &&
           a.line_spacing == b.line_spacing && a.extra_spacing == b.extra_spacing &&
           a.drop_lines == b.drop_lines && a.drop_chars == b.drop_chars &&
           a.drop_scale == b.drop_scale &&
           a.word_min == b.word_min && a.word_desired == b.word_desired &&
           a.word_max == b.word_max && a.letter_min == b.letter_min &&
           a.letter_desired == b.letter_desired && a.letter_max == b.letter_max &&
           same_tabs(a.tabs, b.tabs) && a.default_tab == b.default_tab &&
           a.hyphenate == b.hyphenate && a.hyphen_limit == b.hyphen_limit &&
           a.hyphen_zone == b.hyphen_zone;
}

/* The attributes a style sets on its text. */
bool same_type(const CharStyle &a, const CharStyle &b) {
    return a.family == b.family && a.face == b.face && a.bold == b.bold &&
           a.italic == b.italic && a.caps == b.caps && a.size == b.size &&
           a.leading == b.leading && a.tracking == b.tracking && a.hscale == b.hscale;
}

} // namespace

void apply_style(Paragraph &p, const StyleDef &def) {
    p.style = def.para;
    p.style.name = def.name;
    const Run *base = base_run(p);
    const bool base_bold = base ? base->style.bold : false;
    const bool base_italic = base ? base->style.italic : false;
    for (Run &r : p.runs) {
        const bool em_bold = r.style.bold != base_bold;
        const bool em_italic = r.style.italic != base_italic;
        CharStyle &cs = r.style;
        cs.family = def.type.family;
        cs.face = (em_bold || em_italic) ? std::string() : def.type.face;
        cs.bold = def.type.bold != em_bold;
        cs.italic = def.type.italic != em_italic;
        cs.caps = def.type.caps;
        cs.size = def.type.size;
        cs.leading = def.type.leading;
        cs.tracking = def.type.tracking;
        cs.hscale = def.type.hscale;
    }
}

bool style_overridden(const Paragraph &p, const StyleDef &def) {
    if (!same_format(p.style, def.para))
        return true;
    const Run *base = base_run(p);
    return base && !same_type(base->style, def.type);
}

StyleDef style_from(const std::string &name, const Paragraph &p) {
    StyleDef d;
    d.name = name;
    d.para = p.style;
    d.para.name = name;
    if (const Run *base = base_run(p))
        d.type = base->style;
    /* A style is type, not ornament. */
    d.type.underline = d.type.strike = false;
    d.type.baseline_shift = 0;
    d.type.color = Color();
    return d;
}

const Swatch *PageDoc::find_swatch(SwatchId id) const {
    for (const Swatch &s : swatches)
        if (s.id == id)
            return &s;
    return nullptr;
}

Color PageDoc::resolve(const Paint &p) const {
    const Swatch *s = p.none() ? nullptr : find_swatch(p.swatch);
    if (!s)
        return {0, 0, 0, 0};
    /* A tint is a screen of the ink over paper white. */
    const float t = std::clamp(p.tint, 0.f, 100.f) * 0.01f;
    return {1 - (1 - s->rgb.r) * t, 1 - (1 - s->rgb.g) * t, 1 - (1 - s->rgb.b) * t, s->rgb.a};
}

SwatchId PageDoc::add_swatch(const std::string &name, Color rgb) {
    SwatchId id = kRegistration;
    for (const Swatch &s : swatches)
        id = std::max(id, s.id);
    swatches.push_back({id + 1, name, rgb});
    return id + 1;
}

/* ---- Lookups ------------------------------------------------------------ */

Item *PageDoc::find_item(ItemId id, size_t *page) {
    return const_cast<Item *>(static_cast<const PageDoc *>(this)->find_item(id, page));
}

const Item *PageDoc::find_item(ItemId id, size_t *page) const {
    for (size_t pi = 0; pi < pages.size(); ++pi)
        for (const Item &it : pages[pi].items)
            if (it.id == id) {
                if (page) *page = pi;
                return &it;
            }
    return nullptr;
}

StoryEntry *PageDoc::find_story(StoryId id) {
    return const_cast<StoryEntry *>(static_cast<const PageDoc *>(this)->find_story(id));
}

const StoryEntry *PageDoc::find_story(StoryId id) const {
    for (const StoryEntry &s : stories)
        if (s.id == id)
            return &s;
    return nullptr;
}

size_t PageDoc::story_index(StoryId id) const {
    for (size_t i = 0; i < stories.size(); ++i)
        if (stories[i].id == id)
            return i;
    return SIZE_MAX;
}

const StoryEntry *PageDoc::story_of(ItemId frame, size_t *thread_index) const {
    for (const StoryEntry &s : stories)
        for (size_t i = 0; i < s.thread.size(); ++i)
            if (s.thread[i] == frame) {
                if (thread_index) *thread_index = i;
                return &s;
            }
    return nullptr;
}

StoryEntry *PageDoc::story_of(ItemId frame, size_t *thread_index) {
    return const_cast<StoryEntry *>(static_cast<const PageDoc *>(this)->story_of(frame, thread_index));
}

std::vector<Frame> PageDoc::thread_frames(const StoryEntry &s) const {
    std::vector<Frame> out;
    for (ItemId id : s.thread) {
        const Item *it = find_item(id);
        out.push_back(it ? Frame{0, 0, it->w, it->h} : Frame{});
    }
    return out;
}

/* ---- Edits -------------------------------------------------------------- */

PlacedImage scale_placement(PlacedImage im, float old_w, float old_h, float new_w, float new_h) {
    if (old_w != 0.f) {
        float s = new_w / old_w;
        im.x *= s;
        im.w *= s;
    }
    if (old_h != 0.f) {
        float s = new_h / old_h;
        im.y *= s;
        im.h *= s;
    }
    return im;
}

PlacedImage crop_placement(PlacedImage im, float left, float top) {
    im.x -= left;
    im.y -= top;
    return im;
}

const ImageAsset *PageDoc::find_image(uint32_t id) const {
    for (const ImageAsset &a : images)
        if (a.id == id)
            return &a;
    return nullptr;
}

ImageAsset *PageDoc::find_image(uint32_t id) {
    return const_cast<ImageAsset *>(static_cast<const PageDoc *>(this)->find_image(id));
}

uint32_t PageDoc::add_image(ImageAsset asset) {
    if (asset.id == 0)
        asset.id = next_asset++;
    else if (asset.id >= next_asset)
        next_asset = asset.id + 1;
    images.push_back(std::move(asset));
    return images.back().id;
}

bool PageDoc::image_placed(uint32_t id) const {
    for (const Page &p : pages)
        for (const Item &it : p.items)
            if (const PlacedImage *im = it.image())
                if (im->asset == id)
                    return true;
    return false;
}

ItemId PageDoc::add_item(size_t page, Item item) {
    if (page >= pages.size())
        pages.resize(page + 1);
    item.id = next_id++;
    pages[page].items.push_back(std::move(item));
    return pages[page].items.back().id;
}

StoryId PageDoc::add_story(Story story) {
    StoryEntry e;
    e.id = next_id++;
    e.story = std::move(story);
    stories.push_back(std::move(e));
    return stories.back().id;
}

ItemId PageDoc::add_text_frame(size_t page, StoryId story, float w, float h,
                               const Transform &xf, size_t thread_index) {
    Item it;
    it.w = w;
    it.h = h;
    it.xf = xf;
    it.content = TextFrame{};
    ItemId id = add_item(page, std::move(it));
    if (StoryEntry *s = find_story(story)) {
        thread_index = std::min(thread_index, s->thread.size());
        s->thread.insert(s->thread.begin() + (long) thread_index, id);
    }
    return id;
}

void PageDoc::remove_item(ItemId id) {
    uint32_t asset = 0;
    if (const Item *it = find_item(id))
        if (const PlacedImage *im = it->image())
            asset = im->asset;
    for (Page &p : pages)
        p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                     [&](const Item &it) { return it.id == id; }),
                      p.items.end());
    for (StoryEntry &s : stories)
        s.thread.erase(std::remove(s.thread.begin(), s.thread.end(), id), s.thread.end());
    stories.erase(std::remove_if(stories.begin(), stories.end(),
                                 [](const StoryEntry &s) { return s.thread.empty(); }),
                  stories.end());
    if (asset && !image_placed(asset))
        images.erase(std::remove_if(images.begin(), images.end(),
                                    [&](const ImageAsset &a) { return a.id == asset; }),
                     images.end());
}

void PageDoc::bring_to_front(ItemId id) {
    for (Page &p : pages) {
        auto it = std::find_if(p.items.begin(), p.items.end(),
                               [&](const Item &i) { return i.id == id; });
        if (it != p.items.end())
            std::rotate(it, it + 1, p.items.end());
    }
}

void PageDoc::send_to_back(ItemId id) {
    for (Page &p : pages) {
        auto it = std::find_if(p.items.begin(), p.items.end(),
                               [&](const Item &i) { return i.id == id; });
        if (it != p.items.end())
            std::rotate(p.items.begin(), it, it + 1);
    }
}

void PageDoc::restack(const std::vector<ItemId> &ids, int dir) {
    if (dir == 0)
        return;
    auto selected = [&](ItemId id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    for (Page &p : pages) {
        if (p.items.size() < 2)
            continue;
        if (dir > 0) {
            /* Walk from the front so a selected run swaps once with the
             * item above it and then stays together. */
            for (size_t i = p.items.size() - 1; i-- > 0;)
                if (selected(p.items[i].id) && !selected(p.items[i + 1].id))
                    std::swap(p.items[i], p.items[i + 1]);
        } else {
            for (size_t i = 1; i < p.items.size(); ++i)
                if (selected(p.items[i].id) && !selected(p.items[i - 1].id))
                    std::swap(p.items[i], p.items[i - 1]);
        }
    }
}

size_t PageDoc::insert_page(size_t index) {
    if (index > pages.size())
        index = pages.size();
    pages.insert(pages.begin() + (long) index, Page{});
    return index;
}

bool PageDoc::remove_page(size_t index) {
    if (index >= pages.size() || pages.size() <= 1)
        return false;
    std::vector<ItemId> ids;
    for (const Item &it : pages[index].items)
        ids.push_back(it.id);
    for (ItemId id : ids)
        remove_item(id);
    if (index < pages.size())
        pages.erase(pages.begin() + (long) index);
    return true;
}

void PageDoc::move_page(size_t from, size_t to) {
    if (from >= pages.size() || to >= pages.size() || from == to)
        return;
    if (to > from)
        std::rotate(pages.begin() + (long) from, pages.begin() + (long) from + 1,
                    pages.begin() + (long) to + 1);
    else
        std::rotate(pages.begin() + (long) to, pages.begin() + (long) from,
                    pages.begin() + (long) from + 1);
}

PageDoc new_publication(PageSetup setup, int page_count) {
    PageDoc doc;
    doc.setup = setup;
    if (page_count < 1)
        page_count = 1;
    if (page_count > 999)
        page_count = 999;
    for (int i = 1; i < page_count; ++i)
        doc.insert_page(doc.pages.size());
    return doc;
}

void arrange_pages(const PageSetup &setup, size_t page_count, size_t current,
                   PageView view, std::vector<PageSlot> &slots,
                   float &width, float &height) {
    slots.clear();
    const float pw = setup.width > 1.f ? setup.width : 1.f;
    const float ph = setup.height > 1.f ? setup.height : 1.f;
    const float gap = kPageGap;
    if (page_count == 0) {
        width = pw;
        height = ph;
        return;
    }
    if (current >= page_count)
        current = page_count - 1;

    if (view == PageView::One) {
        slots.push_back({current, 0.f, 0.f});
        width = pw;
        height = ph;
        return;
    }
    if (view == PageView::Stack) {
        for (size_t i = 0; i < page_count; ++i)
            slots.push_back({i, 0.f, i * (ph + gap)});
        width = pw;
        height = page_count * ph + (page_count - 1) * gap;
        return;
    }

    size_t first = 0, n = 1;
    if (!setup.facing) {
        first = current - (current % 2);
        n = std::min<size_t>(2, page_count - first);
    } else if (current == 0) {
        first = 0;
        n = 1;
    } else {
        first = 1 + ((current - 1) / 2) * 2;
        n = std::min<size_t>(2, page_count - first);
    }
    const bool pair = page_count > 1;
    width = pair ? pw * 2.f + gap : pw;
    height = ph;
    if (n == 2) {
        slots.push_back({first, 0.f, 0.f});
        slots.push_back({first + 1, pw + gap, 0.f});
    } else if (setup.facing && first == 0)
        slots.push_back({first, pair ? pw + gap : 0.f, 0.f});
    else
        slots.push_back({first, 0.f, 0.f});
}

} // namespace pagemade
