/*
 * pagemade/composerview.h — ComposerView: one page on a pasteboard, its
 * items drawn in stacking order, each story composed into its text blocks.
 *
 * The view's logical units are points. Host it in a ZoomScrollPanel: the
 * panel's scale is the zoom, and the composer never sees it. Text is
 * composed once per change, not per zoom, and drawn as glyph outlines.
 * Page content goes through the draw list (drawlist.h), the same one PDF
 * and SVG output will use; text overlays and handles are drawn on top.
 * Every item is hit-tested and resized in its own space under its
 * transform, so rotated items behave like upright ones.
 *
 * Pointer tool: click an item to select it (Shift adds or removes), drag
 * on empty space for a marquee (selects what it encloses), drag selected
 * items to move them, drag a corner handle to resize (a line's handles are
 * its ends), or drag a text block's bottom windowshade to change its
 * depth. Delete removes the selection; Cmd/Ctrl+F brings it to the front
 * and Cmd/Ctrl+B sends it to the back. With snapping on, moved edges and
 * dragged handles snap to the page edges, margins and column guides.
 *
 * Rotate tool: drag to turn the selection about its center; Shift snaps
 * the angle to 15 degree steps.
 *
 * Line, rectangle, ellipse and polygon tools: drag to draw (Shift draws a
 * square, circle or 45 degree line). New shapes take the default fill and
 * stroke, which the control palette sets while nothing is selected.
 *
 * Threading (PageMaker's manual text flow): click the red overset arrow
 * and the story is picked up — a new text block, sized to hold what
 * didn't fit, follows the pointer with its text already flowed in. Click
 * to drop it (Escape cancels and puts the text back). Click a threaded
 * block's top windowshade tab (the one with the plus) to merge its text
 * back into the parent block; the block goes away.
 *
 * Text tool (composer/edit.h does the model work): click inside a text
 * block to place the caret, or drag on empty space to draw a new block.
 * Drag to select, double-click for a word, triple-click for a paragraph.
 * Typing replaces the selection, Return splits the paragraph (Shift+Return
 * is a line break in the same paragraph), Cmd/Ctrl + A/C/X/V select all/
 * copy/cut/paste, B/I restyle the selection, Alt+arrows move by words.
 * Escape collapses the selection, then returns to the pointer tool.
 */
#pragma once

#include <nanogui/widget.h>

#include "composer/edit.h"
#include "drawlist.h"

#include <deque>
#include <functional>
#include <vector>

class ComposerView : public nanogui::Widget {
public:
    ComposerView(nanogui::Widget *parent, const pagemade::FontLibrary *fonts,
                 const pagemade::Hyphenator *hyphenator = nullptr);

    void set_document(pagemade::PageDoc doc);
    /* Edit freely, then call recompose(). */
    pagemade::PageDoc &document() { return m_doc; }
    void recompose();
    /* One per story, in PageDoc::stories order. */
    const std::vector<pagemade::Composition> &compositions() const { return m_comp; }

    void set_show_baselines(bool on)   { m_show_baselines = on; }
    void set_show_loose_tight(bool on) { m_show_loose_tight = on; }
    void set_show_guides(bool on)      { m_show_guides = on; }
    void set_snap(bool on)             { m_snap = on; }
    bool snap() const                  { return m_snap; }

    /* The toolbox. */
    enum class Tool { Pointer, Rotate, Text, Line, Rect, Ellipse, Polygon };
    void set_tool(Tool t);
    Tool tool() const { return m_tool; }
    bool editing() const { return edit_story() != nullptr; }

    /* The selection, in the order items were picked. */
    const std::vector<pagemade::ItemId> &selection() const { return m_sel; }
    void select(pagemade::ItemId id);   // 0 clears
    /* Select a story's text block by position: story index, thread index. */
    void select_frame(size_t story, size_t frame);

