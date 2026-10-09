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
 * anywhere in the stacking order (and, later, across pages). Every text
 * frame is in exactly one thread.
 *
 * The whole model is plain values: copying a PageDoc is a snapshot, which
 * is how undo works.
 */
#pragma once

#include "composer/composer.h"

#include <cstdint>
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

/* A text block. Its story is the one whose thread lists it. Per-block
 * text options (inset, vertical alignment, columns) will live here. */
struct TextFrame {};

/* PageMaker's drawn elements. A line runs from (0, 0) to (w, h) in item
 * space, so either may be negative. Fill and stroke become swatches and
 * line styles with the drawing pipeline. */
struct Shape {
    enum class Kind { Rect, Ellipse, Line };
    Kind  kind = Kind::Rect;
    float corner_radius = 0.f;
    bool  filled = false;
    Color fill{1, 1, 1, 1};
    float stroke_width = 1.f;    // 0 = no stroke
    Color stroke;
};

struct Item {
    ItemId    id = 0;
    float     w = 0, h = 0;
    Transform xf;                // item space -> page
    std::variant<TextFrame, Shape> content;

    bool is_text() const { return std::holds_alternative<TextFrame>(content); }
    const Shape *shape() const { return std::get_if<Shape>(&content); }
    Shape *shape() { return std::get_if<Shape>(&content); }
};

struct Page {
    std::vector<Item> items;     // bottom to top
};

struct StoryEntry {
    StoryId             id = 0;
    Story               story;
    std::vector<ItemId> thread;  // text frames in reading order
};

struct PageDoc {
    PageSetup               setup;
    std::vector<Page>       pages{1};
    std::vector<StoryEntry> stories;
    uint32_t                next_id = 1;   // shared by items and stories

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
     * frames is removed too. */
    void    remove_item(ItemId id);
    void    bring_to_front(ItemId id);
    void    send_to_back(ItemId id);
};

/* A one-page newsletter: a headline across the top over a rule, a body
 * story threaded through two columns (the second short enough that the
 * story oversets), and a rotated sidebar box with its own story. */
PageDoc sample_document();

} // namespace pagemade
