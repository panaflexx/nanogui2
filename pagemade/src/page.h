/*
 * pagemade/page.h — the document: stories, and pages holding items in
 * stacking order.
 *
 * Every item has a stable id, a size and a transform. Its geometry lives in
 * its own space, the rectangle (0, 0)–(w, h); the transform maps that space
 * onto the page (points, y down). Moving, rotating and skewing change only
 * the transform. Text frames are composed in their own space as well, so a
 * rotated text block breaks its lines exactly as an upright one would.
 *
 * Stories live beside the pages, not inside them. A story's thread lists
 * its text frames by id in reading order, so it can run through frames
 * anywhere in the stacking order and across pages. Every text frame is in
 * exactly one thread.
 *
 * Pages stay in document order. A hidden page is still edited here; print
 * and PDF export leave it out.
 *
 * Pictures are assets of the publication, not of one page. An item only
 * names the asset; the file bytes live in an ImageStore (image.h), so a
 * snapshot of this document stays the metadata and the placements. A later
 * asset library moves those records between documents.
 *
 * The whole model is plain values: copying a PageDoc is a snapshot, which
 * is how undo works.
 */
#pragma once

#include "composer/composer.h"
#include "image.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace pagemade {

using ItemId = uint32_t;     // 0 = none
using StoryId = uint32_t;

struct Point {
    float x = 0, y = 0;
};

/* x' = a x + c y + e,  y' = b x + d y + f  (the PDF / SVG / NanoVG layout). */
struct Transform {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    static Transform translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
    static Transform rotate(float radians);
    static Transform scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
    /* `t` turned by `radians` about the page point c. */
    static Transform rotate_about(const Transform &t, Point c, float radians);

