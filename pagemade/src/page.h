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
using LayerId = uint32_t;    // 0 = none

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

/* Document Setup: page size, margins and column guides, in points.
 * With facing pages, the inside margin is toward the spine: on the left
 * of a right-hand page and on the right of a left-hand page. */
struct PageSetup {
    float width = 612.f, height = 792.f;    // US Letter
    float margin_top = 54.f, margin_bottom = 54.f;
    float margin_inside = 54.f, margin_outside = 54.f;
    int   columns = 2;
    float gutter = 18.f;
    bool  facing = false;
    /* How far pictures and fills that run off the page should carry on
     * past the trim, on every side (0 when nothing bleeds). The PDF can
     * include it (PdfSettings::bleed). */
    float bleed = 0.f;
};

/* How a PDF is written (File > Export PDF). Kept with the publication, so
 * a book exports the same way every time. */
struct PdfSettings {
    std::string preset = "High quality print";
    /* Pages: all, or first..last (1-based). Hidden pages only on request. */
    bool  all_pages = true;
    int   first_page = 1, last_page = 1;
    bool  include_hidden = false;
    /* Pictures above `downsample_above` ppi, as placed, are resampled to
     * `downsample_to`. Automatic keeps a JPEG's own data when it isn't
     * resampled, sends HEIC/AVIF as JPEG and other pictures lossless; Jpeg
     * makes every opaque picture JPEG; Lossless never adds JPEG loss. */
    bool  downsample = true;
    float downsample_above = 450.f, downsample_to = 300.f;
    enum class Compression { Automatic, Jpeg, Lossless };
    Compression compression = Compression::Automatic;
    int   jpeg_quality = 92;
    /* Rgb: pictures keep their own profiles, other colors are device RGB.
     * RgbTagged: every color is tagged (sRGB unless a picture has its own),
     * as PDF/X wants. Cmyk: not yet. */
    enum class Color { Rgb, RgbTagged, Cmyk };
    Color color = Color::Rgb;
    /* PDF/X-4: tagged color, an output intent, trim and bleed boxes and
     * the identification metadata. */
    bool  pdfx4 = false;
    std::string output_intent;   // ICC profile file; empty is the built-in sRGB
    bool  bleed = true;          // the Document Setup bleed, past the trim
    bool  crop_marks = false;
    /* Flatten transparency: a page with anything see-through is written as
     * one opaque picture at `flatten_ppi` (for print services that can't
     * take live transparency; PDF/X-4 carries it, so presets leave this
     * off). Pages without transparency stay vector. */
    bool  flatten = false;
    float flatten_ppi = 300.f;
    std::string title, author, subject;
    /* Check the publication first and list what may print badly. */
    bool  preflight = true;
    float min_ppi = 200.f;
    bool  open_after = false;
};

/* How the pasteboard shows pages. A screen setting, kept out of the file. */
enum class PageView { One, Spread, Stack };

constexpr float kPageGap = 24.f;   // space between pages on the pasteboard

struct PageSlot {
    size_t index = 0;
    float x = 0, y = 0;            // top-left of that page, points, y down
};

/* Pages on screen for `current`, and the size of that arrangement.
 * A facing spread puts page 1 alone on the right, then pairs 2–3, 4–5, …
 * meeting at the spine; a facing stack is a column of those spreads.
 * With facing pages off, pages pair as 1–2, 3–4, … with a gap, and the
 * stack is a single column. */
void arrange_pages(const PageSetup &setup, size_t page_count, size_t current,
                   PageView view, std::vector<PageSlot> &slots,
                   float &width, float &height);

/* Left and right margins of page `index` (0-based). */
inline void page_side_margins(const PageSetup &s, size_t index, float &left, float &right) {
    const bool verso = s.facing && (index % 2 == 1);
    left = verso ? s.margin_outside : s.margin_inside;
    right = verso ? s.margin_inside : s.margin_outside;
}

/* Colors palette entries. Paints refer to swatches by id, so changing a
 * swatch recolors everything that uses it. Ids 1-3 are PageMaker's built-in
 * [Paper], [Black] and [Registration]; 0 means no paint ([None]). CMYK is
 * the truth: the rgb screen color is derived from it (naive conversion),
 * and print output will use the CMYK values. */
using SwatchId = uint32_t;
constexpr SwatchId kNoPaint = 0, kPaper = 1, kBlack = 2, kRegistration = 3;

struct Cmyk {
    float c = 0, m = 0, y = 0, k = 0;   // 0..100, percent
};

struct Swatch {
    SwatchId    id = 0;
    std::string name;
    Color       rgb;
    Cmyk        cmyk;
};

