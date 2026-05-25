/*
    nanogui/datagrid.h -- Enterprise-quality virtual DataGrid widget

    Supports:
      - Virtual scrolling for very large datasets
      - SQL / file / in-memory models (lazy row access)
      - Pluggable cell editors (temporary positioned widgets)
      - Rich cell styling including Excel-style NumberFormat for Currency
      - Column resizing, reordering, visibility
      - Sorting + filtering hooks
      - Custom cell drawing callbacks
*/

#pragma once

#include <nanogui/widget.h>
#include <nanogui/layout.h>
#include <nanogui/theme.h>
#include <nanovg.h>
#include <any>
#include <functional>
#include <memory>
#include <string>
#include <vector>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// DataType - normalized types covering common SQL databases
// (SQLite, PostgreSQL, SQL Server, MySQL, etc.)
// ---------------------------------------------------------------------------
enum class DataType {
    Null = 0,
    Integer,    // BIGINT, INT, SMALLINT, etc.
    Double,     // REAL, FLOAT, DOUBLE, NUMERIC
    String,     // TEXT, VARCHAR, NVARCHAR, CHAR
    Boolean,    // BOOLEAN, BIT
    Date,       // DATE
    DateTime,   // TIMESTAMP, DATETIME, DATETIME2
    Time,       // TIME
    Blob,       // BLOB, VARBINARY, BYTEA
    Currency    // MONEY, DECIMAL, NUMERIC (money semantics)
};

// ---------------------------------------------------------------------------
// NumberFormat - Excel-style formatting for numeric / currency cells
// ---------------------------------------------------------------------------
struct NANOGUI_EXPORT NumberFormat {
    std::string symbol          = "";      // "$", "€", "£", etc.
    int         decimal_places  = 2;
    bool        use_grouping    = true;    // thousands separators (commas)
    bool        negative_red    = true;    // negatives in red
    bool        parentheses_neg = false;   // accounting style: (123.45)
};

// ---------------------------------------------------------------------------
// CellStyle - per-cell / per-column visual attributes
// Inspired by the Style struct in document.h
// ---------------------------------------------------------------------------
struct NANOGUI_EXPORT CellStyle {
    float        fontSize   = 16.0f;
    // fgColor / bgColor default to fully transparent which is a sentinel
    // meaning "inherit from the active Theme". Explicit non-zero alpha
    // overrides the theme.
    NVGcolor     fgColor    = { { { 0.0f, 0.0f, 0.0f, 0.0f } } };
    NVGcolor     bgColor    = { { { 0.0f, 0.0f, 0.0f, 0.0f } } };
    bool         bold       = false;
    bool         italic     = false;
    bool         underline  = false;
    bool         monospace  = false;

    // Horizontal + vertical alignment (use Alignment from layout.h where possible)
    Alignment    h_align    = Alignment::Middle;
    Alignment    v_align    = Alignment::Middle;

    // Only relevant for numeric / currency columns
    NumberFormat number_format;
};

// ---------------------------------------------------------------------------
// DataGridModel - abstract interface for virtualized data access
// Designed to work efficiently with SQL (LIMIT/OFFSET), large files, and memory
// ---------------------------------------------------------------------------
class NANOGUI_EXPORT DataGridModel {
public:
    virtual ~DataGridModel() = default;

    // --- Metadata (cheap to call) ---
    virtual size_t      row_count() const = 0;
    virtual size_t      column_count() const = 0;
    virtual std::string column_name(int column) const = 0;
    virtual DataType    column_type(int column) const = 0;

    // --- Data access (virtualized - only called for visible + prefetched rows) ---
    virtual std::any    get_value(int row, int column) const = 0;
    virtual CellStyle   get_cell_style(int row, int column) const { return CellStyle{}; }

    // Optional prefetch hint for SQL/file models
    virtual void        prefetch(int start_row, int count) const {}

