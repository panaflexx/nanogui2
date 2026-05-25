/*
    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/**
 * \file nanogui/layout.h
 *
 * \brief A collection of useful layout managers.  The \ref nanogui::GridLayout
 *        was contributed by Christian Schueller.
 */

#pragma once

#include <nanogui/object.h>
#include <nanogui/vector.h>
#include <nanogui/widget.h>
#include <stdexcept>
#include <unordered_map>
#include <vector>

NAMESPACE_BEGIN(nanogui)

/// The different kinds of alignments a layout can perform.
enum class Alignment : uint8_t {
    Minimum = 0, ///< Take only as much space as is required.
    Middle,      ///< Center align.
    Maximum,     ///< Take as much space as is allowed.
    Fill         ///< Fill according to preferred sizes.
};

/// The direction of data flow for a layout.
enum class Orientation {
    Horizontal = 0, ///< Layout expands on horizontal axis.
    Vertical        ///< Layout expands on vertical axis.
};

/**
 * \class Layout layout.h nanogui/layout.h
 *
 * \brief Basic interface of a layout engine.
 */

struct TableTheme {
	Color header_background = Color(180, 180, 180, 255);
	Color header_text_color = Color(0, 0, 0, 255);
	std::string header_font = "sans-bold";
	int header_font_size = -1; // -1 means use default
	Color even_row_background = Color(250, 250, 250, 255);
	Color odd_row_background = Color(245, 245, 245, 255);
	Color even_column_background = Color(250, 250, 250, 255);
	Color odd_column_background = Color(245, 245, 245, 255);
	Color border_color = Color(200, 200, 200, 255);
	float border_width = 1.0f;
	bool shade_rows = true;
	bool shade_columns = false;
	bool draw_borders = true;
	bool first_row_is_header = true;
	bool first_column_is_header = false;
};

class NANOGUI_EXPORT Layout : public Object {
public:
    Widget* SizeDebugPointer;
    /**
     * Performs applies all layout computations for the given widget.
     *
     * \param ctx
     *     The ``NanoVG`` context being used for drawing.
     *
     * \param widget
     *     The Widget whose child widgets will be positioned by the layout class..
     */
    virtual void perform_layout(NVGcontext *ctx, Widget *widget) const = 0;

    /**
     * Compute the preferred size for a given layout and widget
     *
     * \param ctx
     *     The ``NanoVG`` context being used for drawing.
     *
     * \param widget
     *     Widget, whose preferred size should be computed
     *
     * \return
     *     The preferred size, accounting for things such as spacing, padding
     *     for icons, etc.
     */
    virtual Vector2i preferred_size(NVGcontext *ctx, const Widget *widget) const = 0;

    virtual void enable_draw_table(const TableTheme &theme = TableTheme()) {
        m_draw_table = true;
        m_table_theme = theme;
    }

    virtual void disable_draw_table() {
        m_draw_table = false;
    }

	virtual void draw_table(NVGcontext *ctx, const Widget *widget) {}

protected:
    /// Default destructor (exists for inheritance).
    virtual ~Layout() { }

	bool m_draw_table = false;
    TableTheme m_table_theme;
};

/**
 * \class BoxLayout layout.h nanogui/layout.h
 *
 * \brief Simple horizontal/vertical box layout
 *
 * This widget stacks up a bunch of widgets horizontally or vertically. It adds
 * margins around the entire container and a custom spacing between adjacent
 * widgets.
 */
class NANOGUI_EXPORT BoxLayout : public Layout {
public:
    /**
     * \brief Construct a box layout which packs widgets in the given \c Orientation
     *
     * \param orientation
     *     The Orientation this BoxLayout expands along
     *
     * \param alignment
     *     Widget alignment perpendicular to the chosen orientation
     *
     * \param margin
     *     Margin around the layout container
     *
     * \param spacing
     *     Extra spacing placed between widgets
     */
    BoxLayout(Orientation orientation, Alignment alignment = Alignment::Middle,
              int margin = 0, int spacing = 0);

