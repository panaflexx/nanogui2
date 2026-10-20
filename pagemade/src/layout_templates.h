/*
 * pagemade/layout_templates.h — predefined page layouts (the Layouts tab).
 *
 * A layout template is a page design in its own box, in points: empty
 * picture frames to drop photos into, text blocks with their formatting,
 * shapes (rules, mats) and SVG graphics. Its margins say where the content
 * is: the box minus the margins. Placing a template fits that content area
 * into the page's live area (inside the page margins), scaled the same
 * both ways and centered, and the items it makes are grouped so the
 * layout moves as one. Elements may sit in the template's margins; they
 * land the same distance (scaled) outside the live area.
 *
 * A spread template is two pages side by side, split down the middle of
 * its box; `inside` is the margin at the spine. On a facing spread each
 * half goes onto its own page (one group per page: groups don't cross
 * pages). Anywhere else the whole spread is fitted onto the one page.
 *
 * Templates can be turned 90, 180 or 270 degrees clockwise. The layout
 * turns (where each frame sits and its proportions); the content of each
 * frame stays upright, so text reflows into its turned block. A spread
 * turned on its side is no longer a spread.
 *
 * The templates live in a global JSON file, layouts.json beside
 * colors.conf, seeded with the built-in set the first time it is missing.
 * Text sizes are points at the template's own scale: placed smaller or
 * larger, the type scales with the layout.
 */
#pragma once

#include "page.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pagemade {

class VectorStore;

/* One run of a template's text. Unset fields (negative sizes, empty
 * strings, `has_*` false) leave the paragraph style's own type. */
struct LayoutRun {
    std::string text;
    std::string family, face;
    bool  has_bold = false, bold = false;
    bool  has_italic = false, italic = false;
    bool  has_caps = false;
    Caps  caps = Caps::Normal;
    bool  underline = false;
    float size = -1.f, leading = -1.f;
    bool  has_tracking = false;
    float tracking = 0.f;
    std::string swatch;                  // a swatch name of the publication
    float tint = 100.f;
};

/* A paragraph: a named paragraph style of the publication ("Caption",
 * "Headline"; "Normal" when it has none of that name), the type every run
 * starts from (`type`, its `text` unused), and the runs. */
struct LayoutParagraph {
    std::string style = "Normal";
    bool  has_align = false;
    Align align = Align::Left;
    float space_before = -1.f, space_after = -1.f;
    LayoutRun type;
    std::vector<LayoutRun> runs;
};

/* x, y, w, h in the template's box, points, y down. */
struct LayoutRect {
    float x = 0, y = 0, w = 0, h = 0;
};

struct LayoutElement {
    enum class Kind { Photo, Text, Shape, Svg };
    Kind kind = Kind::Photo;
    LayoutRect rect;
    /* Photo: an empty picture frame; its border (`stroke_swatch` empty: none) */
    /* Shape: rectangle, ellipse or line (corner to corner of the rect) */
    Shape::Kind shape = Shape::Kind::Rect;
    float corner_radius = 0.f;
    std::string fill_swatch;             // empty: no fill
    float fill_tint = 100.f;
    std::string stroke_swatch;           // empty: no stroke
    float stroke_tint = 100.f;
    float stroke_weight = 1.f;
    LineStyle stroke_style = LineStyle::Solid;
    /* Text */
    std::vector<LayoutParagraph> paragraphs;
    /* Svg: the markup itself, so the file stands alone. The graphic keeps
     * its proportions, centered in the rect. */
    std::string svg;
};

struct LayoutMargins {
    float top = 54, right = 54, bottom = 54, left = 54;
    float inside = 54;                   // spreads: either side of the spine
};

struct LayoutTemplate {
    std::string name;
    std::string category;                // groups the Category menu
    float width = 612, height = 792;     // the box, points
    bool spread = false;                 // two pages, split at width / 2
    LayoutMargins margins;
    std::vector<LayoutElement> elements; // bottom to top

    size_t photo_count() const;
};

/* `t` turned clockwise by `quarter_turns` (0..3) about its box. */
LayoutTemplate rotate_layout(const LayoutTemplate &t, int quarter_turns);

/* The built-in set: 32 layouts, single pages and spreads, with photos,
 * captions, titles, rules and SVG ornaments. */
std::vector<LayoutTemplate> default_layout_templates();

/* layouts.json <-> templates. Reading skips elements it doesn't know and
 * templates without a box; `error` gets what stopped a broken file. */
std::string layout_templates_json(const std::vector<LayoutTemplate> &templates);
bool layout_templates_from_json(const std::string &json, std::vector<LayoutTemplate> &out,
                                std::string *error = nullptr);

/* $XDG_CONFIG_HOME/pagemade/layouts.json, else ~/.config/pagemade/layouts.json. */
std::string layout_templates_config_path();
/* The file's templates. A missing file is written with the built-in set
 * first; a file that can't be read gives the built-ins (and `error`), and
 * is left alone. */
std::vector<LayoutTemplate> layout_templates_load_or_seed(const std::string &path,
                                                          std::string *error = nullptr);
bool layout_templates_save(const std::vector<LayoutTemplate> &templates, const std::string &path,
                           std::string *error = nullptr);

/* ---- Placing ------------------------------------------------------------ */

/* Where a template lands: for each page it touches, the scale and offset
 * from template box points to that page's points, and the elements
 * (indices) that go there. */
struct LayoutFit {
    struct Part {
        size_t page = 0;
        float scale = 1.f;
        float dx = 0.f, dy = 0.f;        // page = box * scale + (dx, dy)
        std::vector<size_t> elements;
    };
    std::vector<Part> parts;
    bool ok() const { return !parts.empty(); }
};

/* The fit of `t` dropped on `page`. A spread on a facing spread with both
 * pages present covers both; otherwise the one page. */
LayoutFit fit_layout(const PageDoc &doc, const LayoutTemplate &t, size_t page);
/* An element's rectangle on its page. */
LayoutRect layout_rect_on_page(const LayoutFit::Part &part, const LayoutRect &r);

struct LayoutPlacement {
    std::vector<ItemId> items;           // every item made, in stacking order
    std::vector<size_t> pages;           // the pages they went on
    std::vector<std::string> skipped;    // what couldn't be placed (an SVG that won't read)
};

/* Add the template's items to `doc` (the active layer), grouped per page,
 * stories made for its text. SVGs go into `vectors` (skipped when null).
 * The caller records undo and recomposes. */
LayoutPlacement place_layout(PageDoc &doc, VectorStore *vectors, const LayoutTemplate &t,
                             size_t page);

/* A template from items on one page: their rectangles (unturned bounds)
 * relative to the page, the page's margins as the template's. Pictures
 * become empty frames; text keeps its formatting; graphics keep their SVG
 * when `vectors` has the bytes. */
LayoutTemplate layout_from_items(const PageDoc &doc, const VectorStore *vectors, size_t page,
                                 const std::vector<ItemId> &ids, const std::string &name,
                                 const std::string &category);

/* Drag and drop: a template from the Layouts tab. The bytes are its name;
 * DragData::object holds a std::shared_ptr<const LayoutTemplate>, already
 * turned the way the tab shows it. */
constexpr const char *kLayoutTemplateDragType = "application/x-pagemade-layout";

} // namespace pagemade