/* Naive CMYK -> RGB for the screen. Inverts cmyk_to_cmyk(). */
Color cmyk_to_rgb(const Cmyk &cmyk);
/* Naive RGB -> CMYK: K is the min channel, the rest scaled out of it. */
Cmyk rgb_to_cmyk(const Color &rgb);
/* The display string: "C=100 M=0 Y=0 K=0". */
std::string cmyk_text(const Cmyk &cmyk);

/* A swatch at a tint: PageMaker's screens, from 100% toward paper white. */
struct Paint {
    SwatchId swatch = kNoPaint;
    float    tint = 100.f;
    bool none() const { return swatch == kNoPaint; }
};

/* PageMaker's custom lines. The compound ones (Double through ThinThickThin)
 * are solid bands across the stroke's weight; ThickThin puts the thick band
 * outside a closed shape (left of travel along a line). New styles go at the
 * end: the palette menu indexes this enum. */
enum class LineStyle { Solid, Dashed, Dotted, DashDot, Double, ThickThin, ThinThick, ThinThickThin };

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
    float  opacity = 100.f;              // percent; the palette's fill tint
    /* A border on the frame, centered on its edge. Pictures start without. */
    Stroke stroke{{kNoPaint, 100.f}, 1.f, LineStyle::Solid};
};

/* Scale the picture with a frame resize, so the crop stays the same
 * fraction of the picture. */
PlacedImage scale_placement(PlacedImage im, float old_w, float old_h, float new_w, float new_h);
/* The frame's top-left moved to (left, top) in the old item space. The
 * picture stays put on the page. */
PlacedImage crop_placement(PlacedImage im, float left, float top);

/* A placed vector graphic. The frame clips it the way a picture's frame
 * does: x, y, w, h are the whole graphic in item space. The file bytes and
 * the parsed scene live in a VectorStore, not in the document snapshot. */
struct PlacedVector {
    uint32_t asset = 0;
    float x = 0, y = 0, w = 0, h = 0;
    float opacity = 100.f;
    Stroke stroke{{kNoPaint, 100.f}, 1.f, LineStyle::Solid};
};

PlacedVector scale_placement(PlacedVector v, float old_w, float old_h, float new_w, float new_h);
PlacedVector crop_placement(PlacedVector v, float left, float top);

/* A layer of the publication, on every page. Layers stack bottom to top;
 * within one, items keep their own stacking order. A hidden layer is not
 * drawn, printed or exported; a locked one is drawn but its items can't be
 * selected or edited. */
constexpr size_t kMaxLayers = 8;

struct Layer {
    LayerId     id = 0;
    std::string name;
    bool        visible = true;
    bool        locked = false;
};

struct Item {
    ItemId    id = 0;
    LayerId   layer = 0;         // 0 or unknown: the bottom layer
    /* Items on one page with the same nonzero group select, move and
     * stack together (a picture and its caption). Groups don't nest. */
    uint32_t  group = 0;
    float     w = 0, h = 0;
    Transform xf;                // item space -> page
    std::variant<TextFrame, Shape, PlacedImage, PlacedVector> content;

    bool is_text() const { return std::holds_alternative<TextFrame>(content); }
    bool is_image() const { return std::holds_alternative<PlacedImage>(content); }
    bool is_vector() const { return std::holds_alternative<PlacedVector>(content); }
    const Shape *shape() const { return std::get_if<Shape>(&content); }
    Shape *shape() { return std::get_if<Shape>(&content); }
    const PlacedImage *image() const { return std::get_if<PlacedImage>(&content); }
    PlacedImage *image() { return std::get_if<PlacedImage>(&content); }
    const PlacedVector *vector() const { return std::get_if<PlacedVector>(&content); }
    PlacedVector *vector() { return std::get_if<PlacedVector>(&content); }
};

/* One picture the publication owns. Page items store the id. `source` is
 * the file: URI it was placed from, so a linked copy can be found again
 * and an embedded copy can be relinked. */
struct ImageAsset {
    uint32_t id = 0;
    std::string name;
    std::string source;
    ImageMetadata meta;
    /* The source file as it was read, to tell when it has changed since
     * (0 when unknown: older files, or a picture with no file). `hash` is
     * the 64-bit FNV-1a of those bytes. */
    uint64_t source_size = 0;
    int64_t  source_mtime = 0;           // seconds since the epoch
    uint64_t hash = 0;
};

/* One vector graphic the publication owns. Page items store the id. The
 * scene parsed from the file is not here: a VectorStore keeps it, the same
 * way an ImageStore keeps picture bytes. `skipped` is what this slice did
 * not draw ("gradients", "clips", "text"). Ids come from that store and
 * are not the picture ids. */
struct VectorAsset {
    uint32_t id = 0;
    std::string name;
    std::string source;
    float width = 0, height = 0;         // the graphic's box, points
    std::vector<std::string> skipped;
    uint64_t source_size = 0;
    int64_t source_mtime = 0;
    uint64_t hash = 0;
};

