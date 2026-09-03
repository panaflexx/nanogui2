/*
    nanogui/texteditor.h -- Editable multi-line text widget backed by
                            the rich-text Document model.

    The widget supports two operating modes:

      - Mode::RichText : renders the document with the wrapping /
                         justification layout from nanovg-colorfont-
                         atlas (Document::draw). Suitable for a
                         markdown / notes editor.

      - Mode::Code     : forces each paragraph to be one source line,
                         disables word-wrap, uses the monospace font
                         for all runs, and provides exact caret /
                         selection geometry suitable for a code editor.

    Editing in this widget is implemented at the Document level (Style
    / Text / Paragraph), so an outer layer can swap the runs in any
    paragraph at any time -- this is how syntax-highlighted spans are
    delivered (e.g. by a tree-sitter driver). See pieces/editor.c for
    the legacy ncurses version of that driver.
*/
#pragma once

#include <nanogui/widget.h>
#include <nanogui/document.h>
#include <functional>
#include <memory>
#include <string>

NAMESPACE_BEGIN(nanogui)

class NANOGUI_EXPORT TextEditor : public WidgetCRTP<TextEditor> {
public:
    enum class Mode { RichText, Code };

    /// A (paragraph, byte_column) location inside the document.
    struct Position {
        size_t paragraph = 0;
        size_t column    = 0; // BYTE offset within paragraph plain_text()
        bool operator==(const Position& o) const noexcept {
            return paragraph == o.paragraph && column == o.column;
        }
        bool operator!=(const Position& o) const noexcept { return !(*this == o); }
        bool operator<(const Position& o) const noexcept {
            return paragraph < o.paragraph ||
                  (paragraph == o.paragraph && column < o.column);
        }
        bool operator<=(const Position& o) const noexcept {
            return *this < o || *this == o;
        }
    };

    TextEditor(Widget* parent, Mode mode = Mode::Code);

    // -------------------------------------------------------------------
    // Document access
    // -------------------------------------------------------------------

    /// Shared document handle. Editing routines on this widget mutate
    /// the same instance; external code may also mutate it directly,
    /// but should then call `invalidate_layout()`.
    std::shared_ptr<Document>       document()       { return m_doc; }
    std::shared_ptr<const Document> document() const { return m_doc; }
    void set_document(std::shared_ptr<Document> doc);

    /// Replace the entire document contents with `text`. Each '\n' in
    /// `text` starts a new paragraph (code mode) or paragraph break
    /// (rich mode). If `style` is null the widget's mode-appropriate
    /// default (`code_style()` or `default_style()`) is used.
    void set_plain_text(const std::string& text, const Style* style = nullptr);
    std::string plain_text() const;

    // -------------------------------------------------------------------
    // Mode / appearance
    // -------------------------------------------------------------------

    Mode mode() const { return m_mode; }
    void set_mode(Mode m);

    void set_default_style(const Style& s) { m_default_style = s; }
    const Style& default_style() const { return m_default_style; }

    /**
     * \brief Rescale the whole RichText document to a new base font size.
     *
     * Every run's fontSize is multiplied by size / default_style().fontSize,
     * so headings and code blocks -- whose sizes were derived from the old
     * base via set_paragraph_header() / toggle_paragraph_code() -- keep their
     * proportions (e.g. an H1 at 1.625x base is still 1.625x the new base).
     * Also updates the baseline used for new insertions, so typing after the
     * change picks up the new size.
     */
    void set_base_font_size(float size);

    void set_code_style(const Style& s)    { m_code_style = s; }
    const Style& code_style() const        { return m_code_style; }

    /// Background color of the editor surface.
    void set_background_color(const Color& c) { m_bg_color = c; }
    const Color& background_color() const     { return m_bg_color; }

    /// Foreground color of the caret.
    void set_caret_color(const Color& c)      { m_caret_color = c; }
    /// Color of the selection rectangle (alpha-blended).
    void set_selection_color(const Color& c)  { m_selection_color = c; }
    /// Color used to highlight the current line in code mode.
    void set_current_line_color(const Color& c) { m_current_line_color = c; }
    /// Color of the line-number gutter background.
    void set_gutter_color(const Color& c)     { m_gutter_color = c; }
    /// Foreground color of the line-number text.
    void set_line_number_color(const Color& c){ m_line_number_color = c; }

