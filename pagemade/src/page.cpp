/*
 * pagemade/page.cpp — see page.h.
 */
#include "page.h"

#include <algorithm>
#include <map>
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
        /* Body text keeps two lines together at a block break; headings
         * stay with what follows them. */
        def("Body text", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.align = Align::Justify;
            p.first_indent = 12;
            p.orphans = p.widows = 2;
        }),
        def("Body first", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.align = Align::Justify;
            p.orphans = p.widows = 2;
        }),
        def("Drop cap", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.align = Align::Justify;
            p.drop_lines = 3;
            p.orphans = p.widows = 2;
        }),
        def("Headline", "Display", 34, 36, [](ParaStyle &p) {
            p.align = Align::Center;
            p.space_after = 6;
            p.hyphenate = false;
            p.keep_with_next = p.keep_together = true;
        }, true),
        def("Subhead 1", "Sans", 14, 17, [](ParaStyle &p) {
            p.space_before = 12;
            p.space_after = 3;
            p.hyphenate = false;
            p.keep_with_next = p.keep_together = true;
        }, true),
        def("Subhead 2", "Sans", 11, 13, [](ParaStyle &p) {
            p.space_before = 9;
            p.space_after = 2;
            p.hyphenate = false;
            p.keep_with_next = p.keep_together = true;
        }, true),
        def("Caption", "Sans", 8.5f, 10.5f, [](ParaStyle &p) { p.space_before = 4; }, false, true),
        def("Bulleted list", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.left_indent = 18;
            p.first_indent = -18;
            p.space_after = 3;
            p.list = ListKind::Bullet;
        }),
        def("Numbered list", "Serif", 10.5f, 13, [](ParaStyle &p) {
            p.left_indent = 18;
            p.first_indent = -18;
            p.space_after = 3;
            p.list = ListKind::Number;
        }),
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

/* ---- Character styles ------------------------------------------------------ */

const char *char_field_name(int bit) {
    static const char *names[] = {"family", "face", "bold", "italic", "size", "leading",
                                  "color", "underline", "strike", "caps", "tracking",
                                  "hscale", "baseline", "kerning", "ligatures", "no_break"};
    return bit >= 0 && bit < (int) (sizeof names / sizeof names[0]) ? names[bit] : "";
}

std::vector<CharStyleDef> default_char_styles() {
    CharStyleDef em{"Emphasis", {}, kCharItalic};
    em.type.italic = true;
    CharStyleDef strong{"Strong", {}, kCharBold};
    strong.type.bold = true;
    CharStyleDef sc{"Small caps", {}, kCharCaps | kCharTracking};
    sc.type.caps = Caps::Small;
    sc.type.tracking = 20;
    CharStyleDef accent{"Accent", {}, kCharColor | kCharBold};
    accent.type.swatch = 8;                // Red
    accent.type.bold = true;
    for (CharStyleDef *d : {&em, &strong, &sc, &accent})
        d->type.char_style = d->name;
    return {em, strong, sc, accent};
}

void apply_char_style(CharStyle &cs, const CharStyleDef &def) {
    const CharStyle &t = def.type;
    const uint32_t f = def.fields;
    if (f & kCharFamily) cs.family = t.family;
    if (f & kCharFace) cs.face = t.face;
    else if (f & (kCharBold | kCharItalic)) cs.face.clear();
    if (f & kCharBold) cs.bold = t.bold;
    if (f & kCharItalic) cs.italic = t.italic;
    if (f & kCharSize) cs.size = t.size;
    if (f & kCharLeading) cs.leading = t.leading;
    if (f & kCharColor) {
        cs.color = t.color;
        cs.swatch = t.swatch;
        cs.tint = t.tint;
    }
    if (f & kCharUnderline) cs.underline = t.underline;
    if (f & kCharStrike) cs.strike = t.strike;
    if (f & kCharCaps) cs.caps = t.caps;
    if (f & kCharTracking) cs.tracking = t.tracking;
    if (f & kCharHScale) cs.hscale = t.hscale;
    if (f & kCharBaseline) cs.baseline_shift = t.baseline_shift;
    if (f & kCharKerning) cs.kerning = t.kerning;
    if (f & kCharLigatures) cs.ligatures = t.ligatures;
    if (f & kCharNoBreak) cs.no_break = t.no_break;
    cs.char_style = def.name;
}