/* Drag and drop type for a picture of this publication; the bytes are the
 * asset id in decimal. */
constexpr const char *kImageAssetDragType = "application/x-pagemade-image-asset";
/* Alongside a file dragged from the asset window: its pixel size ("w h"),
 * when already known, so the frame placed for it starts at the right shape. */
constexpr const char *kImagePixelsDragType = "application/x-pagemade-image-pixels";

/* A ruler guide, dragged off a ruler: a vertical line at x (`vertical`), or
 * a horizontal one at y, in points from the page's top-left. Guides snap
 * like the margins and are never printed. */
struct Guide {
    bool  vertical = true;
    float pos = 0;
};

/* A fresh identity for each page made this session. */
inline uint32_t next_page_uid() {
    static uint32_t next = 0;
    return ++next;
}

struct Page {
    /* Session-only identity, not saved: copies (undo snapshots) keep it,
     * so a page can be followed across undo and reordering. */
    uint32_t uid = next_page_uid();
    std::vector<Item> items;     // bottom to top
    bool hidden = false;         // kept, and shown, but not printed
    std::vector<Guide> guides;   // ruler guides on this page only
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

/* The attributes a character style sets; the rest of the text's formatting
 * shows through (an Emphasis style is only italic). */
enum CharField : uint32_t {
    kCharFamily    = 1u << 0,
    kCharFace      = 1u << 1,    // a named face ("Light")
    kCharBold      = 1u << 2,
    kCharItalic    = 1u << 3,
    kCharSize      = 1u << 4,
    kCharLeading   = 1u << 5,
    kCharColor     = 1u << 6,    // swatch, tint (and RGB)
    kCharUnderline = 1u << 7,
    kCharStrike    = 1u << 8,
    kCharCaps      = 1u << 9,
    kCharTracking  = 1u << 10,
    kCharHScale    = 1u << 11,
    kCharBaseline  = 1u << 12,
    kCharKerning   = 1u << 13,
    kCharLigatures = 1u << 14,
    kCharNoBreak   = 1u << 15,
    kCharAllFields = (1u << 16) - 1,
};
/* The field names in the file, in bit order. */
const char *char_field_name(int bit);

/* A named character style (the Character panel's menu). */
struct CharStyleDef {
    std::string name;
    CharStyle   type;            // only `fields` of it count
    uint32_t    fields = 0;
};

/* Emphasis, Strong, Small caps and Accent (a spot of red). */
std::vector<CharStyleDef> default_char_styles();
/* Set the style's fields on `cs` and tag it with the name. Bold or italic
 * without a named face clears the face, so the family's bold or italic
 * cut is used. */
void apply_char_style(CharStyle &cs, const CharStyleDef &def);
/* `cs` differs from the style in a field the style sets. */
bool char_style_overridden(const CharStyle &cs, const CharStyleDef &def);
/* A style named `name` that sets what `cs` changes from `base` (the
 * paragraph's own type); everything about the type when nothing does. */
CharStyleDef char_style_from(const std::string &name, const CharStyle &cs, const CharStyle &base);

/* The run that stands for a paragraph's type: the longest. Runs whose bold
 * or italic differ from it are emphasis. */
const Run *base_run(const Paragraph &p);
/* Give `p` the style: all of its paragraph attributes, and the style's
 * family, face, size, leading, tracking, set width and caps on every run.
 * Emphasis keeps its bold or italic; underline, strikethrough, color and
 * baseline shift stay as they were. */
void apply_style(Paragraph &p, const StyleDef &def,
                 const std::vector<CharStyleDef> &char_styles = {});
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
    std::vector<CharStyleDef> char_styles = default_char_styles();
    std::vector<ImageAsset> images;        // the publication's pictures
    std::vector<VectorAsset> vectors;      // the publication's placed graphics
    std::vector<Guide>      guides;        // ruler guides on every page
    PdfSettings             pdf;           // how this publication exports
    std::vector<Layer>      layers{Layer{1, "Default"}};   // bottom to top
    LayerId                 active_layer = 1;              // new items go here
    uint32_t                next_id = 1;   // shared by items and stories
    uint32_t                next_asset = 1;
    LayerId                 next_layer = 2;