    void set_show_line_numbers(bool v)        { m_show_line_numbers = v; }
    bool show_line_numbers() const            { return m_show_line_numbers; }

    void set_tab_width(int n)                 { m_tab_width = n > 0 ? n : 4; }
    int  tab_width() const                    { return m_tab_width; }

    void set_expand_tab(bool v)               { m_expand_tab = v; }
    bool expand_tab() const                   { return m_expand_tab; }

    void set_read_only(bool v)                { m_read_only = v; }
    bool read_only() const                    { return m_read_only; }

    void set_padding(int p)                   { m_padding = p; }
    int  padding() const                      { return m_padding; }

    // -------------------------------------------------------------------
    // Caret / selection
    // -------------------------------------------------------------------

    Position caret() const { return m_caret; }
    void set_caret(Position p, bool extend_selection = false);

    bool has_selection() const { return m_anchor != m_caret; }
    void clear_selection() { m_anchor = m_caret; }
    std::pair<Position, Position> selection() const;
    std::string selected_text() const;

    /// Select the word (or whitespace/punctuation run) under `p`.
    void select_word(Position p);
    /// Select the whole paragraph (without its trailing newline).
    void select_paragraph(size_t paragraph);
    /// Select the entire document.
    void select_all();

    void scroll_to_caret();

    // -------------------------------------------------------------------
    // High-level editing API (idempotent w.r.t. selection)
    // -------------------------------------------------------------------

    /// Insert plain text at the caret (replaces selection if any).
    void insert_text(const std::string& s);

    /// Delete the selection if any, otherwise delete the character to
    /// the left (`forward=false`) or right (`forward=true`) of caret.
    void delete_at_caret(bool forward);

    /// Insert a paragraph break at caret (Enter).
    void insert_newline();

    /// Insert one tab (`'\t'` or `tab_width` spaces, see `expand_tab`).
    void insert_tab();

    // -------------------------------------------------------------------
    // Rich-text style editing (WYSIWYG)
    // -------------------------------------------------------------------

    enum class StyleFlag { Bold, Italic, Underline, Monospace };

    /// Toggle a style flag.  With an active selection, the flag is applied
    /// to (removed from, when every run already has it) the selected range
    /// by splitting runs at the boundaries.  Without a selection, the flag
    /// flips a pending typing attribute that the next inserted text gets;
    /// the pending attribute is cancelled by any explicit caret move.
    void  toggle_style(StyleFlag flag);

    /// Style in effect at the caret (run style merged with any pending
    /// typing attributes) — drives toolbar button state.
    Style style_at_caret() const;

    /// Whether a pending typing attribute is active (see toggle_style).
    bool  typing_style_active() const { return m_typing_style_active; }

    /// Clear the pending typing attribute without changing text.
    void  clear_typing_style() { m_typing_style_active = false; }

    // -------------------------------------------------------------------
    // Paragraph-level formatting (RichText mode)
    // -------------------------------------------------------------------

    /// Set the caret paragraph's header level: 0 = body, 1..3 = "#".."###".
    /// Re-applies font size (scaled from default_style().fontSize by the
    /// same ratios the markdown renderer uses) and bold to every run of
    /// the paragraph.  Re-applying the active level resets to body text.
    void set_paragraph_header(int level);

    /// Header level of the caret paragraph (0 = body text).
    int  paragraph_header() const;

    /// Toggle the caret paragraph as a bullet list item ("- " in markdown).
    void toggle_paragraph_bullet();
    bool paragraph_bullet() const;

    /// Toggle the caret paragraph as a code block (monospace; serialized
    /// as a fenced ``` block in markdown).
    void toggle_paragraph_code();
    bool paragraph_code() const;

    // -------------------------------------------------------------------
    // External callbacks
    // -------------------------------------------------------------------

    /// Fired after every content-changing operation routed through this
    /// widget (insert/delete/newline/tab). Not fired when external code
    /// mutates the Document directly.
    std::function<void()> change_callback;