    /* ---- Control palette ------------------------------------------- */
    /* X/Y are the item's origin (its box's top-left corner, turned with
     * it) from the page's top-left, W/H its size, angle in degrees
     * counterclockwise (PageMaker's convention). One selected item only. */
    struct Geometry {
        float x = 0, y = 0, w = 0, h = 0, angle = 0;
    };
    bool geometry(Geometry &g) const;
    void set_geometry(const Geometry &g);       // one undo step
    /* The fill/stroke the palette shows: the first selected shape, or the
     * defaults for new shapes when no shape is selected. */
    pagemade::Shape shape_style() const;
    bool shapes_selected() const;
    /* Change the selected shapes (one undo step), or the defaults for new
     * shapes when none is selected, as PageMaker does. */
    void apply_shape_style(const std::function<void(pagemade::Shape &)> &fn);

    /* Undo/redo: whole-document snapshots (the model is small), one per
     * action. A run of typing or deleting coalesces into a single step;
     * caret moves and other actions close the run. View state (zoom) is
     * not undoable. */
    void push_undo();                  // snapshot the document as it is now
    void will_edit(bool burst);        // call before mutating the story
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    void undo();
    void redo();

    /* Top-left of the page in view units; the rest is pasteboard. */
    nanogui::Vector2f page_origin() const;

    /* Called after every recompose (status bars). */
    std::function<void()> on_recompose;
    /* Called when the tool changes (Escape back to the pointer tool). */
    std::function<void(Tool)> on_tool_change;
    /* Called when the selection or a selected item's geometry changes. */
    std::function<void()> on_selection_change;

    nanogui::Vector2i preferred_size(NVGcontext *ctx) const override;
    void draw(NVGcontext *ctx) override;
    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down,
                            int modifiers) override;
    bool mouse_drag_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel,
                          int button, int modifiers) override;
    bool mouse_motion_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel,
                            int button, int modifiers) override;
    bool focus_event(bool focused) override;
    bool keyboard_event(int key, int scancode, int action, int modifiers) override;
    bool keyboard_character_event(unsigned int codepoint) override;