    /// The Orientation this BoxLayout is using.
    Orientation orientation() const { return m_orientation; }

    /// Sets the Orientation of this BoxLayout.
    void set_orientation(Orientation orientation) { m_orientation = orientation; }

    /// The Alignment of this BoxLayout.
    Alignment alignment() const { return m_alignment; }

    /// Sets the Alignment of this BoxLayout.
    void set_alignment(Alignment alignment) { m_alignment = alignment; }

    /// The margin of this BoxLayout.
    int margin() const { return m_margin; }

    /// Sets the margin of this BoxLayout.
    void set_margin(int margin) { m_margin = margin; }

    /// The spacing this BoxLayout is using to pad in between widgets.
    int spacing() const { return m_spacing; }

    /// Sets the spacing of this BoxLayout.
    void set_spacing(int spacing) { m_spacing = spacing; }

    /* Implementation of the layout interface */

    /// See \ref Layout::preferred_size.
    virtual Vector2i preferred_size(NVGcontext *ctx, const Widget *widget) const override;

    /// See \ref Layout::perform_layout.
    virtual void perform_layout(NVGcontext *ctx, Widget *widget) const override;

protected:
    /// The Orientation of this BoxLayout.
    Orientation m_orientation;

    /// The Alignment of this BoxLayout.
    Alignment m_alignment;

    /// The margin padding of this BoxLayout.
    int m_margin;

    /// The spacing between widgets of this BoxLayout.
    int m_spacing;
};

/**
 * \class GroupLayout layout.h nanogui/layout.h
 *
 * \brief Special layout for widgets grouped by labels.
 *
 * This widget resembles a box layout in that it arranges a set of widgets
 * vertically. All widgets are indented on the horizontal axis except for
 * \ref Label widgets, which are not indented.
 *
 * This creates a pleasing layout where a number of widgets are grouped
 * under some high-level heading.
 */
class NANOGUI_EXPORT GroupLayout : public Layout {
public:
    /**
     * Creates a GroupLayout.
     *
     * \param margin
     *     The margin around the widgets added.
     *
     * \param spacing
     *     The spacing between widgets added.
     *
     * \param group_spacing
     *     The spacing between groups (groups are defined by each Label added).
     *
     * \param group_indent
     *     The amount to indent widgets in a group (underneath a Label).
     */
    GroupLayout(int margin = 15, int spacing = 6, int group_spacing = 14,
                int group_indent = 20)
        : m_margin(margin), m_spacing(spacing), m_group_spacing(group_spacing),
          m_group_indent(group_indent) {}

    /// The margin of this GroupLayout.
    int margin() const { return m_margin; }

    /// Sets the margin of this GroupLayout.
    void set_margin(int margin) { m_margin = margin; }

    /// The spacing between widgets of this GroupLayout.
    int spacing() const { return m_spacing; }

    /// Sets the spacing between widgets of this GroupLayout.
    void set_spacing(int spacing) { m_spacing = spacing; }

    /// The indent of widgets in a group (underneath a Label) of this GroupLayout.
    int group_indent() const { return m_group_indent; }

    /// Sets the indent of widgets in a group (underneath a Label) of this GroupLayout.
    void set_group_indent(int group_indent) { m_group_indent = group_indent; }

    /// The spacing between groups of this GroupLayout.
    int group_spacing() const { return m_group_spacing; }

    /// Sets the spacing between groups of this GroupLayout.
    void set_group_spacing(int group_spacing) { m_group_spacing = group_spacing; }

    /* Implementation of the layout interface */

    /// See \ref Layout::preferred_size.
    virtual Vector2i preferred_size(NVGcontext *ctx, const Widget *widget) const override;

    /// See \ref Layout::perform_layout.
    virtual void perform_layout(NVGcontext *ctx, Widget *widget) const override;

protected:
    /// The margin of this GroupLayout.
    int m_margin;

    /// The spacing between widgets of this GroupLayout.
    int m_spacing;

    /// The spacing between groups of this GroupLayout.
    int m_group_spacing;

    /// The indent amount of a group under its defining Label of this GroupLayout.
    int m_group_indent;
};