    /* (A * B)(p) = A(B(p)): B is applied first. */
    Transform operator*(const Transform &o) const;
    Transform inverse() const;
    Point apply(Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Point apply_vector(Point v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    float rotation() const;      // radians
    bool  operator==(const Transform &o) const {
        return a == o.a && b == o.b && c == o.c && d == o.d && e == o.e && f == o.f;
    }
};

/* Document Setup: page size, margins and column guides, in points. */
struct PageSetup {
    float width = 612.f, height = 792.f;    // US Letter
    float margin_top = 54.f, margin_bottom = 54.f;
    float margin_inside = 54.f, margin_outside = 54.f;
    int   columns = 2;
    float gutter = 18.f;
};

/* Colors palette entries. Paints refer to swatches by id, so changing a
 * swatch recolors everything that uses it. Ids 1-3 are PageMaker's built-in
 * [Paper], [Black] and [Registration]; 0 means no paint ([None]). Swatches
 * carry an RGB screen color for now; CMYK and spot inks come with PDF. */
using SwatchId = uint32_t;
constexpr SwatchId kNoPaint = 0, kPaper = 1, kBlack = 2, kRegistration = 3;

struct Swatch {
    SwatchId    id = 0;
    std::string name;
    Color       rgb;
};

/* A swatch at a tint: PageMaker's screens, from 100% toward paper white. */
struct Paint {
    SwatchId swatch = kNoPaint;
    float    tint = 100.f;
    bool none() const { return swatch == kNoPaint; }
};

enum class LineStyle { Solid, Dashed, Dotted, DashDot };

struct Stroke {
    Paint     paint{kBlack, 100.f};
    float     weight = 1.f;          // points; 0 = no stroke
    LineStyle style = LineStyle::Solid;
    bool none() const { return weight <= 0.f || paint.none(); }
};

/* A text block. Its story is the one whose thread lists it. Per-block
 * text options (inset, vertical alignment, columns) will live here. */
struct TextFrame {};

/* PageMaker's drawn elements, filling the item's box. A line runs from
 * (0, 0) to (w, h) in item space, so either may be negative. A polygon is
 * inscribed in the box; with a star inset, every other vertex is pulled
 * that percentage of the way toward the center. As in PageMaker, a new
 * shape has no fill and a 1 pt black stroke. */
struct Shape {
    enum class Kind { Rect, Ellipse, Line, Polygon };
    Kind   kind = Kind::Rect;
    float  corner_radius = 0.f;      // rectangles
    int    sides = 6;                // polygons, 3 and up
    float  star_inset = 0.f;         // polygons, percent
    Paint  fill;
    Stroke stroke;
};

/* A picture from the publication's asset table. The frame, the item's
 * (0, 0)–(w, h), clips it. x, y, w, h here are the whole picture in that
 * same item space: a crop is the frame cutting the picture, not a second
 * copy of the pixels. The pointer tool scales this rectangle with the
 * frame. The crop tool moves the frame and leaves the picture where it is
 * on the page, or drags the picture around inside the frame. */
struct PlacedImage {
    uint32_t asset = 0;
    float x = 0, y = 0, w = 0, h = 0;
};

/* Scale the picture with a frame resize, so the crop stays the same
 * fraction of the picture. */
PlacedImage scale_placement(PlacedImage im, float old_w, float old_h, float new_w, float new_h);
/* The frame's top-left moved to (left, top) in the old item space. The
 * picture stays put on the page. */
PlacedImage crop_placement(PlacedImage im, float left, float top);

struct Item {
    ItemId    id = 0;
    float     w = 0, h = 0;
    Transform xf;                // item space -> page
    std::variant<TextFrame, Shape, PlacedImage> content;

    bool is_text() const { return std::holds_alternative<TextFrame>(content); }
    bool is_image() const { return std::holds_alternative<PlacedImage>(content); }
    const Shape *shape() const { return std::get_if<Shape>(&content); }
    Shape *shape() { return std::get_if<Shape>(&content); }
    const PlacedImage *image() const { return std::get_if<PlacedImage>(&content); }
    PlacedImage *image() { return std::get_if<PlacedImage>(&content); }
};

/* One picture the publication owns. Page items store the id. `source` is
 * the file: URI it was placed from, so a linked copy can be found again
 * and an embedded copy can be relinked. */
struct ImageAsset {
    uint32_t id = 0;
    std::string name;
    std::string source;
    ImageMetadata meta;
};

struct Page {
    std::vector<Item> items;     // bottom to top
    bool hidden = false;         // kept, and shown, but not printed
};

struct StoryEntry {
    StoryId             id = 0;
    Story               story;
    std::vector<ItemId> thread;  // text frames in reading order
};

/* [Paper], [Black], [Registration] and PageMaker's default colors. */
std::vector<Swatch> default_swatches();

/* A named paragraph style (the Paragraph panel's style menu): paragraph
 * attributes, and the type its text is set in. A paragraph that uses it
 * carries the name in ParaStyle::name. */
struct StyleDef {
    std::string name;
    ParaStyle   para;            // para.name == name
    CharStyle   type;
};

/* Normal, then PageMaker's predefined styles and a few more. */
std::vector<StyleDef> default_styles();

/* The run that stands for a paragraph's type: the longest. Runs whose bold
 * or italic differ from it are emphasis. */
const Run *base_run(const Paragraph &p);
/* Give `p` the style: all of its paragraph attributes, and the style's
 * family, face, size, leading, tracking, set width and caps on every run.
 * Emphasis keeps its bold or italic; underline, strikethrough, color and
 * baseline shift stay as they were. */
void apply_style(Paragraph &p, const StyleDef &def);
/* `p` is formatted differently from `def` (shown as "Name+"). */
bool style_overridden(const Paragraph &p, const StyleDef &def);
/* A style named `name` made from `p`'s formatting. */
StyleDef style_from(const std::string &name, const Paragraph &p);

struct PageDoc {
    PageSetup               setup;
    std::vector<Page>       pages{1};
    std::vector<StoryEntry> stories;
    std::vector<Swatch>     swatches = default_swatches();
    std::vector<StyleDef>   styles = default_styles();
    std::vector<ImageAsset> images;        // the publication's pictures
    uint32_t                next_id = 1;   // shared by items and stories
    uint32_t                next_asset = 1;

    const Swatch *find_swatch(SwatchId id) const;
    const StyleDef *find_style(const std::string &name) const;
    StyleDef       *find_style(const std::string &name);
    /* The screen color of a paint; transparent for none. */
    Color resolve(const Paint &p) const;
    /* Adds a swatch (ids above the built-ins) and returns its id. */
    SwatchId add_swatch(const std::string &name, Color rgb);

    const ImageAsset *find_image(uint32_t id) const;
    ImageAsset       *find_image(uint32_t id);
    /* Assigns an id when `asset.id` is 0. */
    uint32_t add_image(ImageAsset asset);
    /* True when some item still shows this picture. */
    bool image_placed(uint32_t id) const;

    /* Lookups. `page` (when given) receives the item's page index. */
    Item       *find_item(ItemId id, size_t *page = nullptr);
    const Item *find_item(ItemId id, size_t *page = nullptr) const;
    StoryEntry       *find_story(StoryId id);
    const StoryEntry *find_story(StoryId id) const;
    size_t story_index(StoryId id) const;               // SIZE_MAX if none
    /* The story threading a text frame, and the frame's place in it. */
    const StoryEntry *story_of(ItemId frame, size_t *thread_index = nullptr) const;
    StoryEntry       *story_of(ItemId frame, size_t *thread_index = nullptr);

    /* The thread's frames in their own space, (0, 0)–(w, h), for compose(). */
    std::vector<Frame> thread_frames(const StoryEntry &s) const;

    /* Edits. New items go on top of the stacking order. */
    ItemId  add_item(size_t page, Item item);
    StoryId add_story(Story story);
    /* A text frame placed on `page` and threaded at `thread_index` of the
     * story (appended when past the end). */
    ItemId  add_text_frame(size_t page, StoryId story, float w, float h, const Transform &xf,
                           size_t thread_index = SIZE_MAX);
    /* Removes the item and takes it out of its thread; a story left with no
     * frames is removed too. A picture's asset leaves with its last frame
     * (the bytes stay in the ImageStore, so undo can bring it back). */
    void    remove_item(ItemId id);
    void    bring_to_front(ItemId id);
    void    send_to_back(ItemId id);
    /* One step up (dir > 0, toward the front) or down. Items in `ids` keep
     * their order, so a group moves together. */
    void    restack(const std::vector<ItemId> &ids, int dir);

    /* Insert a blank page at `index` (clamped to the end). */
    size_t  insert_page(size_t index);
    /* Remove the page and its items. False when it is the only page. */
    bool    remove_page(size_t index);
    /* `to` is the index the page occupies after the move. */
    void    move_page(size_t from, size_t to);
};

/* A one-page newsletter: a headline across the top over a rule, a body
 * story threaded through two columns (the second short enough that the
 * story oversets), and a rotated sidebar box with its own story. */
PageDoc sample_document();

} // namespace pagemade