    // --- Editing support ---
    virtual bool        is_cell_editable(int row, int column) const { return false; }
    virtual bool        set_value(int row, int column, const std::any& value) { return false; }

    // --- Sorting / Filtering (model can implement or return false) ---
    virtual bool        supports_sorting() const { return false; }
    virtual void        sort(int column, bool ascending) {}
    virtual bool        supports_filtering() const { return false; }
    virtual void        set_filter(const std::string& text) {}
};

// ---------------------------------------------------------------------------
// DataGridColumn - column descriptor with behavior and presentation options
// ---------------------------------------------------------------------------
struct NANOGUI_EXPORT DataGridColumn {
    std::string title;
    int         width            = 120;          // initial width in pixels
    int         min_width        = 30;           // floor for resize / autosize
    int         max_width        = 800;          // cap for resize / autosize
    bool        resizable        = true;
    bool        reorderable      = true;
    bool        visible          = true;
    bool        sortable         = false;

    DataType    type             = DataType::String;

    // Optional custom draw callback (bypasses built-in renderer)
    using CellDrawFunc = std::function<void(NVGcontext* ctx,
                                            const Vector2i& pos,
                                            const Vector2i& size,
                                            const std::any& value,
                                            const CellStyle& style,
                                            bool selected,
                                            bool focused)>;
    CellDrawFunc custom_draw;

    // Editor factory: called when editing begins. Must return a Widget* (or nullptr)
    // The returned widget will be positioned and sized over the cell and owned by the grid.
    using EditorFactory = std::function<Widget*(class DataGrid* grid,
                                                int row,
                                                int column,
                                                const std::any& current_value)>;
    EditorFactory editor_factory;
};

// ---------------------------------------------------------------------------
// DataGrid - the main widget
// ---------------------------------------------------------------------------
class NANOGUI_EXPORT DataGrid : public WidgetCRTP<DataGrid> {
public:
    DataGrid(Widget* parent);

    // --- Model binding ---
    void set_model(std::shared_ptr<DataGridModel> model);
    DataGridModel* model() const { return m_model.get(); }

    // --- Columns ---
    void set_columns(const std::vector<DataGridColumn>& columns);
    const std::vector<DataGridColumn>& columns() const { return m_columns; }
    void set_column_width(int column, int width);
    void set_column_visible(int column, bool visible);

    // --- Behavior ---
    void set_virtual_scrolling(bool enabled) { m_virtual_scrolling = enabled; }
    bool virtual_scrolling() const { return m_virtual_scrolling; }

    void set_sorting_enabled(bool enabled);
    void set_filtering_enabled(bool enabled);
    void set_editing_enabled(bool enabled) { m_editing_enabled = enabled; }

    // Selection
    enum class SelectionMode { None, Single, Multi, Extended };
    void set_selection_mode(SelectionMode mode) { m_selection_mode = mode; }

    // --- Scrolling (0..1 normalized, like ScrollPanel) ---
    Vector2f scroll() const { return m_scroll; }
    void set_scroll(const Vector2f& s);

    // --- Editing ---
    bool        is_editing() const { return m_editor != nullptr; }
    void        begin_edit(int row, int column);
    void        end_edit(bool commit = true);

    // --- Cell focus (single-cell selection) ---
    int         focused_row() const { return m_focused_row; }
    int         focused_col() const { return m_focused_col; }
    void        set_focused_cell(int row, int col);

    // --- Sort state ---
    int         sort_column() const { return m_sort_col; }
    bool        sort_ascending() const { return m_sort_ascending; }

    // --- Callbacks ---
    void set_cell_clicked_callback(std::function<void(int row, int col)> cb);
    void set_cell_edited_callback(std::function<void(int row, int col, const std::any& new_value)> cb);
    void set_sort_changed_callback(std::function<void(int column, bool ascending)> cb);