private:
    enum class Handle { None, Move, TopLeft, TopRight, BottomLeft, BottomRight, Windowshade, TopTab };
    struct Hit {
        pagemade::ItemId item = 0;
        Handle handle = Handle::None;
    };

    /* Event points arrive rounded to whole points and, during drags, without
     * the panel's zoom applied. Both go through this instead, from the
     * screen's mouse position, at full precision. */
    pagemade::Point page_point_from_screen(const nanogui::Vector2i &screen_p) const;
    float zoom() const;
    /* Handles of selected items first, then items top to bottom. */
    Hit hit_test(const pagemade::Point &pt) const;
    /* Topmost text block under pt (0 = none). */
    pagemade::ItemId text_frame_at(const pagemade::Point &pt) const;
    /* Does pt (page) touch the item? Unfilled shapes only by their outline. */
    bool hits_item(const pagemade::Item &it, const pagemade::Point &pt) const;

    const std::vector<pagemade::Item> &items() const { return m_doc.pages[m_page].items; }
    bool is_selected(pagemade::ItemId id) const;
    void selection_changed();
    pagemade::Bounds selection_bounds() const;
    /* The composition of the story threading text frame `id`, and the
     * frame's place in the thread. */
    const pagemade::Composition *comp_of_frame(pagemade::ItemId id, size_t *thread_index) const;

    /* ---- Snapping ---------------------------------------------------- */
    std::vector<float> guides_x() const;
    std::vector<float> guides_y() const;
    /* Snap a page point to the guides, axis by axis. */
    pagemade::Point snap_point(pagemade::Point p) const;
    /* Adjust a move so one of the box's edges (or its center) lands on a guide. */
    pagemade::Point snap_move(const pagemade::Bounds &b, pagemade::Point d) const;

    void draw_page(NVGcontext *ctx, float px);
    /* Loose/tight lines, baselines, the text selection and the caret. */
    void draw_text_overlays(NVGcontext *ctx, const pagemade::Item &it, float px);
    void draw_chrome(NVGcontext *ctx, float px);   // outlines, handles, windowshades, marquee

    /* ---- Threading ------------------------------------------------- */
    /* The red arrow was clicked: thread a new block after the selected
     * one and let it follow the pointer until end_placement(). */
    void begin_placement(const pagemade::Point &pt);
    void end_placement(bool commit);      // false: the text goes back
    /* The page rectangle of a block that holds the text which doesn't fit
     * after thread frame `after`, its top centered under pt (height capped
     * at the page's column height). */
    pagemade::Frame measure_child_frame(const pagemade::StoryEntry &s, size_t after,
                                        const pagemade::Point &pt) const;

    /* ---- Undo -------------------------------------------------------*/
    void restore_snapshot();           // common tail of undo()/redo()

    /* ---- Pointer and drawing tools ---------------------------------- */
    void delete_selection_items();
    void arrange(bool to_front);
    /* Start drawing a shape (or a text block) at pt; update_creation()
     * stretches it to the pointer; finish_creation() keeps it or, for a
     * click without a drag, takes it back. */
    void begin_creation(const pagemade::Point &pt);
    void update_creation(pagemade::Point pt, bool constrain);
    void finish_creation();

    /* ---- Text tool ------------------------------------------------- */
    pagemade::Story *edit_story();
    const pagemade::Story *edit_story() const;
    const pagemade::Composition *edit_comp() const;
    /* The caret position nearest a page point, in the edit story: the
     * frame under the point, else the story's nearest frame. */
    pagemade::TextPos text_pos_at(const pagemade::Point &pt) const;
    bool has_selection() const { return editing() && m_caret != m_anchor; }
    /* extend: keep the anchor where it is (grow/shrink the selection). */
    void place_caret(pagemade::TextPos p, bool extend);
    void after_edit();                    // recompose and redraw
    bool delete_selection();              // true when something was erased
    void insert_string(const std::string &utf8, bool burst);
    void move_caret_key(int key, int modifiers);
    void toggle_style(int key);           // B -> bold, I -> italic

    const pagemade::FontLibrary *m_fonts;
    const pagemade::Hyphenator *m_hyphenator;
    pagemade::PageDoc m_doc;
    size_t m_page = 0;                    // the page on screen
    std::vector<pagemade::Composition> m_comp;

    Tool m_tool = Tool::Pointer;
    pagemade::StoryId m_edit_story = 0;   // story with the text caret
    pagemade::TextPos m_caret, m_anchor;  // selection is [anchor, caret)
    float m_goal_x = -1.f;                // remembered x for up/down
    bool m_selecting = false;             // drag-selecting with the text tool
    double m_last_click = -1.0;
    int m_clicks = 0;
    pagemade::Point m_down_pt;            // page point of the last press

    std::vector<pagemade::ItemId> m_sel;
    Handle m_drag = Handle::None;         // what the current drag does
    pagemade::ItemId m_drag_target = 0;   // the item whose handle is dragged
    bool m_rotating = false;              // rotate-tool drag in progress
    bool m_marquee = false;               // marquee drag in progress
    bool m_marquee_add = false;           // Shift: add to the selection
    pagemade::Point m_marquee_end;
    bool m_creating = false;              // a drawing tool's drag in progress
    pagemade::ItemId m_created = 0;
    pagemade::PageDoc m_creation_saved;   // pre-drawing state, pushed if something is drawn
    pagemade::Shape m_default_shape;      // fill and stroke for new shapes
    bool m_snap = true;
    bool m_placing = false;               // a picked-up story follows the pointer
    pagemade::ItemId m_placement_parent = 0;
    pagemade::PageDoc m_placement_saved;  // pre-placement state, pushed on commit

    std::deque<pagemade::PageDoc> m_undo, m_redo;
    bool m_burst_open = false;            // typing/deleting run in progress
    bool m_gesture_saved = false;         // drag pushed its snapshot
    pagemade::Point m_drag_start;         // page point at press
    std::vector<pagemade::Item> m_drag_items;   // the selected items at press
    pagemade::Bounds m_drag_bounds;       // their page bounds at press

    bool m_show_baselines = false;
    bool m_show_loose_tight = false;
    bool m_show_guides = true;
};