namespace {

/* The fields in which two styles differ. */
uint32_t char_differences(const CharStyle &a, const CharStyle &b) {
    uint32_t d = 0;
    if (a.family != b.family) d |= kCharFamily;
    if (a.face != b.face) d |= kCharFace;
    if (a.bold != b.bold) d |= kCharBold;
    if (a.italic != b.italic) d |= kCharItalic;
    if (a.size != b.size) d |= kCharSize;
    if (a.leading != b.leading) d |= kCharLeading;
    if (!(a.color == b.color) || a.swatch != b.swatch || a.tint != b.tint) d |= kCharColor;
    if (a.underline != b.underline) d |= kCharUnderline;
    if (a.strike != b.strike) d |= kCharStrike;
    if (a.caps != b.caps) d |= kCharCaps;
    if (a.tracking != b.tracking) d |= kCharTracking;
    if (a.hscale != b.hscale) d |= kCharHScale;
    if (a.baseline_shift != b.baseline_shift) d |= kCharBaseline;
    if (a.kerning != b.kerning) d |= kCharKerning;
    if (a.ligatures != b.ligatures) d |= kCharLigatures;
    if (a.no_break != b.no_break) d |= kCharNoBreak;
    return d;
}

} // namespace

bool char_style_overridden(const CharStyle &cs, const CharStyleDef &def) {
    /* A face cleared for bold or italic is the style's doing, not an override. */
    uint32_t d = char_differences(cs, def.type) & def.fields;
    if (!(def.fields & kCharFace))
        d &= ~kCharFace;
    return d != 0;
}

CharStyleDef char_style_from(const std::string &name, const CharStyle &cs, const CharStyle &base) {
    CharStyleDef d;
    d.name = name;
    d.type = cs;
    d.type.char_style = name;
    d.fields = char_differences(cs, base);
    if (d.fields == 0)
        d.fields = kCharFamily | kCharFace | kCharBold | kCharItalic | kCharSize | kCharColor;
    return d;
}

const CharStyleDef *PageDoc::find_char_style(const std::string &name) const {
    for (const CharStyleDef &s : char_styles)
        if (s.name == name)
            return &s;
    return nullptr;
}

CharStyleDef *PageDoc::find_char_style(const std::string &name) {
    return const_cast<CharStyleDef *>(static_cast<const PageDoc *>(this)->find_char_style(name));
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
           a.drop_scale == b.drop_scale && a.keep_together == b.keep_together &&
           a.keep_with_next == b.keep_with_next && a.start_new_frame == b.start_new_frame &&
           a.orphans == b.orphans && a.widows == b.widows && a.list == b.list &&
           a.bullet == b.bullet && a.number_format == b.number_format &&
           a.number_suffix == b.number_suffix && a.list_start == b.list_start &&
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

void apply_style(Paragraph &p, const StyleDef &def, const std::vector<CharStyleDef> &char_styles) {
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
        /* A run in a character style keeps what that style sets. */
        if (!cs.char_style.empty())
            for (const CharStyleDef &c : char_styles)
                if (c.name == cs.char_style)
                    apply_char_style(cs, c);
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
    d.type.swatch = 0;
    d.type.tint = 100.f;
    d.type.char_style.clear();
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

Color PageDoc::text_color(uint32_t swatch, float tint, const Color &rgb) const {
    if (swatch == kNoPaint || !find_swatch(swatch))
        return rgb;
    return resolve(Paint{swatch, tint});
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
        size_t pg = 0;
        const Item *it = find_item(id, &pg);
        Frame f;
        if (it) {
            f.w = it->w;
            f.h = it->h;
            f.page = (int) pg + 1;
            f.pages = (int) pages.size();
        }
        out.push_back(f);
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

bool PageDoc::transfer_item(ItemId id, size_t page) {
    if (page >= pages.size())
        return false;
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        std::vector<Item> &items = pages[pi].items;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].id != id)
                continue;
            if (pi == page)
                return false;
            Item moved = std::move(items[i]);
            items.erase(items.begin() + (long) i);
            pages[page].items.push_back(std::move(moved));
            sort_by_layer();             // the top of its own layer there
            return true;
        }
    }
    return false;
}

ItemId PageDoc::add_item(size_t page, Item item) {
    if (page >= pages.size())
        pages.resize(page + 1);
    item.id = next_id++;
    if (!find_layer(item.layer))
        item.layer = find_layer(active_layer) ? active_layer : layer_of(item);
    /* On top of its own layer: after the last item at or below it */
    std::vector<Item> &items = pages[page].items;
    const size_t li = layer_index(item.layer);
    size_t at = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (layer_index(layer_of(items[i])) <= li)
            at = i + 1;
    const ItemId id = item.id;
    items.insert(items.begin() + (long) at, std::move(item));
    return id;
}