    /* ---- Layers ---- */
    const Layer *find_layer(LayerId id) const;
    Layer       *find_layer(LayerId id);
    size_t layer_index(LayerId id) const;                // SIZE_MAX if none
    /* The item's layer, or the bottom one when it names none. */
    LayerId layer_of(const Item &it) const;
    bool item_visible(const Item &it) const;
    /* Visible and unlocked: it can be selected and edited. */
    bool item_editable(const Item &it) const;
    /* New items can go on the active layer (visible and unlocked). */
    bool active_layer_editable() const;
    /* A new layer above the active one, made active. 0 when there are
     * kMaxLayers already. */
    LayerId add_layer(const std::string &name);
    /* Its items go to the layer below it (above, for the bottom one). False
     * for the only layer. */
    bool remove_layer(LayerId id);
    /* One step up (dir > 0) or down. */
    void move_layer(LayerId id, int dir);
    /* Put items on a layer, at the top of it. */
    void set_items_layer(const std::vector<ItemId> &ids, LayerId layer);
    /* Each page's items in layer order, keeping the order within a layer.
     * Every edit that moves items between layers keeps this. */
    void sort_by_layer();
    /* At least one layer, a valid active one, and every item on a real layer
     * (an opened file). */
    void tidy_layers();

    const Swatch *find_swatch(SwatchId id) const;
    const StyleDef *find_style(const std::string &name) const;
    StyleDef       *find_style(const std::string &name);
    const CharStyleDef *find_char_style(const std::string &name) const;
    CharStyleDef       *find_char_style(const std::string &name);
    /* The screen color of a paint; transparent for none. */
    Color resolve(const Paint &p) const;
    /* Text's color: its swatch and tint, or `rgb` without a swatch (or
     * when the swatch is gone). */
    Color text_color(uint32_t swatch, float tint, const Color &rgb) const;
    /* Adds a swatch (ids above the built-ins) and returns its id. */
    SwatchId add_swatch(const std::string &name, Color rgb);

    const ImageAsset *find_image(uint32_t id) const;
    ImageAsset       *find_image(uint32_t id);
    /* Assigns an id when `asset.id` is 0. */
    uint32_t add_image(ImageAsset asset);
    /* True when some item still shows this picture. */
    bool image_placed(uint32_t id) const;
    const VectorAsset *find_vector(uint32_t id) const;
    VectorAsset       *find_vector(uint32_t id);
    /* `asset.id` is the store's id. 0 is refused, and picture ids are left
     * alone: the two tables don't share a counter. */
    uint32_t add_vector(VectorAsset asset);
    bool vector_placed(uint32_t id) const;
    /* Take an unused picture out of the publication (a placed one stays:
     * false). Pictures otherwise stay when their last frame goes, so an
     * asset library can show and place them again. */
    bool remove_image(uint32_t id);
    /* Every picture remove_image would take. One pass, so a caller can
     * make it a single undo step. */
    size_t remove_unused_images();

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
     * frames is removed too. A picture's asset stays, unused, after its
     * last frame goes (remove_image takes it out). */
    void    remove_item(ItemId id);
    /* Move an item onto `page`, on top of that page's stack. The id and the
     * transform stay as they are; the caller shifts the transform into the
     * new page's space. False when the item is missing or already there. */
    bool    transfer_item(ItemId id, size_t page);
    void    bring_to_front(ItemId id);
    void    send_to_back(ItemId id);
    /* One step up (dir > 0, toward the front) or down. Items in `ids` keep
     * their order, so a group moves together. */
    void    restack(const std::vector<ItemId> &ids, int dir);

    /* The item and everything grouped with it, in stacking order; just
     * the item when it isn't grouped; empty when it's missing. */
    std::vector<ItemId> group_members(ItemId id) const;
    /* Group the items (two or more, on one page; groups they were in are
     * merged). They become adjacent in the stack where the topmost one
     * was, keeping their order. Returns the group, or 0 when they can't
     * be grouped. */
    uint32_t group_items(const std::vector<ItemId> &ids);
    /* Take the items' whole groups apart. */
    void     ungroup_items(const std::vector<ItemId> &ids);
    /* Clear groups left with one member, or split across pages. */
    void     tidy_groups();

    /* Insert a blank page at `index` (clamped to the end). */
    size_t  insert_page(size_t index);
    /* Remove the page and its items. False when it is the only page. */
    bool    remove_page(size_t index);
    /* `to` is the index the page occupies after the move. */
    void    move_page(size_t from, size_t to);
    /* Copy pages [first, first + count) and insert the copies right after
     * them: items (new ids and groups, same layers and stacking), guides
     * and the hidden flag. Each story threading copied frames is copied
     * with only those frames threaded, so a story that runs across a
     * copied spread stays one story in the copy. Returns the index of the
     * first copy, or SIZE_MAX when the range is empty. */
    size_t  duplicate_pages(size_t first, size_t count);
};

/* A blank publication: `page_count` empty pages (at least one) at `setup`. */
PageDoc new_publication(PageSetup setup, int page_count);

/* A one-page newsletter: a headline across the top over a rule, a body
 * story threaded through two columns (the second short enough that the
 * story oversets), and a rotated sidebar box with its own story. */
PageDoc sample_document();

} // namespace pagemade