/**
 * \class GridLayout layout.h nanogui/layout.h
 *
 * \brief Grid layout.
 *
 * Widgets are arranged in a grid that has a fixed grid resolution \c resolution
 * along one of the axes. The layout orientation indicates the fixed dimension;
 * widgets are also appended on this axis. The spacing between items can be
 * specified per axis. The horizontal/vertical alignment can be specified per
 * row and column.
 */
class NANOGUI_EXPORT GridLayout : public Layout {
public:
    /**
     * Create a 2-column grid layout by default.
     *
     * \param orientation
     *     The fixed dimension of this GridLayout.
     *
     * \param resolution
     *     The number of rows or columns in the grid (depending on the Orientation).
     *
     * \param alignment
     *     How widgets should be aligned within each grid cell.
     *
     * \param margin
     *     The amount of spacing to add around the border of the grid.
     *
     * \param spacing
     *     The amount of spacing between widgets added to the grid.
     */
    GridLayout(Orientation orientation = Orientation::Horizontal, int resolution = 2,
               Alignment alignment = Alignment::Middle,
               int margin = 0, int spacing = 0)
        : m_orientation(orientation), m_resolution(resolution), m_margin(margin) {
        m_default_alignment[0] = m_default_alignment[1] = alignment;
        m_spacing = Vector2i(spacing);
    }

    /// The Orientation of this GridLayout.
    Orientation orientation() const { return m_orientation; }

    /// Sets the Orientation of this GridLayout.
    void set_orientation(Orientation orientation) {
        m_orientation = orientation;
    }

    /// The number of rows or columns (depending on the Orientation) of this GridLayout.
    int resolution() const { return m_resolution; }
    /// Sets the number of rows or columns (depending on the Orientation) of this GridLayout.
    void set_resolution(int resolution) { m_resolution = resolution; }

    /// The spacing at the specified axis (row or column number, depending on the Orientation).
    int spacing(int axis) const { return m_spacing[axis]; }
    /// Sets the spacing for a specific axis.
    void set_spacing(Orientation axis, int spacing) { m_spacing[static_cast<int>(axis)] = spacing; }
    /// Sets the spacing for all axes.
    void set_spacing(int spacing) { m_spacing[0] = m_spacing[1] = spacing; }

    /// The margin around this GridLayout.
    int margin() const { return m_margin; }
    /// Sets the margin of this GridLayout.
    void set_margin(int margin) { m_margin = margin; }

    /**
     * The Alignment of the specified axis (row or column number, depending on
     * the Orientation) at the specified index of that row or column.
     */
    Alignment alignment(int axis, int item) const {
        if (item < (int) m_alignment[axis].size())
            return m_alignment[axis][item];
        else
            return m_default_alignment[axis];
    }

    /// Sets the Alignment of the columns.
    void set_col_alignment(Alignment value) { m_default_alignment[0] = value; }

    /// Sets the Alignment of the rows.
    void set_row_alignment(Alignment value) { m_default_alignment[1] = value; }

    /// Use this to set variable Alignment for columns.
    void set_col_alignment(const std::vector<Alignment> &value) { m_alignment[0] = value; }

    /// Use this to set variable Alignment for rows.
    void set_row_alignment(const std::vector<Alignment> &value) { m_alignment[1] = value; }

    /* Implementation of the layout interface */
    /// See \ref Layout::preferred_size.
    virtual Vector2i preferred_size(NVGcontext *ctx, const Widget *widget) const override;

    /// See \ref Layout::perform_layout.
    virtual void perform_layout(NVGcontext *ctx, Widget *widget) const override;

	/// Draw table on background
	virtual void draw_table(NVGcontext *ctx, const Widget *widget) override;

protected:
    // Compute the maximum row and column sizes
    void compute_layout(NVGcontext *ctx, const Widget *widget,
                        std::vector<int> *grid) const;