    // --- Standard Widget overrides ---
    Vector2i preferred_size(NVGcontext* ctx) const override;
    void perform_layout(NVGcontext* ctx) override;
    void draw(NVGcontext* ctx) override;

    bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override;
    bool mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override;
    bool mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override;
    bool scroll_event(const Vector2i& p, const Vector2f& rel) override;
    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

protected:
    // Internal helpers for virtualization and rendering
    int visible_row_count() const;
    int first_visible_row() const;
    void update_visible_range();

    // Column header interaction
    void draw_header(NVGcontext* ctx);
    void draw_body(NVGcontext* ctx);
    void draw_scrollbars(NVGcontext* ctx);

    // Editor management
    void position_editor();

    // Helpers for the new feature set
    std::string format_cell_text(const std::any& value, DataType type, const CellStyle& style) const;
    int         autosize_column(NVGcontext* ctx, int col);

private:
    std::shared_ptr<DataGridModel> m_model;
    std::vector<DataGridColumn>    m_columns;

    // Scrolling state (inspired by ScrollPanel)
    Vector2f   m_scroll{0.0f, 0.0f};
    float      m_vel_x = 0.0f;
    float      m_vel_y = 0.0f;
    double     m_last_t = 0.0;

    // Virtual scrolling
    bool       m_virtual_scrolling = true;
    int        m_first_visible_row = 0;
    float      m_row_scroll_offset = 0.0f;   // fractional rows for smooth scrolling
    int        m_visible_rows      = 0;      // includes prefetch
    int        m_fully_visible_rows = 0;     // exact rows that fit without cutoff

    // Interaction state
    int        m_hovered_row = -1;
    int        m_hovered_col = -1;
    int        m_pressed_header_col = -1;
    int        m_resizing_col = -1;
    int        m_hovered_resize_col = -1;   // column divider index being hovered for resize
    Vector2i   m_last_mouse_pos;

    // Scrollbar dragging
    bool       m_dragging_vscroll = false;
    float      m_drag_start_scroll = 0.0f;
    float      m_drag_start_mouse_y = 0.0f;

    // Current vertical scrollbar geometry (updated each draw)
    float      m_vscroll_track = 0.0f;
    float      m_vscroll_thumb_h = 0.0f;
    float      m_vscroll_thumb_y = 0.0f;

    // Horizontal scroll state
    bool       m_dragging_hscroll = false;
    float      m_drag_start_scroll_x = 0.0f;
    float      m_drag_start_mouse_x = 0.0f;
    float      m_hscroll_track = 0.0f;
    float      m_hscroll_thumb_w = 0.0f;
    float      m_hscroll_thumb_x = 0.0f;

    // Editing
    bool       m_editing_enabled = true;
    Widget*    m_editor = nullptr;
    int        m_edit_row = -1;
    int        m_edit_col = -1;

    // Selection
    SelectionMode m_selection_mode = SelectionMode::Single;
    std::vector<int> m_selected_rows;
    std::vector<int> m_selected_cols;  // populated by header clicks

    // Cell focus (a single highlighted cell, also where Enter-to-edit fires)
    int        m_focused_row = -1;
    int        m_focused_col = -1;

    // Sorting state (set by clicking a sortable header)
    int        m_sort_col       = -1;
    bool       m_sort_ascending = true;

    // Double-click detection
    double     m_last_click_time = 0.0;
    int        m_last_click_col  = -1;
    int        m_last_click_row  = -1;
    bool       m_last_click_header = false;
    bool       m_last_click_resize = false;

    // Callbacks
    std::function<void(int, int)>                           m_cell_clicked_cb;
    std::function<void(int, int, const std::any&)>          m_cell_edited_cb;
    std::function<void(int, bool)>                          m_sort_changed_cb;

    // Column reordering / resizing support
    std::vector<int> m_column_order;   // indices into m_columns
    bool             m_reordering = false;
    int              m_reorder_from = -1;
};

NAMESPACE_END(nanogui)
