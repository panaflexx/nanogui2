/*
 * pagemade/composerview.h — ComposerView: one page on a pasteboard, with
 * each text flow composed into its text blocks.
 *
 * The view's logical units are points. Host it in a ZoomScrollPanel: the
 * panel's scale is the zoom, and the composer never sees it. Text is
 * composed once per change, not per zoom, and drawn as glyph outlines.
 *
 * Pointer tool basics: click a text block to select it, drag inside it to
 * move it, drag a corner handle to resize it, or drag the bottom
 * windowshade handle to change its depth. Every change recomposes.
 * A red arrow in the bottom windowshade means the story is overset.
 *
 * Threading (PageMaker's manual text flow): click the red overset arrow
 * and the story is picked up — a new text block, sized to hold what
 * didn't fit, follows the pointer with its text already flowed in. Click
 * to drop it (Escape cancels and puts the text back). Click a threaded
 * block's top windowshade tab (the one with the plus) to merge its text
 * back into the parent block; the block goes away.
 *
 * Text tool basics (composer/edit.h does the model work): click inside a
 * text block to place the caret, drag to select, double-click for a word,
 * triple-click for a paragraph. Typing replaces the selection, Return
 * splits the paragraph (Shift+Return is a line break in the same
 * paragraph), Cmd/Ctrl + A/C/X/V select all/copy/cut/paste, B/I restyle
 * the selection, Alt+arrows move by words. Escape collapses the
 * selection, then returns to the pointer tool.
 */
#pragma once

#include <nanogui/widget.h>

#include "composer/edit.h"
#include "page.h"

#include <deque>
#include <functional>
#include <vector>

class ComposerView : public nanogui::Widget {
public:
    ComposerView(nanogui::Widget *parent, const pagemade::FontLibrary *fonts);

    void set_document(pagemade::PageDoc doc);
    /* Edit freely, then call recompose(). */
    pagemade::PageDoc &document() { return m_doc; }
    void recompose();
    const std::vector<pagemade::Composition> &compositions() const { return m_comp; }

    void set_show_baselines(bool on)   { m_show_baselines = on; }
    void set_show_loose_tight(bool on) { m_show_loose_tight = on; }
    void set_show_guides(bool on)      { m_show_guides = on; }

    /* The toolbox: the pointer tool manipulates text blocks, the text
     * tool edits the story inside them. */
    enum class Tool { Pointer, Text };
    void set_tool(Tool t);
    Tool tool() const { return m_tool; }
    bool editing() const { return m_edit_flow >= 0; }
    int edit_flow() const { return m_edit_flow; }

    /* Select a text block (flow -1 clears the selection). */
    void select(int flow, int frame) { m_sel_flow = flow; m_sel_frame = frame; }

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
        int flow = -1, frame = -1;
        Handle handle = Handle::None;
    };

    /* Event points arrive rounded to whole points and, during drags, without
     * the panel's zoom applied. Both go through this instead, from the
     * screen's mouse position, at full precision. */
    nanogui::Vector2f page_point_from_screen(const nanogui::Vector2i &screen_p) const;
    float zoom() const;
    Hit hit_test(const nanogui::Vector2f &pt) const;

    void draw_page(NVGcontext *ctx, float px);
    void draw_overlays(NVGcontext *ctx, float px);
    void draw_frames(NVGcontext *ctx, float px);
    void draw_text_overlays(NVGcontext *ctx, float px);   // selection + caret

    /* ---- Threading ------------------------------------------------- */
    /* The red arrow was clicked: insert a new block after the selected
     * one and let it follow the pointer until end_placement(). */
    void begin_placement(const nanogui::Vector2f &pt);
    void end_placement(bool commit);      // false: the text goes back
    /* A block sized to hold the text that doesn't fit after `after`, its
     * top centered under pt (height capped at the page's column height). */
    pagemade::Frame measure_child_frame(int flow, int after,
                                        const nanogui::Vector2f &pt) const;

    /* ---- Undo -------------------------------------------------------*/
    void restore_snapshot();           // common tail of undo()/redo()

    /* ---- Text tool ------------------------------------------------- */
    pagemade::Story *edit_story();
    const pagemade::Composition *edit_comp() const;
    bool has_selection() const { return editing() && m_caret != m_anchor; }
    /* extend: keep the anchor where it is (grow/shrink the selection). */
    void place_caret(pagemade::TextPos p, bool extend);
    void after_edit();                    // recompose and redraw
    bool delete_selection();              // true when something was erased
    void insert_string(const std::string &utf8, bool burst);
    void move_caret_key(int key, int modifiers);
    void toggle_style(int key);           // B -> bold, I -> italic

    const pagemade::FontLibrary *m_fonts;
    pagemade::PageDoc m_doc;
    std::vector<pagemade::Composition> m_comp;

    Tool m_tool = Tool::Pointer;
    int m_edit_flow = -1;                 // story with the text caret
    pagemade::TextPos m_caret, m_anchor;  // selection is [anchor, caret)
    float m_goal_x = -1.f;                // remembered x for up/down
    bool m_selecting = false;             // drag-selecting with the text tool
    double m_last_click = -1.0;
    int m_clicks = 0;
    nanogui::Vector2f m_down_pt;          // page point of the last press

    int m_sel_flow = -1, m_sel_frame = -1;
    Handle m_drag = Handle::None;
    bool m_placing = false;              // a picked-up story follows the pointer
    pagemade::PageDoc m_placement_saved; // pre-placement state, pushed on commit

    std::deque<pagemade::PageDoc> m_undo, m_redo;
    bool m_burst_open = false;           // typing/deleting run in progress
    bool m_gesture_saved = false;        // block drag pushed its snapshot
    nanogui::Vector2f m_drag_start;    // page point at press
    pagemade::Frame m_drag_frame;      // block at press

    bool m_show_baselines = false;
    bool m_show_loose_tight = false;
    bool m_show_guides = true;
};