    /// The Orientation of the GridLayout.
    Orientation m_orientation;
    /// The default Alignment of the GridLayout.
    Alignment m_default_alignment[2];
    /// The actual Alignment being used for each column/row
    std::vector<Alignment> m_alignment[2];
    /// The number of rows or columns before starting a new one, depending on the Orientation.
    int m_resolution;
    /// The spacing used for each dimension.
    Vector2i m_spacing;
    /// The margin around this GridLayout.
    int m_margin;
};

/**
 * \class AdvancedGridLayout layout.h nanogui/layout.h
 *
 * \brief Advanced Grid layout.
 *
 * The is a fancier grid layout with support for items that span multiple rows
 * or columns, and per-widget alignment flags. Each row and column additionally
 * stores a stretch factor that controls how additional space is redistributed.
 * The downside of this flexibility is that a layout anchor data structure must
 * be provided for each widget.
 *
 * An example:
 *
 * \rst
 * .. code-block:: cpp
 *
 *    using Anchor = AdvancedGridLayout::Anchor;
 *    Label *label = new Label(window, "A label");
 *    // Add a centered label at grid position (1, 5), which spans two horizontal cells
 *    layout->set_anchor(label, Anchor(1, 5, 2, 1, Alignment::Middle, Alignment::Middle));
 *
 * \endrst
 *
 * The grid is initialized with user-specified column and row size vectors
 * (which can be expanded later on if desired). If a size value of zero is
 * specified for a column or row, the size is set to the maximum preferred size
 * of any widgets contained in the same row or column. Any remaining space is
 * redistributed according to the row and column stretch factors.
 *
 * The high level usage somewhat resembles the classic HIG layout:
 *
 * - https://web.archive.org/web/20070813221705/http://www.autel.cz/dmi/tutorial.html
 * - https://github.com/jaapgeurts/higlayout
 */
class NANOGUI_EXPORT AdvancedGridLayout : public Layout {
public:
    /**
     * \struct Anchor layout.h nanogui/layout.h
     *
     * \brief Helper struct to coordinate anchor points for the layout.
     */
    struct Anchor {
        uint8_t pos[2];    ///< The ``(x, y)`` position.
        uint8_t size[2];   ///< The ``(x, y)`` size.
        Alignment align[2];///< The ``(x, y)`` Alignment.

        /// Creates a ``0`` Anchor.
        Anchor() { }

        /// Create an Anchor at position ``(x, y)`` with specified Alignment.
        Anchor(int x, int y, Alignment horiz = Alignment::Fill,
              Alignment vert = Alignment::Fill) {
            pos[0] = (uint8_t) x; pos[1] = (uint8_t) y;
            size[0] = size[1] = 1;
            align[0] = horiz; align[1] = vert;
        }

        /// Create an Anchor at position ``(x, y)`` of size ``(w, h)`` with specified alignments.
        Anchor(int x, int y, int w, int h,
              Alignment horiz = Alignment::Fill,
              Alignment vert = Alignment::Fill) {
            pos[0] = (uint8_t) x; pos[1] = (uint8_t) y;
            size[0] = (uint8_t) w; size[1] = (uint8_t) h;
            align[0] = horiz; align[1] = vert;
        }

        /// Allows for printing out Anchor position, size, and alignment.
        operator std::string() const {
            char buf[60];
            std::snprintf(buf, 60, "Format[pos=(%i, %i), size=(%i, %i), align=(%i, %i)]",
                pos[0], pos[1], size[0], size[1], (int) align[0], (int) align[1]);
            return buf;
        }
    };

    /// Creates an AdvancedGridLayout with specified columns, rows, and margin.
    AdvancedGridLayout(const std::vector<int> &cols = {}, const std::vector<int> &rows = {}, int margin = 0);

    /// The margin of this AdvancedGridLayout.
    int margin() const { return m_margin; }
    /// Sets the margin of this AdvancedGridLayout.
    void set_margin(int margin) { m_margin = margin; }

    /// Return the number of cols
    int col_count() const { return (int) m_cols.size(); }

    /// Return the number of rows
    int row_count() const { return (int) m_rows.size(); }