/* ---- Layers -------------------------------------------------------------- */

const Layer *PageDoc::find_layer(LayerId id) const {
    for (const Layer &l : layers)
        if (l.id == id)
            return &l;
    return nullptr;
}

Layer *PageDoc::find_layer(LayerId id) {
    for (Layer &l : layers)
        if (l.id == id)
            return &l;
    return nullptr;
}

size_t PageDoc::layer_index(LayerId id) const {
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].id == id)
            return i;
    return SIZE_MAX;
}

LayerId PageDoc::layer_of(const Item &it) const {
    if (find_layer(it.layer))
        return it.layer;
    return layers.empty() ? 0 : layers.front().id;
}

bool PageDoc::item_visible(const Item &it) const {
    const Layer *l = find_layer(layer_of(it));
    return !l || l->visible;
}

bool PageDoc::item_editable(const Item &it) const {
    const Layer *l = find_layer(layer_of(it));
    return !l || (l->visible && !l->locked);
}

bool PageDoc::active_layer_editable() const {
    const Layer *l = find_layer(active_layer);
    return !l || (l->visible && !l->locked);
}

LayerId PageDoc::add_layer(const std::string &name) {
    if (layers.size() >= kMaxLayers)
        return 0;
    Layer l;
    l.id = next_layer++;
    l.name = name;
    const size_t at = layer_index(active_layer);
    layers.insert(at == SIZE_MAX ? layers.end() : layers.begin() + (long) at + 1, l);
    active_layer = l.id;
    return l.id;
}

bool PageDoc::remove_layer(LayerId id) {
    const size_t i = layer_index(id);
    if (i == SIZE_MAX || layers.size() <= 1)
        return false;
    const LayerId to = layers[i > 0 ? i - 1 : 1].id;
    /* The items go to the top of the layer below (the bottom of the one
     * above), keeping their order */
    for (Page &p : pages)
        for (Item &it : p.items)
            if (layer_of(it) == id)
                it.layer = to;
    layers.erase(layers.begin() + (long) i);
    if (active_layer == id)
        active_layer = to;
    sort_by_layer();
    return true;
}

void PageDoc::move_layer(LayerId id, int dir) {
    const size_t i = layer_index(id);
    if (i == SIZE_MAX || dir == 0)
        return;
    const size_t j = dir > 0 ? i + 1 : i - 1;
    if (dir < 0 && i == 0)
        return;
    if (j >= layers.size())
        return;
    std::swap(layers[i], layers[j]);
    sort_by_layer();
}

void PageDoc::set_items_layer(const std::vector<ItemId> &ids, LayerId layer) {
    if (!find_layer(layer))
        return;
    for (Page &p : pages) {
        /* The moved items go on top of their new layer, in their order */
        std::vector<Item> moved, rest;
        for (Item &it : p.items) {
            if (std::find(ids.begin(), ids.end(), it.id) != ids.end()) {
                it.layer = layer;
                moved.push_back(std::move(it));
            } else {
                rest.push_back(std::move(it));
            }
        }
        if (moved.empty())
            continue;
        rest.insert(rest.end(), std::make_move_iterator(moved.begin()),
                    std::make_move_iterator(moved.end()));
        p.items = std::move(rest);
    }
    sort_by_layer();
}

void PageDoc::sort_by_layer() {
    for (Page &p : pages)
        std::stable_sort(p.items.begin(), p.items.end(), [&](const Item &a, const Item &b) {
            return layer_index(layer_of(a)) < layer_index(layer_of(b));
        });
}

void PageDoc::tidy_layers() {
    /* Drop repeated ids and anything past the limit */
    std::vector<Layer> keep;
    for (const Layer &l : layers) {
        bool dup = l.id == 0;
        for (const Layer &k : keep)
            dup = dup || k.id == l.id;
        if (!dup && keep.size() < kMaxLayers)
            keep.push_back(l);
    }
    layers = std::move(keep);
    if (layers.empty())
        layers.push_back(Layer{1, "Default"});
    for (const Layer &l : layers)
        next_layer = std::max(next_layer, l.id + 1);
    if (!find_layer(active_layer))
        active_layer = layers.back().id;
    for (Page &p : pages)
        for (Item &it : p.items)
            it.layer = layer_of(it);
    sort_by_layer();
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
    for (Page &p : pages)
        p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                     [&](const Item &it) { return it.id == id; }),
                      p.items.end());
    for (StoryEntry &s : stories)
        s.thread.erase(std::remove(s.thread.begin(), s.thread.end(), id), s.thread.end());
    stories.erase(std::remove_if(stories.begin(), stories.end(),
                                 [](const StoryEntry &s) { return s.thread.empty(); }),
                  stories.end());
}

