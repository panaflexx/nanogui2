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
 * depth. Delete removes the selection. Arrange: Cmd/Ctrl+F brings the
 * selection to the front, Cmd/Ctrl+B sends it to the back, Cmd/Ctrl+]
 * brings it forward one step and Cmd/Ctrl+[ sends it backward one
 * (Shift with the brackets jumps to the front or the back). With snapping
 * on, moved edges and
 * dragged handles snap to the page edges, margins and column guides.
 *
 * Rotate tool: drag to turn the selection about its center; Shift snaps
 * the angle to 15 degree steps.
 *
 * Line, rectangle, ellipse and polygon tools: drag to draw (Shift draws a
 * square, circle or 45 degree line). New shapes take the default fill and
 * stroke, which the control palette sets while nothing is selected.
 *
 * Crop tool: drag a corner to trim the frame (the picture stays where it
 * is on the page) or drag inside a picture to slide it. Shift locks that
 * slide to the larger axis. The pointer tool scales the picture with the
 * frame. Place drops a picture at the margin, at its print size.
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
#include <map>
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
    enum class Tool { Pointer, Crop, Rotate, Text, Line, Rect, Ellipse, Polygon };
    void set_tool(Tool t);
    Tool tool() const { return m_tool; }
    bool editing() const { return edit_story() != nullptr; }

    /* Picture bytes live outside the document (undo snapshots stay small).
     * The view does not own the store. */
    void set_image_store(pagemade::ImageStore *images);
    /* Place a file: one undo step, selected, then back to the pointer.
     * The id is taken before the file is read, and a failure does not
     * reuse it. False leaves the document unchanged. */
    bool place_image_file(const std::string &path, std::string *error);
    /* Another frame of a picture the publication already has. */
    void place_image(uint32_t asset);

    /* The selection, in the order items were picked. */
    const std::vector<pagemade::ItemId> &selection() const { return m_sel; }
    void select(pagemade::ItemId id);   // 0 clears
    /* Select a story's text block by position: story index, thread index. */
    void select_frame(size_t story, size_t frame);

    /* ---- Pages ------------------------------------------------------ */
    /* One page is on the pasteboard. Hidden pages can be shown and edited;
     * printing skips them. */
    size_t page_index() const { return m_page; }
    size_t page_count() const { return m_doc.pages.size(); }
    bool page_hidden(size_t i) const {
        return i < m_doc.pages.size() && m_doc.pages[i].hidden;
    }
    void show_page(size_t i);
    void insert_page(bool after);          // one undo step
    void remove_page();                    // keeps the last page
    void move_page_by(int delta);
    /* Dragging a page icon: `record_undo` on the first step of the gesture
     * so the whole drag is one undo step. */
    void move_page_to(size_t index, bool record_undo);
    void set_page_hidden(bool hidden);

    /* ---- Arrange ---------------------------------------------------- */
    enum class Stack { Forward, Backward, Front, Back };
    void arrange(Stack how);

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

    /* Character attributes for the control palette's type view. With a text
     * selection, the selection; at a caret, the style about to be typed;
     * with text blocks selected, the type in those blocks; otherwise the
     * defaults for a new text block. `mix_*` is set when the range disagrees
     * with itself. False only when selected blocks hold no text. */
    struct TypeStyle {
        std::string family, face;
        bool  bold = false, italic = false;
        pagemade::Caps caps = pagemade::Caps::Normal;
        bool  underline = false, strike = false;
        bool  kerning = true, ligatures = true;
        float size = 12.f, leading = 0.f, baseline = 0.f;
        float tracking = 0.f, hscale = 100.f;
        bool  mix_family = false, mix_style = false, mix_size = false;
        bool  mix_leading = false, mix_baseline = false;
        bool  mix_caps = false, mix_deco = false;
        bool  mix_track = false, mix_hscale = false;
        bool  mix_kerning = false, mix_ligatures = false;
    };
    bool type_style(TypeStyle &t) const;
    void apply_type(const std::function<void(pagemade::CharStyle &)> &fn);

    /* Paragraph alignment of the selected lines. With the text tool, that
     * is the caret's line or the lines the selection touches, each whole
     * line. With text blocks selected, it is the lines in those blocks.
     * False when there is no such text. `mixed` when the lines disagree. */
    bool text_align(pagemade::Align &align, bool &mixed) const;
    /* Apply `align` there. A line that is only part of its paragraph is
     * split off, so the rest of the paragraph is left as it was. One undo
     * step. Does nothing when text_align would return false. */
    void apply_align(pagemade::Align align);

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

    /* Changed since it was last saved (or loaded)? Every document state has
     * a version, and undo and redo carry it, so undoing back to the saved
     * state counts as unmodified again. */
    bool modified() const { return m_version != m_clean_version; }
    void mark_clean();                 // the document was just saved
    /* Called whenever the document changes, is saved or is replaced. */
    std::function<void()> on_document_change;

    /* Top-left of the page in view units; the rest is pasteboard. */
    nanogui::Vector2f page_origin() const;

    /* Called after every recompose (status bars). */
    std::function<void()> on_recompose;
    /* Called when the tool changes (Escape back to the pointer tool). */
    std::function<void(Tool)> on_tool_change;
    /* Called when the selection or a selected item's geometry changes. */
    std::function<void()> on_selection_change;
    /* Called when the page on screen, or the page list, changes. */
    std::function<void()> on_page_change;

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

    const std::vector<pagemade::Item> &items() const {
        if (m_page < m_doc.pages.size())
            return m_doc.pages[m_page].items;
        static const std::vector<pagemade::Item> none;
        return none;
    }
    void page_changed();               // clear the selection and tell the shell
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
    /* Start drawing a shape (or a text block) at pt; update_creation()
     * stretches it to the pointer; finish_creation() keeps it or, for a
     * click without a drag, takes it back. */
    void begin_creation(const pagemade::Point &pt);
    void update_creation(pagemade::Point pt, bool constrain);
    void finish_creation();

    /* A new frame of `asset` at print size, at the margin. Caller recorded
     * the undo step and added the asset. */
    void place_frame(uint32_t asset, const pagemade::ImageMetadata &meta);
    /* Drop GPU copies when the store is replaced. Called from draw(). */
    void sync_textures(NVGcontext *ctx);
    /* Display view for one placement. 0 draws the stand-in. */
    int texture_for(NVGcontext *ctx, const pagemade::DrawImage &im, float px);

    /* ---- Text tool ------------------------------------------------- */
    /* Lines whose alignment the toolbar edits: the caret line, the
     * selected lines, or the lines inside the selected text blocks. */
    struct AlignHit {
        size_t story = 0;             // index into the document's stories
        pagemade::TextPos a, b;       // half-open range in that story
    };
    void collect_align_hits(std::vector<AlignHit> &out) const;

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
    pagemade::CharStyle m_default_type;   // type for a new text block
    bool m_typing_on = false;             // caret: the next insert uses m_typing
    pagemade::CharStyle m_typing;
    bool m_snap = true;
    bool m_placing = false;               // a picked-up story follows the pointer
    pagemade::ItemId m_placement_parent = 0;
    pagemade::PageDoc m_placement_saved;  // pre-placement state, pushed on commit

    struct Snapshot {
        pagemade::PageDoc doc;
        uint64_t version;
    };
    /* Push a state as an undo step; the current state gets a new version. */
    void push_snapshot(pagemade::PageDoc doc, uint64_t version);
    void bump_version();
    std::deque<Snapshot> m_undo, m_redo;
    uint64_t m_version = 0, m_clean_version = 0, m_next_version = 0;
    uint64_t m_creation_version = 0, m_placement_version = 0;
    bool m_burst_open = false;            // typing/deleting run in progress
    bool m_gesture_saved = false;         // drag pushed its snapshot
    pagemade::Point m_drag_start;         // page point at press
    std::vector<pagemade::Item> m_drag_items;   // the selected items at press
    pagemade::Bounds m_drag_bounds;       // their page bounds at press

    bool m_show_baselines = false;
    bool m_show_loose_tight = false;
    bool m_show_guides = true;

    pagemade::ImageStore *m_images = nullptr;
    /* Screen copies of display views. Freed when the store's generation
     * changes, not from the destructor: the screen may already be gone. */
    struct Tex { int id = 0; int w = 0, h = 0; };
    std::map<uint32_t, Tex> m_tex;
    uint64_t m_tex_gen = 0;
};