    /// Append a row of the given size (and stretch factor)
    void append_row(int size, float stretch = 0.f) { m_rows.push_back(size); m_row_stretch.push_back(stretch); };

    /// Append a column of the given size (and stretch factor)
    void append_col(int size, float stretch = 0.f) { m_cols.push_back(size); m_col_stretch.push_back(stretch); };

    /// Set the stretch factor of a given row
    void set_row_stretch(int index, float stretch) { m_row_stretch.at(index) = stretch; }

    /// Set the stretch factor of a given column
    void set_col_stretch(int index, float stretch) { m_col_stretch.at(index) = stretch; }

    /// Specify the anchor data structure for a given widget
    void set_anchor(const Widget *widget, const Anchor &anchor) { m_anchor[widget] = anchor; }

    /// Retrieve the anchor data structure for a given widget
    Anchor anchor(const Widget *widget) const {
        auto it = m_anchor.find(widget);
        if (it == m_anchor.end())
            throw std::runtime_error("Widget was not registered with the grid layout!");
        return it->second;
    }

    /* Implementation of the layout interface */

    /// See \ref Layout::preferred_size.
    virtual Vector2i preferred_size(NVGcontext *ctx, const Widget *widget) const override;

    /// See \ref Layout::perform_layout.
    virtual void perform_layout(NVGcontext *ctx, Widget *widget) const override;

	/// Draw table on background
	virtual void draw_table(NVGcontext *ctx, const Widget *widget) override;

protected:
    // Compute the maximum row and column sizes
    void compute_layout(NVGcontext *ctx, const Widget *widget,
                        std::vector<int> *grid) const;

protected:
    /// The columns of this AdvancedGridLayout.
    std::vector<int> m_cols;

    /// The rows of this AdvancedGridLayout.
    std::vector<int> m_rows;

    /// The stretch for each column of this AdvancedGridLayout.
    std::vector<float> m_col_stretch;

    /// The stretch for each row of this AdvancedGridLayout.
    std::vector<float> m_row_stretch;

    /// The mapping of widgets to their specified anchor points.
    std::unordered_map<const Widget *, Anchor> m_anchor;

    /// The margin around this AdvancedGridLayout.
    int m_margin;
};

/// Flex direction for FlexLayout (equivalent to CSS flex-direction)
enum class FlexDirection {
    Row = 0,        ///< Items arranged horizontally, left to right
    RowReverse,     ///< Items arranged horizontally, right to left
    Column,         ///< Items arranged vertically, top to bottom
    ColumnReverse   ///< Items arranged vertically, bottom to top
};

/// Justify content for main axis alignment (equivalent to CSS justify-content)
enum class JustifyContent {
    FlexStart = 0,  ///< Items packed at start of main axis
    FlexEnd,        ///< Items packed at end of main axis
    Center,         ///< Items centered along main axis
    SpaceBetween,   ///< Items evenly distributed, first/last at edges
    SpaceAround,    ///< Items evenly distributed with equal space around
    SpaceEvenly     ///< Items evenly distributed with equal space between
};

/// Align items for cross axis alignment (equivalent to CSS align-items)
enum class AlignItems {
    FlexStart = 0,  ///< Items aligned at start of cross axis
    FlexEnd,        ///< Items aligned at end of cross axis
    Center,         ///< Items centered along cross axis
    Stretch,        ///< Items stretched to fill cross axis
    Baseline        ///< Items aligned along their baseline
};

/// Flex wrap behavior (equivalent to CSS flex-wrap)
enum class FlexWrap {
    NoWrap = 0,     ///< Items stay on single line
    Wrap,           ///< Items wrap to new lines as needed
    WrapReverse     ///< Items wrap to new lines in reverse order
};

/**
 * \class FlexLayout layout.h nanogui/layout.h
 *
 * \brief CSS Flexbox-inspired layout manager
 *
 * This layout implements the CSS Flexbox model for one-dimensional layout.
 * Items can be arranged along a main axis with flexible sizing and various
 * alignment options along both main and cross axes.
 */