bool PageDoc::remove_image(uint32_t id) {
    if (image_placed(id))
        return false;
    auto it = std::find_if(images.begin(), images.end(),
                           [&](const ImageAsset &a) { return a.id == id; });
    if (it == images.end())
        return false;
    images.erase(it);
    return true;
}

size_t PageDoc::remove_unused_images() {
    size_t n = 0;
    for (auto it = images.begin(); it != images.end(); ) {
        if (!image_placed(it->id)) {
            it = images.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

void PageDoc::bring_to_front(ItemId id) {
    for (Page &p : pages) {
        auto it = std::find_if(p.items.begin(), p.items.end(),
                               [&](const Item &i) { return i.id == id; });
        if (it != p.items.end())
            std::rotate(it, it + 1, p.items.end());
    }
    sort_by_layer();                     // the front of its layer
}

void PageDoc::send_to_back(ItemId id) {
    for (Page &p : pages) {
        auto it = std::find_if(p.items.begin(), p.items.end(),
                               [&](const Item &i) { return i.id == id; });
        if (it != p.items.end())
            std::rotate(p.items.begin(), it, it + 1);
    }
    sort_by_layer();                     // the back of its layer
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
    sort_by_layer();                     // a step never leaves the layer
}

std::vector<ItemId> PageDoc::group_members(ItemId id) const {
    size_t pg = 0;
    const Item *it = find_item(id, &pg);
    if (!it)
        return {};
    if (it->group == 0)
        return {id};
    std::vector<ItemId> out;
    for (const Item &o : pages[pg].items)
        if (o.group == it->group)
            out.push_back(o.id);
    return out;
}

uint32_t PageDoc::group_items(const std::vector<ItemId> &ids) {
    /* The items and whatever they were grouped with, all on one page */
    size_t page = SIZE_MAX;
    std::vector<ItemId> members;
    for (ItemId id : ids) {
        size_t pg = 0;
        if (!find_item(id, &pg))
            continue;
        if (page == SIZE_MAX)
            page = pg;
        else if (pg != page)
            return 0;
        for (ItemId m : group_members(id))
            if (std::find(members.begin(), members.end(), m) == members.end())
                members.push_back(m);
    }
    if (page == SIZE_MAX || members.size() < 2)
        return 0;
    const uint32_t gid = next_id++;
    std::vector<Item> &items = pages[page].items;
    auto in = [&](const Item &it) {
        return std::find(members.begin(), members.end(), it.id) != members.end();
    };
    /* Pull the members together below the topmost one, in stacking order */
    size_t top = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (in(items[i]))
            top = i;
    std::vector<Item> grouped, rest;
    size_t insert_at = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (in(items[i])) {
            grouped.push_back(items[i]);
            grouped.back().group = gid;
        } else {
            rest.push_back(items[i]);
            if (i < top)
                insert_at = rest.size();
        }
    }
    /* One group, one layer: the topmost member's */
    const LayerId layer = layer_of(grouped.back());
    for (Item &it : grouped)
        it.layer = layer;
    rest.insert(rest.begin() + (long) insert_at, grouped.begin(), grouped.end());
    items = std::move(rest);
    sort_by_layer();
    return gid;
}

void PageDoc::ungroup_items(const std::vector<ItemId> &ids) {
    for (ItemId id : ids)
        for (ItemId m : group_members(id))
            if (Item *it = find_item(m))
                it->group = 0;
}

void PageDoc::tidy_groups() {
    /* A group lives on one page: where it is found first */
    std::map<uint32_t, size_t> home, count;
    for (size_t pi = 0; pi < pages.size(); ++pi)
        for (Item &it : pages[pi].items) {
            if (!it.group)
                continue;
            auto h = home.find(it.group);
            if (h == home.end())
                home[it.group] = pi;
            else if (h->second != pi)
                it.group = 0;
            if (it.group)
                ++count[it.group];
        }
    for (Page &p : pages)
        for (Item &it : p.items)
            if (it.group && count[it.group] < 2)
                it.group = 0;
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