    /// Fired whenever the caret moves.
    std::function<void(Position)> caret_callback;

    /// Optional pre-key filter (Vim controllers etc.). Return true to
    /// indicate the key was fully handled and default behaviour should
    /// be skipped.
    std::function<bool(int key, int scancode, int action, int modifiers)>
        key_filter;

    /// Optional pre-character filter. Return true to skip default insert.
    std::function<bool(unsigned int codepoint)> character_filter;

    // -------------------------------------------------------------------
    // Widget overrides
    // -------------------------------------------------------------------

    bool mouse_button_event(const Vector2i& p, int button, bool down,
                            int modifiers) override;
    bool mouse_drag_event(const Vector2i& p, const Vector2i& rel,
                          int button, int modifiers) override;
    bool scroll_event(const Vector2i& p, const Vector2f& rel) override;
    bool focus_event(bool focused) override;
    bool keyboard_event(int key, int scancode, int action,
                        int modifiers) override;
    bool keyboard_character_event(unsigned int codepoint) override;
    void draw(NVGcontext* ctx) override;
    Vector2i preferred_size(NVGcontext* ctx) const override;

protected:
    // ---- geometry helpers (Code mode) ---------------------------------
    void   ensure_metrics(NVGcontext* ctx) const;
    float  line_height(NVGcontext* ctx) const;
    float  char_advance(NVGcontext* ctx) const;
    int    gutter_width(NVGcontext* ctx) const;
    size_t paragraph_count() const;
    size_t visual_col_for_byte(const std::string& line, size_t byte) const;
    size_t byte_for_visual_col(const std::string& line, size_t vcol) const;

    Position position_from_point(NVGcontext* ctx, const Vector2i& p) const;
    Vector2f point_for_position(NVGcontext* ctx, Position pos) const;

    // ---- editing primitives operating on the Document -----------------
    void   delete_range(Position a, Position b);
    void   insert_string(Position at, const std::string& s);
    void   fire_changed();

    // ---- code-mode drawing --------------------------------------------
    void   draw_code(NVGcontext* ctx);

    // ---- rich-text mode drawing ---------------------------------------
    void   draw_rich(NVGcontext* ctx);

protected:
    std::shared_ptr<Document> m_doc;
    Mode  m_mode;
    Style m_default_style;   // baseline style for rich mode insertions
    Style m_code_style;      // baseline style for code mode insertions

    Color m_bg_color;
    Color m_caret_color;
    Color m_selection_color;
    Color m_current_line_color;
    Color m_gutter_color;
    Color m_line_number_color;

    bool  m_show_line_numbers;
    bool  m_expand_tab;
    bool  m_read_only;
    int   m_tab_width;
    int   m_padding;

    // caret + selection (m_anchor == m_caret means no selection)
    Position m_caret;
    Position m_anchor;

    // multi-click selection state (1 = caret, 2 = word, 3 = paragraph, 4 = all)
    double  m_last_click_time = -1.0;
    int     m_click_count     = 0;
    Vector2i m_last_click_pos = Vector2i(0, 0);

    // Pending typing attributes (see toggle_style): when active, inserted
    // text gets m_typing_style instead of inheriting the containing run's.
    Style m_typing_style;
    bool  m_typing_style_active = false;

    // viewport offset in pixels (positive scrolls content up/left)
    float    m_scroll_x;
    float    m_scroll_y;

    // Inertia scroll
    float  m_vel_x     = 0.0f;  // horizontal velocity (px/s)
    float  m_vel_y     = 0.0f;  // vertical   velocity (px/s)
    double m_last_t    = 0.0;   // glfwGetTime() at last draw frame
    float  m_content_h            = 0.0f;  // cached document height (rich mode)
    bool   m_scroll_caret_pending = false; // scroll_to_caret() ran since last draw

    // cached metrics (updated lazily inside draw / hit-test)
    mutable float m_cached_line_h;
    mutable float m_cached_char_adv;
    mutable bool  m_metrics_valid;
};

NAMESPACE_END(nanogui)