class NANOGUI_EXPORT FlexLayout : public Layout {
public:
    /**
     * \struct FlexItem layout.h nanogui/layout.h
     *
     * \brief Flex properties for individual child widgets
     */
    struct FlexItem {
        float flex_grow = 0.0f;     ///< How much the item should grow (CSS flex-grow)
        float flex_shrink = 1.0f;   ///< How much the item should shrink (CSS flex-shrink)
        int flex_basis = -1;        ///< Initial main size before free space distribution (CSS flex-basis)
        AlignItems align_self = AlignItems::FlexStart;  ///< Override container align_items for this item

        FlexItem() = default;
        FlexItem(float grow, float shrink = 1.0f, int basis = -1)
            : flex_grow(grow), flex_shrink(shrink), flex_basis(basis) {}
    };

    /**
     * Creates a FlexLayout with specified direction and alignment.
     *
     * \param direction The flex direction (main axis orientation)
     * \param justify_content Main axis alignment
     * \param align_items Cross axis alignment
     * \param margin Margin around the container
     * \param gap Gap between flex items
     */
    FlexLayout(FlexDirection direction = FlexDirection::Row,
               JustifyContent justify_content = JustifyContent::FlexStart,
               AlignItems align_items = AlignItems::Stretch,
               int margin = 0, int gap = 0);

    /// Get the flex direction
    FlexDirection direction() const { return m_direction; }
    /// Set the flex direction
    void set_direction(FlexDirection direction) { m_direction = direction; }

    /// Get the justify content setting
    JustifyContent justify_content() const { return m_justify_content; }
    /// Set the justify content setting
    void set_justify_content(JustifyContent justify) { m_justify_content = justify; }

    /// Get the align items setting
    AlignItems align_items() const { return m_align_items; }
    /// Set the align items setting
    void set_align_items(AlignItems align) { m_align_items = align; }

    /// Get the flex wrap setting
    FlexWrap flex_wrap() const { return m_flex_wrap; }
    /// Set the flex wrap setting
    void set_flex_wrap(FlexWrap wrap) { m_flex_wrap = wrap; }

    /// Get the margin
    int margin() const { return m_margin; }
    /// Set the margin
    void set_margin(int margin) { m_margin = margin; }

    /// Get the gap between items
    int gap() const { return m_gap; }
    /// Set the gap between items
    void set_gap(int gap) { m_gap = gap; }

    /// Set flex properties for a specific widget
    void set_flex_item(const Widget *widget, const FlexItem &item) { m_flex_items[widget] = item; }

    /// Get flex properties for a widget (returns default if not set)
    FlexItem get_flex_item(const Widget *widget) const {
        auto it = m_flex_items.find(widget);
        return it != m_flex_items.end() ? it->second : FlexItem();
    }

	// Respect widget's width/height flex modes
    SizeMode width_mode(const Widget* w) const { return w ? w->width_mode() : SizeMode::Preferred; }
    SizeMode height_mode(const Widget* w) const { return w ? w->height_mode() : SizeMode::Preferred; }

    /* Implementation of the layout interface */
    virtual Vector2i preferred_size(NVGcontext *ctx, const Widget *widget) const override;
    virtual void perform_layout(NVGcontext *ctx, Widget *widget) const override;

protected:
    /// Determine if direction is row-based
    bool is_row_direction() const { return m_direction == FlexDirection::Row || m_direction == FlexDirection::RowReverse; }

    /// Determine if direction is reversed
    bool is_reverse_direction() const { return m_direction == FlexDirection::RowReverse || m_direction == FlexDirection::ColumnReverse; }

    /// Get main axis index (0 for row, 1 for column)
    int main_axis() const { return is_row_direction() ? 0 : 1; }

    /// Get cross axis index
    int cross_axis() const { return is_row_direction() ? 1 : 0; }

protected:
    FlexDirection m_direction;
    JustifyContent m_justify_content;
    AlignItems m_align_items;
    FlexWrap m_flex_wrap;
    int m_margin;
    int m_gap;
    std::unordered_map<const Widget*, FlexItem> m_flex_items;
};


NAMESPACE_END(nanogui)
