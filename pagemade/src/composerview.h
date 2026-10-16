/*
 * pagemade/composerview.h — ComposerView: pages on a pasteboard, items
 * drawn in stacking order, each story composed into its text blocks.
 * The pasteboard shows one page, a side-by-side spread, or every page
 * stacked. A click on a visible page turns to it.
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
 * depth. A move whose center lands on another page that is on screen
 * gives the item to that page. Delete removes the selection. Arrange:
 * Cmd/Ctrl+F brings the selection to the front, Cmd/Ctrl+B sends it to
 * the back, Cmd/Ctrl+] brings it forward one step and Cmd/Ctrl+[ sends it
 * backward one
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
#include "setup_adjust.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace pagemade { class BackgroundWorker; }

class ComposerView : public nanogui::Widget {
public:
    ComposerView(nanogui::Widget *parent, const pagemade::FontLibrary *fonts,
                 const pagemade::Hyphenator *hyphenator = nullptr);
    ~ComposerView() override;

    void set_document(pagemade::PageDoc doc);
    /* Edit freely, then call recompose(). */
    pagemade::PageDoc &document() { return m_doc; }
    void recompose();
    /* One per story, in PageDoc::stories order. */
    const std::vector<pagemade::Composition> &compositions() const { return m_comp; }

    void set_show_baselines(bool on)   { m_show_baselines = on; }
    void set_show_loose_tight(bool on) { m_show_loose_tight = on; }
    bool show_loose_tight() const { return m_show_loose_tight; }
    /* Strict: a line is loose or tight by its word spacing alone, before
     * letter spacing takes up the difference. */
    void set_strict_loose_tight(bool on) { m_strict_loose_tight = on; }
    bool strict_loose_tight() const { return m_strict_loose_tight; }
    void set_show_guides(bool on);
    bool show_guides() const           { return m_show_guides; }
    /* Locked ruler guides stay put: the pointer tool no longer picks them
     * up. New ones still come off the rulers, and the Clear commands work. */
    void set_lock_guides(bool on)      { m_lock_guides = on; }
    bool lock_guides() const           { return m_lock_guides; }
    void set_snap(bool on)             { m_snap = on; }
    bool snap() const                  { return m_snap; }

    /* ---- Ruler guides ----------------------------------------------- *
     * Dragged off a ruler onto the pages: the guide lands on the page
     * under the pointer, or on every page with Shift (purple; a page's
     * own guides are blue). The pointer tool drags a guide to move it, and
     * back off the pages (onto a ruler) to remove it. Each is one undo
     * step. Points are on screen, as the rulers see them. */
    void begin_guide_drag(bool vertical);
    void guide_drag_to(const nanogui::Vector2i &screen_p, bool all_pages);
    void end_guide_drag(const nanogui::Vector2i &screen_p, bool all_pages);
    bool guide_dragging() const { return m_guide_drag.active; }
    /* Remove the current page's guides, or the every-page guides. */
    void clear_guides(bool all_pages);
    /* The current page's top-left on screen and pixels per point, for the
     * rulers. False before the view has a size. */
    bool ruler_mapping(nanogui::Vector2f &origin, float &scale) const;

    /* The toolbox. */
    enum class Tool { Pointer, Crop, Rotate, Text, Line, Rect, Ellipse, Polygon };
    void set_tool(Tool t);
    Tool tool() const { return m_tool; }
    bool editing() const { return edit_story() != nullptr; }
    /* Insert text at the caret as its own undo step (the Insert Character
     * menu). False when no text is being edited. */
    bool insert_at_caret(const std::string &utf8);
    /* Typographer's quotes as you type (on by default). */
    bool smart_quotes() const { return m_smart_quotes; }
    void set_smart_quotes(bool on) { m_smart_quotes = on; }

    /* Element > Caption Placed Pictures: a placed picture (dropped, or
     * File > Place) gets a text block below it in the Caption style,
     * holding the file name. Part of the placement's undo step. */
    bool caption_pictures() const { return m_caption_pictures; }
    void set_caption_pictures(bool on) { m_caption_pictures = on; }

    /* Element > Fit Placed Pictures to Columns: a placed picture takes the
     * width of the column it was dropped in (the first column for File >
     * Place), keeping its proportions; its caption takes the same width. */
    bool fit_to_columns() const { return m_fit_columns; }
    void set_fit_to_columns(bool on) { m_fit_columns = on; }

    /* Picture bytes live outside the document (undo snapshots stay small).
     * The view does not own the store. */
    void set_image_store(pagemade::ImageStore *images);
    /* Place a file: one undo step, selected, then back to the pointer.
     * The id is taken before the file is read, and a failure does not
     * reuse it. False leaves the document unchanged. This form reads on
     * the caller (the command line). The other reads on a background
     * thread and runs `done` on the UI thread; an empty error placed it.
     * A null point is the margin of `page`. */
    bool place_image_file(const std::string &path, std::string *error);
    /* The frame goes down at once, as a stand-in sized from `px_hint` (the
     * picture's pixel size, when the caller knows it); the file's header
     * is read on a background thread, then the frame takes the picture's
     * print size (unless it was resized meanwhile) and the picture loads
     * like any linked one. `done` reports a file that can't be read. */
    void place_image_file(const std::string &path, size_t page, const pagemade::Point *at,
                          std::function<void(const std::string &error)> done,
                          nanogui::Vector2i px_hint = nanogui::Vector2i(0, 0));
    /* One undo step that adds a picture the store already holds, without
     * placing it. */
    void add_asset(pagemade::ImageAsset asset);
    /* Read `path` off the UI thread and add_asset it. */
    void import_asset(const std::string &path, std::function<void(const std::string &error)> done);
    /* Another frame of a picture the publication already has, at the margin
     * or with its top-left at `at` on the current page. */
    void place_image(uint32_t asset);
    void place_image(uint32_t asset, const pagemade::Point &at);
    /* The same at a point: its top-left corner at `at` on `page`. */
    void place_image_at(uint32_t asset, size_t page, const pagemade::Point &at);
    /* Take out every picture that is not placed. One undo step, and a
     * no-op when every picture is placed. */
    void remove_unused_images();

    /* Pictures in a frame on screen as of the last draw. Linked pictures
     * are read when they become live, on a background thread. */
    const std::set<uint32_t> &live_images() const { return m_live; }
    std::function<void()> on_live_change;
    /* Bytes of linked pictures kept loaded off screen before the ones off
     * screen longest are unloaded. */
    void set_image_budget(size_t bytes) { m_image_budget = bytes; }

    /* Drag and drop. A publication picture (kImageAssetDragType) is placed
     * where it drops. Files ("text/uri-list") go to on_drop_files with the
     * drop point and, from the asset window, the first file's pixel size
     * (kImagePixelsDragType; 0 x 0 if unknown). */
    std::function<void(const std::vector<std::string> &paths, size_t page,
                       pagemade::Point at, nanogui::Vector2i px_hint)> on_drop_files;
    bool drag_motion_event(const nanogui::Vector2i &p, nanogui::DragEvent &e) override;
    void drag_leave_event() override;
    bool drag_drop_event(const nanogui::Vector2i &p, nanogui::DragEvent &e) override;

    /* The selection, in the order items were picked. */
    const std::vector<pagemade::ItemId> &selection() const { return m_sel; }
    void select(pagemade::ItemId id);   // 0 clears
    /* Select a story's text block by position: story index, thread index. */
    void select_frame(size_t story, size_t frame);

    /* ---- Pages ------------------------------------------------------ */
    /* Hidden pages can be shown and edited; printing skips them.
     * PageView chooses one page, the spread that holds the current page,
     * or a stack of every page. */
    size_t page_index() const { return m_page; }
    size_t page_count() const { return m_doc.pages.size(); }
    bool page_hidden(size_t i) const {
        return i < m_doc.pages.size() && m_doc.pages[i].hidden;
    }
    void set_page_view(pagemade::PageView view);
    pagemade::PageView page_view() const { return m_page_view; }
    /* Top-left of page `index` in the arrangement (0, 0 when it is off screen). */
    nanogui::Vector2f page_slot_offset(size_t index) const;
    /* The rectangle Fit Page should frame: one page, or the whole spread. */
    void fit_extent(float &width, float &height) const;
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

    /* ---- Layers (the Layers panel) ----------------------------------- *
     * Every change is one undo step except choosing the active layer.
     * Hiding or locking a layer drops its items from the selection. */
    const std::vector<pagemade::Layer> &layers() const { return m_doc.layers; }
    pagemade::LayerId active_layer() const { return m_doc.active_layer; }
    void set_active_layer(pagemade::LayerId id);
    /* False at kMaxLayers. */
    bool add_layer(const std::string &name);
    void remove_layer(pagemade::LayerId id);
    void move_layer(pagemade::LayerId id, int dir);
    void rename_layer(pagemade::LayerId id, const std::string &name);
    void set_layer_visible(pagemade::LayerId id, bool visible);
    void set_layer_locked(pagemade::LayerId id, bool locked);
    /* The selected items onto a layer (whole groups go together). */
    void move_selection_to_layer(pagemade::LayerId id);
    /* How many items a layer holds, on all pages. */
    size_t layer_item_count(pagemade::LayerId id) const;

    /* ---- Document Setup and PDF settings ---------------------------- */
    /* New page size, margins, columns, facing or bleed, with the placed
     * items adjusted, scaled or left (setup_adjust.h). One undo step. */
    void change_setup(const pagemade::PageSetup &setup, pagemade::SetupAdjust how);
    /* How the publication exports (kept in the file; not an undo step). */
    void set_pdf_settings(const pagemade::PdfSettings &settings);
    /* The page size or facing pages changed (Document Setup, or undoing
     * it): the pasteboard is a new size. */
    std::function<void()> on_setup_change;

    /* ---- Right-click menu ------------------------------------------- *
     * A right-click first selects what is under the pointer (its whole
     * group), or clears the selection over empty space, then asks the app
     * for a menu at `screen_pos`. While text is being edited the selection
     * stays as it is, and text_context() is true. */
    std::function<void(const nanogui::Vector2i &screen_pos)> on_context_menu;
    bool text_context() const { return editing(); }
    /* The page point the menu was asked for (Paste puts things there). */
    pagemade::Point context_point() const { return m_context_pt; }

    /* ---- Clipboard ------------------------------------------------- *
     * While editing text these work on the text (and the system
     * clipboard). Otherwise on items: copies keep their text, pictures,
     * groups and the threading among them, and paste onto the current
     * page's active layer, selected. `at` puts the copies' top-left there;
     * without it they land a little below and right of the originals. */
    void edit_cut();
    void edit_copy();
    /* Delete the selected items (Remove), one undo step. */
    void remove_selection() { delete_selection_items(); }
    void edit_paste(const pagemade::Point *at = nullptr);
    bool can_copy() const;
    bool can_paste() const;

    /* ---- Align (the selection's items and groups, each as a unit) --- */
    enum class AlignTo { Left, Center, Right, Top, Middle, Bottom };
    /* Two or more units: items, or whole groups. */
    bool can_align() const;
    void align_selection(AlignTo how);

    /* ---- Pictures -------------------------------------------------- */
    /* Some selected picture has a file to read again. */
    bool can_reload_pictures() const;
    /* Read the selected pictures' files again (changed on disk). */
    void reload_pictures();

    /* ---- Groups (Element > Group / Ungroup) ------------------------- */
    /* Two or more items on one page, not already all one group. */
    bool can_group() const;
    /* Some selected item is grouped. */
    bool can_ungroup() const;
    void group_selection();
    void ungroup_selection();

    /* ---- Control palette ------------------------------------------- */
    /* X/Y are the item's origin (its box's top-left corner, turned with
     * it) from the page's top-left, W/H its size, angle in degrees
     * counterclockwise (PageMaker's convention). One selected item only. */
    struct Geometry {
        float x = 0, y = 0, w = 0, h = 0, angle = 0;
    };
    bool geometry(Geometry &g) const;
    void set_geometry(const Geometry &g);       // one undo step
    /* The fill/stroke the palette shows: the first selected shape or
     * picture, or the defaults for new shapes when neither is selected. A
     * picture shows as no fill at its opacity (the fill tint) and its border. */
    pagemade::Shape shape_style() const;
    bool shapes_selected() const;
    bool pictures_selected() const;
    /* Change the selected shapes (one undo step), or the defaults for new
     * shapes when none is selected, as PageMaker does. */
    void apply_shape_style(const std::function<void(pagemade::Shape &)> &fn);
    /* The same, also changing selected pictures with `image` (one undo step
     * for both). With neither selected, `shape` sets the shape defaults. */
    void apply_item_style(const std::function<void(pagemade::Shape &)> &shape,
                          const std::function<void(pagemade::PlacedImage &)> &image);
    /* The Colors palette (PageDoc::swatches), each one undo step.
     * add_doc_swatch appends a swatch and returns its id. update_doc_swatch
     * renames and recolors one in place: everything painted with it follows.
     * remove_doc_swatch drops a custom swatch and repaints its users black;
     * built-ins are refused. */
    pagemade::SwatchId add_doc_swatch(const std::string &name, pagemade::Color rgb,
                                      pagemade::Cmyk cmyk);
    bool update_doc_swatch(pagemade::SwatchId id, const std::string &name,
                           pagemade::Color rgb, pagemade::Cmyk cmyk);
    bool remove_doc_swatch(pagemade::SwatchId id);

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
        bool  kerning = true, ligatures = true, no_break = false;
        float size = 12.f, leading = 0.f, baseline = 0.f;
        float tracking = 0.f, hscale = 100.f;
        bool  mix_family = false, mix_style = false, mix_size = false;
        bool  mix_leading = false, mix_baseline = false;
        bool  mix_caps = false, mix_deco = false;
        bool  mix_track = false, mix_hscale = false;
        bool  mix_kerning = false, mix_ligatures = false, mix_no_break = false;
        /* Color (a swatch and tint; swatch 0 = the RGB) and the character
         * style; `char_overridden`: the text differs from that style. */
        uint32_t swatch = 0;
        float tint = 100.f;
        pagemade::Color rgb;
        std::string char_style;
        bool  mix_color = false, mix_tint = false, mix_char_style = false;
        bool  char_overridden = false;
    };
    bool type_style(TypeStyle &t) const;
    /* `undo` false: the caller has already made the undo step. */
    void apply_type(const std::function<void(pagemade::CharStyle &)> &fn, bool undo = true);

    /* Character styles (PageDoc::char_styles), each one undo step.
     * apply_char_style sets the text apply_type would change in a style;
     * an empty name drops the style and keeps the formatting.
     * define_char_style makes one from the selection: what its first
     * character changes from its paragraph's own type. Redefining a name
     * in use updates its runs (those without overrides). delete_char_style
     * drops a style; its runs keep their formatting. */
    void apply_char_style(const std::string &name);
    void define_char_style(const std::string &name);
    void delete_char_style(const std::string &name);

    /* Paragraph alignment of the selected lines. With the text tool, that
     * is the caret's line or the lines the selection touches, each whole
     * line. With text blocks selected, it is the lines in those blocks.
     * False when there is no such text. `mixed` when the lines disagree. */
    bool text_align(pagemade::Align &align, bool &mixed) const;
    /* Apply `align` there. A line that is only part of its paragraph is
     * split off, so the rest of the paragraph is left as it was. One undo
     * step. Does nothing when text_align would return false. */
    void apply_align(pagemade::Align align);

    /* The other paragraph attributes (spacing, indents) belong to whole
     * paragraphs: the ones the alignment lines touch. Empty when no text
     * is selected. apply_para changes them all, as one undo step. */
    std::vector<const pagemade::Paragraph *> selected_paragraphs() const;
    void apply_para(const std::function<void(pagemade::ParaStyle &)> &fn);

    /* Named paragraph styles (PageDoc::styles), each one undo step.
     * apply_style sets the selected paragraphs in a style. define_style
     * makes one from the first selected paragraph and tags the selection
     * with it; redefining a name in use updates the paragraphs that follow
     * it (those without overrides). delete_style drops a style; its
     * paragraphs keep their formatting. */
    void apply_style(const std::string &name);
    void define_style(const std::string &name);
    void delete_style(const std::string &name);

    /* Undo/redo: whole-document snapshots (the model is small), one per
     * action. A run of typing or deleting coalesces into a single step;
     * caret moves and other actions close the run. View state (zoom) is
     * not undoable. */
    void push_undo();                  // snapshot the document as it is now
    void will_edit(bool burst);        // call before mutating the story
    /* Palette spinners: while the change made with `key` set follows the
     * last one made with the same key, and nothing else happened between,
     * it joins that undo step. Set it around one change; nullptr after. */
    void set_undo_merge(const void *key) { m_merge_key = key; }
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
    /* A short message for the status bar (a refused edit). */
    std::function<void(const std::string &)> on_note;

    /* Top-left of the arrangement in view units; the rest is pasteboard. */
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
        uint32_t group = 0;              // a corner of this selected group's box
    };
    /* Page bounds of each group whose members are all selected, on `page`. */
    std::map<uint32_t, pagemade::Bounds> selected_groups(size_t page) const;
    /* Scale a group's members with its box, a corner dragged by `d`. */
    void resize_group(pagemade::Point d, int modifiers);

    /* Event points arrive rounded to whole points and, during drags, without
     * the panel's zoom applied. Both go through this instead, from the
     * screen's mouse position, at full precision. */
    pagemade::Point page_point_from_screen(const nanogui::Vector2i &screen_p) const;
    /* `local` is view space, origin at the widget's top-left. The point
     * comes back in the page under the cursor, or the page the gesture
     * started on while the button is down. */
    pagemade::Point page_point_from_local(const nanogui::Vector2f &local) const;
    float zoom() const;
    /* Handles of selected items first, then items top to bottom. */
    Hit hit_test(const pagemade::Point &pt, size_t page) const;
    /* Topmost text block under pt (0 = none). */
    pagemade::ItemId text_frame_at(const pagemade::Point &pt, size_t page) const;
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
    /* Items on a hidden or locked layer leave the selection; a story with
     * no editable block stops being edited. */
    void drop_uneditable();
    /* New items can go on the active layer; otherwise says why (on_note). */
    bool check_active_layer();
    /* `ids` and everything grouped with them, without repeats. */
    std::vector<pagemade::ItemId> with_groups(const std::vector<pagemade::ItemId> &ids) const;
    void selection_changed();
    pagemade::Bounds selection_bounds() const;
    /* The composition of the story threading text frame `id`, and the
     * frame's place in the thread. */
    const pagemade::Composition *comp_of_frame(pagemade::ItemId id, size_t *thread_index) const;

    /* ---- Snapping ---------------------------------------------------- */
    /* Margins, columns and ruler guides of the current page, or of `page` */
    std::vector<float> guides_x() const { return guides_x(m_page); }
    std::vector<float> guides_y() const { return guides_y(m_page); }
    std::vector<float> guides_x(size_t page) const;
    std::vector<float> guides_y(size_t page) const;
    /* Page edges, margins and columns of `page` (what a ruler guide snaps to) */
    std::vector<float> base_guides_x(size_t page) const;
    std::vector<float> base_guides_y() const;

    /* ---- Ruler guides (see the public section) ----------------------- */
    struct GuideDrag {
        bool   active = false;
        bool   vertical = true;
        bool   all_pages = false;  // on every page (doc guides) or one page
        int    index = -1;         // the guide being moved; -1 for a new one
        size_t page = 0;           // the page its position is measured on
        float  pos = 0;
        bool   over = false;       // over the pages (else a drop cancels or removes)
    };
    /* View-space point (widget top-left origin) of a screen point */
    nanogui::Vector2f local_from_screen(const nanogui::Vector2i &screen_p) const;
    /* A screen point in `page`'s coordinates */
    pagemade::Point point_on_page(const nanogui::Vector2i &screen_p, size_t page) const;
    bool screen_over_pages(const nanogui::Vector2i &screen_p) const;
    /* The ruler guide under `pt` on `page`: its index, -1 if none */
    int guide_at(const pagemade::Point &pt, size_t page, bool &all_pages) const;
    void draw_ruler_guides(NVGcontext *ctx, float px, size_t page);
    /* Snap a page point to the guides, axis by axis. */
    pagemade::Point snap_point(pagemade::Point p) const;
    /* Adjust a move so one of the box's edges (or its center) lands on a guide. */
    pagemade::Point snap_move(const pagemade::Bounds &b, pagemade::Point d) const;

    void draw_page(NVGcontext *ctx, float px, size_t page);
    /* Loose/tight lines, baselines, the text selection and the caret. */
    void draw_text_overlays(NVGcontext *ctx, const pagemade::Item &it, float px);
    /* Outlines, handles, windowshades, marquee. `which` skips those items,
     * or draws only them when `only` is set. The marquee is part of the
     * full pass, not the "only these" pass. */
    void draw_chrome(NVGcontext *ctx, float px, size_t page,
                     const std::vector<pagemade::ItemId> *which = nullptr, bool only = false);
    /* Items in the hand: a move drag, or a story picked up to thread.
     * Drawn after every page so the next page's paper cannot cover them. */
    void lifted_ids(std::vector<pagemade::ItemId> &ids) const;
    /* A finished move whose center lies on another visible page. */
    void adopt_moved_items();

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
    /* A picture's frame: at the margin of the current page, or with its
     * top-left at `at` on `page`. */
    void place_frame(uint32_t asset, const pagemade::ImageMetadata &meta,
                     size_t page, const pagemade::Point *at = nullptr);
    /* The caption block under `picture` (on `page`), or 0 when captions
     * are off. Its story is one Caption paragraph of the picture's name. */
    pagemade::ItemId add_caption(size_t page, const pagemade::Item &picture);
    /* The depth a new caption takes below its picture (gap included), or 0
     * when captions are off. */
    float caption_depth() const;
    /* A column of `page`: its left edge and width. `x` picks the column it
     * falls in (the nearest one from a gutter or margin); null, the first. */
    void column_at(size_t page, const float *x, float &col_x, float &col_w) const;
    /* A picture of `aspect` (height over width) fitted to a column: as wide
     * as the column, or narrower and centered when that would be taller
     * than the page's live area. */
    void fit_column(size_t page, float col_x, float col_w, float aspect,
                    float &x, float &w, float &h) const;
    /* Keep a caption under its picture after the picture's frame changed size. */
    void fit_caption(pagemade::ItemId caption, const pagemade::Item &picture);
    /* Store the bytes (or a link), stamp the asset, one undo step, and
     * optionally place it. */
    bool commit_import(uint32_t id, const std::string &path, pagemade::ImageImport imported,
                       size_t page, bool place, const pagemade::Point *at, std::string *error);
    bool key_held(int modifiers, int mod, int key_left, int key_right) const;

    /* Picture loading: see "Picture loading" in composerview.cpp */
    struct LoadResult;
    pagemade::BackgroundWorker &loader();     // display views (reads, decodes)
    pagemade::BackgroundWorker &importer();   // Place and import reads
    void request_display(uint32_t id, int long_edge);
    /* A file placed before its header was read (place_image_file) */
    struct Probing {
        pagemade::ItemId item = 0;
        pagemade::ItemId caption = 0;    // its caption, which follows the resize
        bool column = false;             // fitted to the column at col_x, col_w
        float col_x = 0, col_w = 0;
        float w = 0, h = 0;              // the stand-in frame's size
    };
    pagemade::ItemId m_last_caption = 0; // set by place_frame
    pagemade::Point m_context_pt;         // where the last right-click was
    pagemade::PageSetup m_shown_setup;    // the setup the pasteboard was laid out for
    void check_setup_change();            // tell on_setup_change when it differs
    /* Items copied (Cut, Copy): their stories with only the copied blocks
     * threaded, the pictures they show, and where they were. */
    struct ItemClip {
        std::vector<pagemade::Item> items;           // stacking order
        std::vector<pagemade::StoryEntry> stories;   // threads name copied ids
        std::vector<pagemade::ImageAsset> assets;
        pagemade::Bounds bounds;
        size_t page = 0;
        int pastes = 0;                              // each Ctrl+V steps further
    };
    ItemClip m_clip;
    void text_clipboard(int key);         // Cut, Copy or Paste of edited text
    bool m_last_column = false;          // ... and the column it fitted, if any
    float m_last_col_x = 0, m_last_col_w = 0;
    void finish_probe(uint32_t id, const pagemade::ImageImport &imported,
                      const std::function<void(const std::string &)> &done);
    void finish_display(uint32_t id, const std::string &path, LoadResult &r);
    void update_live_images();
    void drop_texture(uint32_t id);
    bool frame_on_screen(NVGcontext *ctx, const pagemade::DrawImage &im) const;
    /* Print size of a picture on `page`, shrunk to fit inside the margins. */
    void frame_size(const pagemade::ImageMetadata &meta, size_t page, float &w, float &h) const;
    /* The asset id a drag offers, 0 if none. */
    uint32_t dragged_asset(const nanogui::DragData &data) const;
    /* Where a drag would place its frame: the page under the pointer, the
     * top-left there (snapped), and the frame's size (0 while unknown) */
    void drop_target(const nanogui::DragData &data, pagemade::Point &at, size_t &page,
                     float &w, float &h) const;

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
    /* The selection's first character and its paragraph's base type. */
    bool selection_char_style(pagemade::CharStyle &cs, pagemade::CharStyle &base) const;
    /* (story index, paragraph index) for selected_paragraphs. */
    std::vector<std::pair<size_t, size_t>> selected_para_indices() const;

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
    bool m_smart_quotes = true;
    bool m_caption_pictures = false;
    bool m_fit_columns = false;
    void move_caret_key(int key, int modifiers);
    void toggle_style(int key);           // B -> bold, I -> italic

    const pagemade::FontLibrary *m_fonts;
    const pagemade::Hyphenator *m_hyphenator;
    pagemade::PageDoc m_doc;
    size_t m_page = 0;                    // the page being edited
    pagemade::PageView m_page_view = pagemade::PageView::One;
    bool m_pointer_down = false;          // a drag stays on the page it started
    size_t m_gesture_page = 0;
    mutable size_t m_point_page = 0;      // page page_point_from_local resolved
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
    uint32_t m_drag_group = 0;            // ... or the group whose box corner is
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
    const void *m_merge_key = nullptr;    // set_undo_merge
    const void *m_merged_key = nullptr;   // key of the newest undo step
    uint64_t m_merged_version = 0;        // the version that step left
    /* The next change continues a spin run (joins the last undo step). */
    bool continues_run() const {
        return m_merge_key && m_merge_key == m_merged_key && m_version == m_merged_version;
    }
    /* A spin run's targets, fixed at its first step: with a text block
     * selected, a step can push text out of the block (or pull more in),
     * and the rest of the run must change the same text. */
    struct TypeRange { size_t story; pagemade::TextPos a, b; };
    std::vector<TypeRange> m_run_type;
    std::vector<std::pair<size_t, size_t>> m_run_paras;
    bool m_gesture_saved = false;         // drag pushed its snapshot
    pagemade::Point m_drag_start;         // page point at press
    std::vector<pagemade::Item> m_drag_items;   // the selected items at press
    pagemade::Bounds m_drag_bounds;       // their page bounds at press

    bool m_show_baselines = false;
    bool m_show_loose_tight = false;
    bool m_strict_loose_tight = false;
    bool m_show_guides = true;
    GuideDrag m_guide_drag;
    bool m_lock_guides = false;

    pagemade::ImageStore *m_images = nullptr;
    /* Cleared in the destructor so a load still in flight does not touch
     * the view after it is gone; m_doc_epoch drops one meant for a
     * document that has since been replaced. */
    struct LoadGate { std::atomic<bool> open{true}; };
    std::shared_ptr<LoadGate> m_load_gate = std::make_shared<LoadGate>();
    std::unique_ptr<pagemade::BackgroundWorker> m_loader, m_importer;
    uint64_t m_doc_epoch = 0;
    std::map<uint32_t, int> m_load_want;      // display edge asked for, per picture
    std::set<uint32_t> m_load_failed;         // couldn't be read: not asked again
    std::map<uint32_t, Probing> m_probing;    // placed, header still being read
    std::set<uint32_t> m_live, m_live_frame;  // on screen: last draw, this draw
    std::map<uint32_t, uint64_t> m_last_live; // draw tick each was last on screen
    uint64_t m_live_tick = 0;
    size_t m_image_budget = (size_t) 512 << 20;

    /* A drag over the view: where it would land, and the frame's size (0
     * while unknown: files are only probed on drop) */
    bool m_drop_hover = false;
    size_t m_drop_page = 0;
    pagemade::Point m_drop_at;
    float m_drop_w = 0.f, m_drop_h = 0.f;
    /* Screen copies of display views. Freed when the store's generation
     * changes, not from the destructor: the screen may already be gone. */
    struct Tex { int id = 0; int w = 0, h = 0; };
    std::map<uint32_t, Tex> m_tex;
    uint64_t m_tex_gen = 0;
};
