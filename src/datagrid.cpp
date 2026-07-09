/*
    src/datagrid.cpp -- Enterprise DataGrid widget implementation

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/datagrid.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <nanogui/textbox.h>
#include <nanogui/checkbox.h>
#include <nanogui/combobox.h>
#include <nanogui/slider.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// DataGrid Implementation
// ---------------------------------------------------------------------------

DataGrid::DataGrid(Widget* parent)
    : WidgetCRTP<DataGrid>(parent)
{
    DebugName = (m_parent ? m_parent->DebugName : "") + ",DataGrid";
    set_font_size(16);

    // Momentum / flick scrolling is advanced inside draw(). Parent display
    // lists that stay clean never call child draw(), which kills inertia.
    // Keep ancestors uncached so coasting frames always re-enter draw().
    for (Widget* w = m_parent; w != nullptr; w = w->parent()) {
        if (w->cached())
            w->set_cached(false);
    }
}

void DataGrid::set_model(std::shared_ptr<DataGridModel> model) {
    m_model = std::move(model);
    m_selected_rows.clear();
    m_scroll = {0.0f, 0.0f};
    m_vel_x = m_vel_y = 0.0f;
    if (m_model) {
        m_column_order.resize(m_model->column_count());
        for (size_t i = 0; i < m_column_order.size(); ++i)
            m_column_order[i] = (int)i;
    }
    screen()->redraw();
}

void DataGrid::set_columns(const std::vector<DataGridColumn>& columns) {
    m_columns = columns;
    if (!m_column_order.empty() && m_column_order.size() != columns.size()) {
        m_column_order.resize(columns.size());
        for (size_t i = 0; i < m_column_order.size(); ++i)
            m_column_order[i] = (int)i;
    }
    screen()->redraw();
}

void DataGrid::set_column_width(int column, int width) {
    if (column >= 0 && column < (int)m_columns.size())
        m_columns[column].width = std::max(20, width);
}

void DataGrid::set_column_visible(int column, bool visible) {
    if (column >= 0 && column < (int)m_columns.size())
        m_columns[column].visible = visible;
}

void DataGrid::set_sorting_enabled(bool enabled) {
    // Future: could store flag if needed
}

void DataGrid::set_filtering_enabled(bool enabled) {
    // Future
}

void DataGrid::set_scroll(const Vector2f& s) {
    m_scroll = s;
    m_scroll.x() = std::clamp(m_scroll.x(), 0.0f, 1.0f);
    m_scroll.y() = std::clamp(m_scroll.y(), 0.0f, 1.0f);
    screen()->redraw();
}

void DataGrid::begin_edit(int row, int column) {
    if (!m_editing_enabled || !m_model || row < 0 || column < 0)
        return;

    end_edit(true);

    if (column >= (int)m_columns.size() || !m_columns[column].editor_factory)
        return;

    auto value = m_model->get_value(row, column);
    Widget* editor = m_columns[column].editor_factory(this, row, column, value);
    if (!editor)
        return;

    m_editor = editor;
    m_edit_row = row;
    m_edit_col = column;

    // Add as child if not already
    if (editor->parent() != this)
        add_child(editor);

    position_editor();
    editor->request_focus();
    screen()->redraw();
}

// ---------------------------------------------------------------------------
// Editor value extraction
// ---------------------------------------------------------------------------

namespace {

// Trim ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Parse a textual editor value into a typed `any` based on the column type.
// Falls back to returning the string verbatim if parsing fails.
nanogui::any parse_text_value(const std::string& raw, DataType type) {
    const std::string s = trim(raw);
    if (s.empty()) return std::string();

    switch (type) {
        case DataType::Integer: {
            try {
                size_t pos = 0;
                long long v = std::stoll(s, &pos);
                if (pos == s.size()) return (int64_t)v;
            } catch (...) {}
            return s;
        }
        case DataType::Double: {
            try {
                size_t pos = 0;
                double v = std::stod(s, &pos);
                if (pos == s.size()) return v;
            } catch (...) {}
            return s;
        }
        case DataType::Boolean: {
            std::string l(s.size(), '\0');
            for (size_t i = 0; i < s.size(); ++i)
                l[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
            if (l == "true"  || l == "1" || l == "yes" || l == "y") return true;
            if (l == "false" || l == "0" || l == "no"  || l == "n") return false;
            return s;
        }
        case DataType::String:
        case DataType::DateTime:
        case DataType::Blob:
        default:
            return s;
    }
}

// Default extractor: pull a value out of an unknown editor widget.
// Returns an empty `any` if no recognised widget type is found.
nanogui::any default_extract_editor_value(Widget* editor, DataType column_type) {
    if (!editor) return {};

    if (auto* cb = dynamic_cast<CheckBox*>(editor))
        return cb->checked();

    if (auto* combo = dynamic_cast<ComboBox*>(editor))
        return (int64_t)combo->selected_index();

    if (auto* sl = dynamic_cast<Slider*>(editor)) {
        if (column_type == DataType::Integer)
            return (int64_t)std::llround(sl->value());
        return (double)sl->value();
    }

    // TextBox covers TextBox itself and its IntBox<>/FloatBox<> specialisations.
    if (auto* tb = dynamic_cast<TextBox*>(editor))
        return parse_text_value(tb->value(), column_type);

    return {};
}

} // namespace

void DataGrid::end_edit(bool commit) {
    if (!m_editor)
        return;

    // Snapshot state and clear members FIRST so that any callback fired
    // by the focus shift (e.g. TextBox::focus_event(false) -> its callback
    // -> grid->end_edit()) finds m_editor == nullptr and bails out, instead
    // of recursing and leaving us with a dangling pointer to operate on.
    Widget* editor = m_editor;
    int er = m_edit_row;
    int ec = m_edit_col;
    m_editor = nullptr;
    m_edit_row = -1;
    m_edit_col = -1;

    // -- Commit phase --------------------------------------------------------
    // Read the value back from the editor, write it into the model, and notify
    // the user callback. This is done BEFORE focus shuffling / child removal so
    // the editor widget is still alive and inspectable.
    if (commit && editor && m_model && er >= 0 && ec >= 0 &&
        ec < (int)m_columns.size())
    {
        const DataGridColumn& col = m_columns[ec];

        nanogui::any new_value = col.editor_reader
            ? col.editor_reader(this, er, ec, editor)
            : default_extract_editor_value(editor, col.type);

        if (new_value.has_value()) {
            // Only attempt set_value() if the model says the cell is editable.
            bool wrote = false;
            if (m_model->is_cell_editable(er, ec))
                wrote = m_model->set_value(er, ec, new_value);

            // Fire the user callback regardless of whether the model accepted
            // the write -- consumers may want to react to a rejected edit.
            if (m_cell_edited_cb)
                m_cell_edited_cb(er, ec, new_value);

            (void)wrote;
        }
    }

    // Move focus off the editor BEFORE we destroy it so the screen
    // doesn't hold a dangling pointer to the freed widget in its
    // m_focus_path. Doing this AFTER clearing m_editor above makes
    // end_edit safely re-entrant (the focus shift's side effects may
    // call back into end_edit).
    Screen* sc = screen();
    if (sc)
        sc->update_focus(this);

    if (editor && editor->parent() == this)
        remove_child(editor);

    if (sc)
        sc->redraw();
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

static std::string format_number_with_groups(double v, const NumberFormat& nf) {
    bool negative = v < 0.0;
    double abs_v = std::abs(v);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", nf.decimal_places, abs_v);
    std::string s = buf;

    if (nf.use_grouping) {
        size_t dot = s.find('.');
        size_t int_end = (dot == std::string::npos) ? s.size() : dot;
        std::string out;
        out.reserve(s.size() + 4);
        for (size_t i = 0; i < int_end; ++i) {
            if (i > 0 && (int_end - i) % 3 == 0)
                out.push_back(',');
            out.push_back(s[i]);
        }
        if (dot != std::string::npos)
            out.append(s.substr(dot));
        s = std::move(out);
    }

    if (!nf.symbol.empty())
        s = nf.symbol + s;

    if (negative) {
        if (nf.parentheses_neg)
            s = "(" + s + ")";
        else
            s = "-" + s;
    }
    return s;
}

static std::string format_time_point(const std::chrono::system_clock::time_point& tp,
                                     const char* fmt) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm_buf);
    return buf;
}

std::string DataGrid::format_cell_text(const nanogui::any& value, DataType type,
                                       const CellStyle& style) const
{
    if (!value.has_value())
        return std::string();

    const std::type_info& ti = value.type();

    // Currency / Double / numeric formatting
    if (type == DataType::Currency || type == DataType::Double) {
        double v = 0.0;
        if (ti == typeid(double))         v = nanogui::any_cast<double>(value);
        else if (ti == typeid(float))     v = (double)nanogui::any_cast<float>(value);
        else if (ti == typeid(int64_t))   v = (double)nanogui::any_cast<int64_t>(value);
        else if (ti == typeid(int))       v = (double)nanogui::any_cast<int>(value);
        else return "[?]";
        return format_number_with_groups(v, style.number_format);
    }

    if (type == DataType::Integer) {
        int64_t v = 0;
        if (ti == typeid(int64_t))      v = nanogui::any_cast<int64_t>(value);
        else if (ti == typeid(int))     v = (int64_t)nanogui::any_cast<int>(value);
        else if (ti == typeid(double))  v = (int64_t)nanogui::any_cast<double>(value);
        else return "[?]";
        if (style.number_format.use_grouping) {
            NumberFormat nf = style.number_format;
            nf.decimal_places = 0;
            return format_number_with_groups((double)v, nf);
        }
        return std::to_string(v);
    }

    if (type == DataType::Boolean) {
        if (ti == typeid(bool)) return nanogui::any_cast<bool>(value) ? "true" : "false";
        return "[?]";
    }

    if (type == DataType::DateTime) {
        if (ti == typeid(std::chrono::system_clock::time_point))
            return format_time_point(nanogui::any_cast<std::chrono::system_clock::time_point>(value),
                                     "%Y-%m-%d %H:%M");
        if (ti == typeid(std::string)) return nanogui::any_cast<std::string>(value);
        return "[date]";
    }
    if (type == DataType::Date) {
        if (ti == typeid(std::chrono::system_clock::time_point))
            return format_time_point(nanogui::any_cast<std::chrono::system_clock::time_point>(value),
                                     "%Y-%m-%d");
        if (ti == typeid(std::string)) return nanogui::any_cast<std::string>(value);
        return "[date]";
    }
    if (type == DataType::Time) {
        if (ti == typeid(std::chrono::system_clock::time_point))
            return format_time_point(nanogui::any_cast<std::chrono::system_clock::time_point>(value),
                                     "%H:%M:%S");
        if (ti == typeid(std::string)) return nanogui::any_cast<std::string>(value);
        return "[time]";
    }

    // Strings & fallbacks
    if (ti == typeid(std::string))            return nanogui::any_cast<std::string>(value);
    if (ti == typeid(const char*))            return nanogui::any_cast<const char*>(value);
    if (ti == typeid(int64_t))                return std::to_string(nanogui::any_cast<int64_t>(value));
    if (ti == typeid(int))                    return std::to_string(nanogui::any_cast<int>(value));
    if (ti == typeid(double))                 { char b[32]; std::snprintf(b, sizeof(b), "%g", nanogui::any_cast<double>(value)); return b; }
    if (ti == typeid(bool))                   return nanogui::any_cast<bool>(value) ? "true" : "false";

    return "[value]";
}

int DataGrid::autosize_column(NVGcontext* ctx, int col) {
    if (!m_model || col < 0 || col >= (int)m_columns.size())
        return 0;

    const DataGridColumn& dc = m_columns[col];
    nvgSave(ctx);

    // Measure header
    nvgFontFace(ctx, "sans-bold");
    nvgFontSize(ctx, 14.0f);
    float bounds[4];
    nvgTextBounds(ctx, 0, 0, dc.title.c_str(), nullptr, bounds);
    float max_w = (bounds[2] - bounds[0]);
    // Reserve room for sort indicator + horizontal padding
    max_w += dc.sortable ? 32.0f : 16.0f;

    // Measure visible rows (bounded for safety)
    nvgFontFace(ctx, "sans");
    int last_row = std::min(m_first_visible_row + std::max(m_visible_rows, 1),
                            (int)m_model->row_count());
    for (int row = m_first_visible_row; row < last_row; ++row) {
        nanogui::any v = m_model->get_value(row, col);
        CellStyle style = m_model->get_cell_style(row, col);
        nvgFontSize(ctx, style.fontSize);
        nvgFontFace(ctx, style.bold ? "sans-bold" : "sans");
        std::string text = format_cell_text(v, dc.type, style);
        nvgTextBounds(ctx, 0, 0, text.c_str(), nullptr, bounds);
        float w = (bounds[2] - bounds[0]) + 16.0f; // padding
        if (w > max_w) max_w = w;
    }
    nvgRestore(ctx);

    int new_w = (int)std::ceil(max_w);
    new_w = std::min(dc.max_width, new_w);
    new_w = std::max(dc.min_width, new_w);
    return new_w;
}

void DataGrid::set_cell_clicked_callback(std::function<void(int, int)> cb) {
    m_cell_clicked_cb = std::move(cb);
}

void DataGrid::set_cell_edited_callback(std::function<void(int, int, const nanogui::any&)> cb) {
    m_cell_edited_cb = std::move(cb);
}

void DataGrid::set_sort_changed_callback(std::function<void(int, bool)> cb) {
    m_sort_changed_cb = std::move(cb);
}

void DataGrid::set_focused_cell(int row, int col) {
    if (!m_model) {
        m_focused_row = m_focused_col = -1;
        return;
    }
    if (row >= 0 && row < (int)m_model->row_count() &&
        col >= 0 && col < (int)m_columns.size()) {
        m_focused_row = row;
        m_focused_col = col;
    } else {
        m_focused_row = m_focused_col = -1;
    }
    screen()->redraw();
}

void DataGrid::position_editor() {
    if (!m_editor || m_edit_row < 0 || m_edit_col < 0)
        return;

    const int header_height = 28;
    const int row_height = 24;

    // Horizontal scroll offset in pixels
    float h_offset = 0.0f;
    float total_col_w = 0.0f;
    for (int idx : m_column_order)
        if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
            total_col_w += m_columns[idx].width;
    if (total_col_w > (float)width())
        h_offset = m_scroll.x() * (total_col_w - (float)width());

    // Walk columns in display order until we hit the target
    float xf = -h_offset;
    bool found = false;
    for (int idx : m_column_order) {
        if (idx < 0 || idx >= (int)m_columns.size()) continue;
        if (!m_columns[idx].visible) continue;
        if (idx == m_edit_col) { found = true; break; }
        xf += m_columns[idx].width;
    }
    if (!found) return;

    int col_width = m_columns[m_edit_col].width;
    float y_offset = m_row_scroll_offset * (float)row_height;
    int y = (int)(header_height + (m_edit_row - m_first_visible_row) * row_height - y_offset);

    m_editor->set_position(Vector2i((int)xf, y));
    m_editor->set_size(Vector2i(col_width, row_height));
}

Vector2i DataGrid::preferred_size(NVGcontext* /*ctx*/) const {
    if (m_parent && m_parent->height() > 0)
        return Vector2i(600, std::max(200, m_parent->height() - 80));
    return Vector2i(600, 400);
}

void DataGrid::perform_layout(NVGcontext* ctx) {
    Widget::perform_layout(ctx);
    update_visible_range();
    if (m_editor)
        position_editor();
}

void DataGrid::update_visible_range() {
    if (!m_model || m_model->row_count() == 0) {
        m_first_visible_row = 0;
        m_visible_rows = 0;
        m_row_scroll_offset = 0.0f;
        return;
    }

    const int header_height = 28;
    const int row_height = 24;
    int available_height = height() - header_height;

    m_fully_visible_rows = std::max(1, available_height / row_height);
    m_visible_rows = m_fully_visible_rows + 2; // +2 for partial/prefetch rows

    float total_scrollable = std::max(1, (int)m_model->row_count() - m_fully_visible_rows + 1);
    float scroll_pos = m_scroll.y() * total_scrollable;

    m_first_visible_row = (int)std::floor(scroll_pos);
    m_row_scroll_offset = scroll_pos - m_first_visible_row; // 0.0 - 1.0
    m_first_visible_row = std::clamp(m_first_visible_row, 0, std::max(0, (int)m_model->row_count() - 1));

    // Clamp so the last row is always fully visible and flush at the bottom
    int last_possible_first = std::max(0, (int)m_model->row_count() - m_fully_visible_rows);
    if (m_first_visible_row > last_possible_first) {
        m_first_visible_row = last_possible_first;
        m_row_scroll_offset = 0.0f;
    }
}

int DataGrid::visible_row_count() const {
    return m_visible_rows;
}

int DataGrid::first_visible_row() const {
    return m_first_visible_row;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void DataGrid::draw(NVGcontext* ctx) {
    // Momentum scrolling integration (inspired by ScrollPanel)
    {
        double now = glfwGetTime();
        float dt = (m_last_t > 0.0) ? std::min((float)(now - m_last_t), 0.05f) : 0.0f;
        m_last_t = now;

        bool moving = false;

        if (std::abs(m_vel_y) > 0.5f && m_model && m_model->row_count() > 0) {
            float range = (float)std::max(1, (int)m_model->row_count() - m_visible_rows);
            float cur = m_scroll.y() * range + m_vel_y * dt;
            cur = std::clamp(cur, 0.f, range);
            m_scroll.y() = (range > 0.0f) ? cur / range : 0.0f;

            if (cur <= 0.f || cur >= range) m_vel_y = 0.f;
            else {
                m_vel_y *= std::exp(-8.0f * dt);
                if (std::abs(m_vel_y) < 0.5f) m_vel_y = 0.f;
            }
            moving = true;
        }

        // Horizontal momentum (m_vel_x)
        if (std::abs(m_vel_x) > 0.5f && m_model) {
            float total_col_w = 0.f;
            for (int idx : m_column_order)
                if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
                    total_col_w += m_columns[idx].width;

            if (total_col_w > (float)width()) {
                float max_scroll = total_col_w - (float)width();
                float cur = m_scroll.x() * max_scroll + m_vel_x * dt;
                cur = std::clamp(cur, 0.f, max_scroll);
                m_scroll.x() = (max_scroll > 0.f) ? cur / max_scroll : 0.f;

                if (cur <= 0.f || cur >= max_scroll) m_vel_x = 0.f;
                else {
                    m_vel_x *= std::exp(-8.0f * dt);
                    if (std::abs(m_vel_x) < 0.5f) m_vel_x = 0.f;
                }
                moving = true;
            }
        }

        if (moving) {
            update_visible_range();
            propagate_cache_dirty();
            screen()->redraw();
        }
    }

    nvgSave(ctx);
    nvgTranslate(ctx, m_pos.x(), m_pos.y());

    // Background (theme)
    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, (float)width(), (float)height());
    if (m_theme)
        nvgFillColor(ctx, m_theme->m_window_fill_focused);
    else
        nvgFillColor(ctx, nvgRGB(250, 250, 252));
    nvgFill(ctx);

    // Draw body first, then header on top so it overlays scrolling rows
    draw_body(ctx);
    draw_header(ctx);
    draw_scrollbars(ctx);

    nvgRestore(ctx);

    // Draw children (including the active editor) on top of the grid contents
    Widget::draw(ctx);
}

// ---------------------------------------------------------------------------
// Small drawing helpers (sort glyphs, theme-aware colors)
// ---------------------------------------------------------------------------

static inline NVGcolor mix_color(const NVGcolor& a, const NVGcolor& b, float t) {
    NVGcolor c;
    c.r = a.r + (b.r - a.r) * t;
    c.g = a.g + (b.g - a.g) * t;
    c.b = a.b + (b.b - a.b) * t;
    c.a = a.a + (b.a - a.a) * t;
    return c;
}

// Draw a small filled triangle sort glyph with a 1-px drop shadow.
//  dir = +1 (up arrow, ascending), -1 (down arrow, descending), 0 (unsorted: stacked pair)
static void draw_sort_glyph(NVGcontext* ctx, float cx, float cy, int dir,
                            const NVGcolor& fill, const NVGcolor& shadow,
                            bool faint)
{
    const float w = 4.5f; // half-width
    const float h = 5.0f; // total height

    auto draw_tri = [&](float ox, float oy, int d, const NVGcolor& col) {
        nvgBeginPath(ctx);
        if (d > 0) { // pointing up
            nvgMoveTo(ctx, ox,         oy - h * 0.5f);
            nvgLineTo(ctx, ox + w,     oy + h * 0.5f);
            nvgLineTo(ctx, ox - w,     oy + h * 0.5f);
        } else {     // pointing down
            nvgMoveTo(ctx, ox - w,     oy - h * 0.5f);
            nvgLineTo(ctx, ox + w,     oy - h * 0.5f);
            nvgLineTo(ctx, ox,         oy + h * 0.5f);
        }
        nvgClosePath(ctx);
        nvgFillColor(ctx, col);
        nvgFill(ctx);
    };

    if (dir == 0) {
        // Stacked tiny up + down ("sortable but unsorted")
        NVGcolor f = fill;
        f.a *= faint ? 0.45f : 0.7f;
        NVGcolor s = shadow;
        s.a *= faint ? 0.30f : 0.5f;
        // shadow first (offset by 1px down-right)
        draw_tri(cx + 0.6f, cy - 4.0f + 0.6f, +1, s);
        draw_tri(cx + 0.6f, cy + 4.0f + 0.6f, -1, s);
        draw_tri(cx,        cy - 4.0f,        +1, f);
        draw_tri(cx,        cy + 4.0f,        -1, f);
        return;
    }

    // Active sort: bigger triangle with a softer shadow
    NVGcolor s = shadow;
    s.a *= 0.55f;
    draw_tri(cx + 0.8f, cy + 0.8f, dir, s);
    draw_tri(cx,        cy,        dir, fill);
}

void DataGrid::draw_header(NVGcontext* ctx) {
    if (!m_model) return;

    const int header_height = 28;

    // Theme colors
    NVGcolor hdr_top    = m_theme ? (NVGcolor)m_theme->m_button_gradient_top_unfocused
                                  : nvgRGB(240, 240, 245);
    NVGcolor hdr_bot    = m_theme ? (NVGcolor)m_theme->m_button_gradient_bot_unfocused
                                  : nvgRGB(230, 230, 235);
    NVGcolor border_dk  = m_theme ? (NVGcolor)m_theme->m_border_dark
                                  : nvgRGB(200, 200, 205);
    NVGcolor border_lt  = m_theme ? (NVGcolor)m_theme->m_border_light
                                  : nvgRGB(220, 220, 225);
    NVGcolor text_color = m_theme ? (NVGcolor)m_theme->m_text_color
                                  : nvgRGB(60, 60, 70);
    NVGcolor sel_tint   = m_theme ? (NVGcolor)m_theme->m_button_gradient_top_focused
                                  : nvgRGB(210, 225, 250);

    // Gradient header background
    NVGpaint bg_paint = nvgLinearGradient(ctx, 0, 0, 0, (float)header_height,
                                          hdr_top, hdr_bot);
    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, (float)width(), (float)header_height);
    nvgFillPaint(ctx, bg_paint);
    nvgFill(ctx);

    // Bottom separator line
    nvgStrokeColor(ctx, border_dk);
    nvgStrokeWidth(ctx, 1.0f);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, 0, (float)header_height);
    nvgLineTo(ctx, (float)width(), (float)header_height);
    nvgStroke(ctx);

    nvgFontFace(ctx, "sans-bold");
    nvgFontSize(ctx, 14.0f);

    // Horizontal scroll offset in pixels
    float h_offset = 0.0f;
    float total_col_w = 0.0f;
    for (int idx : m_column_order) {
        if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
            total_col_w += m_columns[idx].width;
    }
    if (total_col_w > (float)width()) {
        float max_scroll = total_col_w - (float)width();
        h_offset = m_scroll.x() * max_scroll;
    }

    float x = -h_offset;
    for (int idx : m_column_order) {
        if (idx < 0 || idx >= (int)m_columns.size()) continue;
        const auto& col = m_columns[idx];
        if (!col.visible) continue;

        // Skip columns completely off left edge
        if (x + col.width < 0) { x += col.width; continue; }
        // Stop if past right edge
        if (x > (float)width()) break;

        bool col_selected = std::find(m_selected_cols.begin(), m_selected_cols.end(), idx)
                            != m_selected_cols.end();

        // Column-selected header highlight (overlay with the focused gradient)
        if (col_selected) {
            nvgBeginPath(ctx);
            nvgRect(ctx, x, 0, (float)col.width, (float)header_height);
            NVGcolor tint = sel_tint;
            tint.a *= 0.55f;
            nvgFillColor(ctx, tint);
            nvgFill(ctx);
        }

        // Sort glyph reservation
        const float sort_zone_w = col.sortable ? 18.0f : 0.0f;

        // Header text (only draw if visible)
        if (x < (float)width()) {
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x, 0, (float)col.width - sort_zone_w, (float)header_height);
            nvgFontFace(ctx, "sans-bold");
            nvgFontSize(ctx, 14.0f);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            // Slight text shadow for the header
            NVGcolor shadow = m_theme ? (NVGcolor)m_theme->m_text_color_shadow
                                      : nvgRGBA(0, 0, 0, 90);
            nvgFillColor(ctx, shadow);
            nvgText(ctx, x + 6 + 1, header_height * 0.5f + 1, col.title.c_str(), nullptr);
            nvgFillColor(ctx, text_color);
            nvgText(ctx, x + 6, header_height * 0.5f, col.title.c_str(), nullptr);
            nvgRestore(ctx);
        }

        // Sort glyph (drawn outside scissor, in its own reserved area)
        if (col.sortable) {
            float gx = x + col.width - sort_zone_w * 0.5f;
            float gy = header_height * 0.5f;
            bool is_active = (m_sort_col == idx);
            NVGcolor glyph_fill;
            if (is_active) {
                // Active sort glyph uses the theme's accent / proceed color
                NVGcolor accent = m_theme ? (NVGcolor)m_theme->m_proceed_color
                                          : nvgRGB(40, 110, 220);
                glyph_fill = accent;
                glyph_fill.a = 1.0f;
            } else {
                // Inactive: subdued (mix text color toward bg)
                glyph_fill = text_color;
                glyph_fill.a *= 0.75f;
            }
            NVGcolor glyph_shadow = m_theme ? (NVGcolor)m_theme->m_text_color_shadow
                                            : nvgRGBA(0, 0, 0, 120);
            int dir = is_active ? (m_sort_ascending ? +1 : -1) : 0;
            draw_sort_glyph(ctx, gx, gy, dir, glyph_fill, glyph_shadow, !is_active);
        }

        // Right border (subtle)
        nvgStrokeColor(ctx, border_lt);
        nvgStrokeWidth(ctx, 1.0f);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, x + col.width, 0);
        nvgLineTo(ctx, x + col.width, (float)header_height);
        nvgStroke(ctx);

        x += col.width;
    }
}

void DataGrid::draw_body(NVGcontext* ctx) {
    if (!m_model) return;

    const int header_height = 28;
    const int row_height = 24;
    const float total_width = (float)width();

    nvgFontFace(ctx, "sans");
    nvgFontSize(ctx, (float)font_size());

    int total_rows = (int)m_model->row_count();
    int last_row = std::min(m_first_visible_row + m_visible_rows, total_rows);

    float y_offset = m_row_scroll_offset * (float)row_height;
    // Resolve theme-derived palette once for the whole body pass
    NVGcolor base_bg      = m_theme ? (NVGcolor)m_theme->m_window_fill_focused
                                    : nvgRGB(250, 250, 252);
    NVGcolor alt_row_bg;
    {
        // Slightly brighter (or darker) shade for alternate rows
        alt_row_bg = base_bg;
        float delta = (base_bg.r + base_bg.g + base_bg.b) / 3.0f < 0.5f ? 0.06f : -0.04f;
        alt_row_bg.r = std::clamp(alt_row_bg.r + delta, 0.0f, 1.0f);
        alt_row_bg.g = std::clamp(alt_row_bg.g + delta, 0.0f, 1.0f);
        alt_row_bg.b = std::clamp(alt_row_bg.b + delta, 0.0f, 1.0f);
        alt_row_bg.a = base_bg.a > 0.0f ? base_bg.a : 1.0f;
    }
    NVGcolor sel_bg       = m_theme ? (NVGcolor)m_theme->m_button_gradient_top_focused
                                    : nvgRGB(200, 220, 255);
    sel_bg.a              = 0.85f;
    NVGcolor grid_color   = m_theme ? (NVGcolor)m_theme->m_border_light
                                    : nvgRGB(230, 230, 235);
    NVGcolor default_text = m_theme ? (NVGcolor)m_theme->m_text_color
                                    : nvgRGB(40, 40, 50);
    NVGcolor accent       = m_theme ? (NVGcolor)m_theme->m_proceed_color
                                    : nvgRGB(40, 110, 220);

    for (int row = m_first_visible_row; row < last_row; ++row) {
        float y = (float)(header_height + (row - m_first_visible_row) * row_height - y_offset);

        // Alternating row background
        bool selected = std::find(m_selected_rows.begin(), m_selected_rows.end(), row) != m_selected_rows.end();
        if (selected) {
            nvgBeginPath(ctx);
            nvgRect(ctx, 0, y, total_width, (float)row_height);
            nvgFillColor(ctx, sel_bg);
            nvgFill(ctx);
        } else if ((row % 2) == 0) {
            nvgBeginPath(ctx);
            nvgRect(ctx, 0, y, total_width, (float)row_height);
            nvgFillColor(ctx, alt_row_bg);
            nvgFill(ctx);
        }

        // Horizontal grid line (bottom of row)
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, 0, y + (float)row_height);
        nvgLineTo(ctx, total_width, y + (float)row_height);
        nvgStrokeColor(ctx, grid_color);
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);

        // Draw cells
            // Recompute h_offset for body (same calc)
            float h_offset_body = 0.0f;
            float total_col_w_body = 0.0f;
            for (int idx : m_column_order) {
                if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
                    total_col_w_body += m_columns[idx].width;
            }
            if (total_col_w_body > (float)width()) {
                float max_scroll = total_col_w_body - (float)width();
                h_offset_body = m_scroll.x() * max_scroll;
            }
            float x = -h_offset_body;
            for (int col_idx : m_column_order) {
                if (col_idx < 0 || col_idx >= (int)m_columns.size()) continue;
                const auto& col = m_columns[col_idx];
                if (!col.visible) continue;

                // Skip off-screen columns
                if (x + col.width < 0) { x += col.width; continue; }
                if (x > (float)width()) break;

                nanogui::any value = m_model->get_value(row, col_idx);
                CellStyle style = m_model->get_cell_style(row, col_idx);

                bool col_selected = std::find(m_selected_cols.begin(), m_selected_cols.end(), col_idx)
                                    != m_selected_cols.end();
                bool is_focus = (row == m_focused_row && col_idx == m_focused_col);
                bool is_editing_this = (m_editor && row == m_edit_row && col_idx == m_edit_col);

                // If this cell hosts the live editor, paint an opaque
                // background using the grid's base color so the underlying
                // formatted cell text never bleeds through the editor.
                if (is_editing_this) {
                    NVGcolor opaque_bg = base_bg;
                    opaque_bg.a = 1.0f;
                    nvgBeginPath(ctx);
                    nvgRect(ctx, x, y, (float)col.width, (float)row_height);
                    nvgFillColor(ctx, opaque_bg);
                    nvgFill(ctx);
                    // Still draw the right gridline / move x forward, but
                    // skip any text / focus border for this cell.
                    nvgBeginPath(ctx);
                    nvgMoveTo(ctx, x + col.width, y);
                    nvgLineTo(ctx, x + col.width, y + (float)row_height);
                    nvgStrokeColor(ctx, grid_color);
                    nvgStrokeWidth(ctx, 1.0f);
                    nvgStroke(ctx);
                    x += col.width;
                    continue;
                }

                // Per-cell background (style.bgColor wins, then column-selection tint)
                if (style.bgColor.a > 0.0f) {
                    nvgBeginPath(ctx);
                    nvgRect(ctx, x, y, (float)col.width, (float)row_height);
                    nvgFillColor(ctx, style.bgColor);
                    nvgFill(ctx);
                } else if (col_selected && !selected) {
                    NVGcolor tint = accent;
                    tint.a = 0.18f;
                    nvgBeginPath(ctx);
                    nvgRect(ctx, x, y, (float)col.width, (float)row_height);
                    nvgFillColor(ctx, tint);
                    nvgFill(ctx);
                }

                // Use custom draw if provided
                if (col.custom_draw) {
                    col.custom_draw(ctx, Vector2i((int)x, (int)y),
                                    Vector2i(col.width, row_height),
                                    value, style, selected, is_focus);
                } else {
                    // Determine sign for negative-red formatting
                    bool is_negative = false;
                    if (value.has_value()) {
                        const std::type_info& ti = value.type();
                        if (ti == typeid(double))      is_negative = nanogui::any_cast<double>(value) < 0.0;
                        else if (ti == typeid(int64_t)) is_negative = nanogui::any_cast<int64_t>(value) < 0;
                        else if (ti == typeid(int))     is_negative = nanogui::any_cast<int>(value) < 0;
                        else if (ti == typeid(float))   is_negative = nanogui::any_cast<float>(value) < 0.f;
                    }

                    // fgColor.a == 0 is the "inherit theme text color" sentinel
                    NVGcolor color = (style.fgColor.a > 0.0f) ? style.fgColor
                                                              : default_text;
                    if (is_negative && style.number_format.negative_red &&
                        (col.type == DataType::Currency || col.type == DataType::Double ||
                         col.type == DataType::Integer)) {
                        color = nvgRGB(220, 60, 60);
                    }

                    // Choose font face / size
                    const char* face = style.bold ? "sans-bold" : "sans";
                    if (style.monospace) face = "mono";
                    nvgFontFace(ctx, face);
                    nvgFontSize(ctx, style.fontSize > 0.f ? style.fontSize : (float)font_size());

                    // Horizontal alignment
                    int align_flags = NVG_ALIGN_MIDDLE;
                    float tx = x + 6.0f;
                    if (style.h_align == Alignment::Maximum) {
                        align_flags |= NVG_ALIGN_RIGHT;
                        tx = x + col.width - 8.0f;
                    } else if (style.h_align == Alignment::Middle) {
                        align_flags |= NVG_ALIGN_CENTER;
                        tx = x + col.width * 0.5f;
                    } else {
                        align_flags |= NVG_ALIGN_LEFT;
                    }

                    std::string text = format_cell_text(value, col.type, style);

                    nvgSave(ctx);
                    nvgIntersectScissor(ctx, x, y, (float)col.width, (float)row_height);
                    nvgTextAlign(ctx, align_flags);
                    nvgFillColor(ctx, color);
                    nvgText(ctx, tx, y + row_height * 0.5f, text.c_str(), nullptr);

                    // Underline if requested
                    if (style.underline) {
                        float tw = nvgTextBounds(ctx, tx, y + row_height * 0.5f,
                                                 text.c_str(), nullptr, nullptr);
                        float ux0 = tx, ux1 = tx + tw;
                        if (style.h_align == Alignment::Maximum)      { ux0 = tx - tw; ux1 = tx; }
                        else if (style.h_align == Alignment::Middle)  { ux0 = tx - tw * 0.5f; ux1 = tx + tw * 0.5f; }
                        nvgBeginPath(ctx);
                        nvgMoveTo(ctx, ux0, y + row_height * 0.5f + style.fontSize * 0.45f);
                        nvgLineTo(ctx, ux1, y + row_height * 0.5f + style.fontSize * 0.45f);
                        nvgStrokeColor(ctx, color);
                        nvgStrokeWidth(ctx, 1.0f);
                        nvgStroke(ctx);
                    }
                    nvgRestore(ctx);
                }

                // Vertical grid line on the right of each cell
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, x + col.width, y);
                nvgLineTo(ctx, x + col.width, y + (float)row_height);
                nvgStrokeColor(ctx, grid_color);
                nvgStrokeWidth(ctx, 1.0f);
                nvgStroke(ctx);

                // Cell focus border (drawn last so it sits on top)
                if (is_focus) {
                    nvgBeginPath(ctx);
                    nvgRect(ctx, x + 1.0f, y + 1.0f,
                            (float)col.width - 2.0f, (float)row_height - 2.0f);
                    nvgStrokeColor(ctx, accent);
                    nvgStrokeWidth(ctx, 2.0f);
                    nvgStroke(ctx);
                }

                x += col.width;
            }
        }
    }

void DataGrid::draw_scrollbars(NVGcontext* ctx) {
    if (!m_model || m_model->row_count() == 0)
        return;

    constexpr float SB_W = 8.0f;
    constexpr float SB_MARGIN = 5.0f;
    constexpr float SB_MIN = 28.0f;
    const int header_height = 28;

    float body_height = (float)(height() - header_height);

    // Vertical scrollbar (pill style, matching ScrollPanel)
    // Only covers the body area, not the header
    int total_rows = (int)m_model->row_count();
    m_vscroll_thumb_h = 0.0f;
    if (total_rows > m_visible_rows) {
        float vis_ratio = (float)m_visible_rows / (float)total_rows;
        float th = std::max(SB_MIN, body_height * vis_ratio);
        float track = body_height - th;
        float ty = m_scroll.y() * track;  // relative to body start

        m_vscroll_track = track;
        // Store thumb rect in widget-local coords (body area only)
        m_vscroll_thumb_y = (float)header_height + ty + 3.f;
        m_vscroll_thumb_h = th - 6.f;

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, (float)width() - SB_W - SB_MARGIN,
                       (float)header_height + ty + 3.f,
                       SB_W, th - 6.f, SB_W * 0.5f);
        nvgFillColor(ctx, nvgRGBA(150, 155, 165, 180));
        nvgFill(ctx);
    }

    // Horizontal scrollbar (at bottom of body)
    m_hscroll_thumb_w = 0.0f;
    float total_col_w = 0.0f;
    for (int idx : m_column_order) {
        if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
            total_col_w += m_columns[idx].width;
    }
    if (total_col_w > (float)width()) {
        float vis_ratio = (float)width() / total_col_w;
        float tw = std::max(SB_MIN, (float)width() * vis_ratio);
        float track = (float)width() - tw;
        float tx = m_scroll.x() * track;

        m_hscroll_track = track;
        m_hscroll_thumb_x = tx + 3.f;
        m_hscroll_thumb_w = tw - 6.f;

        // Draw horizontal thumb, positioned at bottom of body area, leave space for vscroll corner
        float hbar_y = (float)height() - SB_W - SB_MARGIN;
        float hbar_right = (float)width() - SB_MARGIN;
        if (m_vscroll_thumb_h > 0.0f) {
            hbar_right -= (SB_W + SB_MARGIN); // avoid overlapping vertical bar
        }

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, tx + 3.f, hbar_y, tw - 6.f, SB_W, SB_W * 0.5f);
        nvgFillColor(ctx, nvgRGBA(150, 155, 165, 180));
        nvgFill(ctx);
    }
}

// ---------------------------------------------------------------------------
// Input Handling
// ---------------------------------------------------------------------------

bool DataGrid::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    if (Widget::mouse_button_event(p, button, down, modifiers))
        return true;

    Vector2i local = p - m_pos;

    if (down && button == GLFW_MOUSE_BUTTON_1) {
        const int header_height = 28;
        const int row_height = 24;

        // Compute pending double-click context (consumed below)
        double now_t = glfwGetTime();
        bool dblclick_window = (now_t - m_last_click_time) < 0.4;

        // Scrollbar hit-test first so thumb dragging takes priority over row selection
        // Horizontal scrollbar (bottom) has priority if overlapping vertical
        if (m_hscroll_thumb_w > 0.0f) {
            constexpr float SB_W = 8.0f;
            constexpr float SB_MARGIN = 5.0f;
            float hbar_y = (float)height() - SB_W - SB_MARGIN;
            if (local.y() >= hbar_y && local.y() < hbar_y + SB_W) {
                if (local.x() >= m_hscroll_thumb_x && local.x() < m_hscroll_thumb_x + m_hscroll_thumb_w) {
                    m_dragging_hscroll = true;
                    m_drag_start_scroll_x = m_scroll.x();
                    m_drag_start_mouse_x = (float)local.x();
                    return true;
                } else {
                    // Click on horizontal track - jump
                    float click_pos = (float)local.x() - m_hscroll_thumb_w * 0.5f;
                    float t = (m_hscroll_track > 0.0f) ? click_pos / m_hscroll_track : 0.0f;
                    m_scroll.x() = std::clamp(t, 0.0f, 1.0f);
                    screen()->redraw();
                    return true;
                }
            }
        }

        if (m_vscroll_thumb_h > 0.0f) {
            constexpr float SB_W = 8.0f;
            constexpr float SB_MARGIN = 5.0f;
            float sb_x = (float)width() - SB_W - SB_MARGIN;
            if (local.x() >= sb_x && local.x() < sb_x + SB_W) {
                // local.y() already includes header, and m_vscroll_thumb_y is stored in widget-local coords
                if (local.y() >= m_vscroll_thumb_y && local.y() < m_vscroll_thumb_y + m_vscroll_thumb_h) {
                    m_dragging_vscroll = true;
                    m_drag_start_scroll = m_scroll.y();
                    m_drag_start_mouse_y = (float)local.y();
                    return true;
                } else if (local.y() >= 28) {
                    // Click on track (below header) - jump scroll position
                    // click_pos is relative to body start
                    float body_click = (float)local.y() - 28.f - m_vscroll_thumb_h * 0.5f;
                    float t = (m_vscroll_track > 0.0f) ? body_click / m_vscroll_track : 0.0f;
                    m_scroll.y() = std::clamp(t, 0.0f, 1.0f);
                    update_visible_range();
                    screen()->redraw();
                    return true;
                }
            }
        }

        if (local.y() < header_height) {
            // Header click - detect resize hot-zone directly from click position
            float click_x = (float)local.x();
            float h_offset = 0.0f;
            float total_col_w = 0.0f;
            for (int idx : m_column_order) {
                if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
                    total_col_w += m_columns[idx].width;
            }
            if (total_col_w > (float)width()) {
                float max_scroll = total_col_w - (float)width();
                h_offset = m_scroll.x() * max_scroll;
            }

            float xx = -h_offset;
            int resize_target = -1;
            float min_dist = 9999.0f;
            int nearest_col = -1;
            float nearest_div = 0;

            for (size_t i = 0; i < m_column_order.size(); ++i) {
                int col = m_column_order[i];
                if (col < 0 || col >= (int)m_columns.size()) continue;
                if (!m_columns[col].visible) continue;

                if (xx + m_columns[col].width < 0) { xx += m_columns[col].width; continue; }
                if (xx > (float)width()) break;

                float divider_x = xx + m_columns[col].width;
                float dist = std::abs(click_x - divider_x);
                if (dist < min_dist) {
                    min_dist = dist;
                    nearest_col = col;
                    nearest_div = divider_x;
                }
                if (dist <= 8.0f) {  // increased tolerance
                    resize_target = col;
                    break;
                }
                xx += m_columns[col].width;
            }

            (void)nearest_col; (void)nearest_div; (void)min_dist; (void)click_x; (void)h_offset;


            if (resize_target >= 0) {
                // Double-click on resize hotzone -> autosize column
                bool is_dbl_resize = dblclick_window &&
                                     m_last_click_header &&
                                     m_last_click_resize &&
                                     m_last_click_col == resize_target;
                if (is_dbl_resize) {
                    NVGcontext* ctx = screen() ? screen()->nvg_context() : nullptr;
                    if (ctx) {
                        int new_w = autosize_column(ctx, resize_target);
                        if (new_w > 0) {
                            m_columns[resize_target].width = new_w;
                            screen()->redraw();
                        }
                    }
                    // Reset so the next click is a fresh single click
                    m_last_click_time = 0.0;
                    m_last_click_col = -1;
                    m_last_click_row = -1;
                    m_last_click_header = false;
                    m_last_click_resize = false;
                    return true;
                }

                m_resizing_col = resize_target;
                m_last_mouse_pos = local;
                m_last_click_time = now_t;
                m_last_click_col = resize_target;
                m_last_click_row = -1;
                m_last_click_header = true;
                m_last_click_resize = true;
                return true;
            }

            // Normal header click: select column (and toggle sort if sortable)
            // Recompute h_offset to find the clicked column in display coords.
            float h_offset2 = 0.0f;
            float total_col_w2 = 0.0f;
            for (int idx : m_column_order)
                if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
                    total_col_w2 += m_columns[idx].width;
            if (total_col_w2 > (float)width())
                h_offset2 = m_scroll.x() * (total_col_w2 - (float)width());

            float xh = -h_offset2;
            int clicked_col = -1;
            int clicked_order_idx = -1;
            float clicked_col_left = 0.0f;   // x position of the clicked column's left edge
            for (size_t i = 0; i < m_column_order.size(); ++i) {
                int col = m_column_order[i];
                if (col < 0 || col >= (int)m_columns.size()) continue;
                if (!m_columns[col].visible) continue;
                if ((float)local.x() >= xh && (float)local.x() < xh + m_columns[col].width) {
                    clicked_col = col;
                    clicked_order_idx = (int)i;
                    clicked_col_left = xh;
                    break;
                }
                xh += m_columns[col].width;
            }
            m_pressed_header_col = clicked_order_idx;

            if (clicked_col >= 0) {
                // Column selection (single column for now)
                m_selected_cols = { clicked_col };
                m_selected_rows.clear();
                m_focused_row = -1;
                m_focused_col = -1;

                // Sort cycling: only when the click falls inside the
                // small glyph zone at the right of the column header.
                // This matches the area where draw_header() actually
                // paints the up/down triangles (sort_zone_w = 18 px).
                if (m_columns[clicked_col].sortable) {
                    constexpr float kSortZoneW = 18.0f;
                    float zone_left  = clicked_col_left + m_columns[clicked_col].width - kSortZoneW;
                    float zone_right = clicked_col_left + m_columns[clicked_col].width;
                    bool in_sort_zone = ((float)local.x() >= zone_left &&
                                         (float)local.x() <  zone_right);
                    if (in_sort_zone) {
                        // 3-state cycle: none -> asc -> desc -> none
                        if (m_sort_col != clicked_col) {
                            m_sort_col = clicked_col;
                            m_sort_ascending = true;
                        } else if (m_sort_ascending) {
                            m_sort_ascending = false; // now descending
                        } else {
                            m_sort_col = -1;          // back to unsorted
                            m_sort_ascending = true;
                        }
                        if (m_model && m_model->supports_sorting())
                            m_model->sort(m_sort_col, m_sort_ascending);
                        if (m_sort_changed_cb)
                            m_sort_changed_cb(m_sort_col, m_sort_ascending);
                    }
                }
                screen()->redraw();
            }

            m_last_click_time = now_t;
            m_last_click_col = clicked_col;
            m_last_click_row = -1;
            m_last_click_header = true;
            m_last_click_resize = false;
            return true;
        } else {
            // Body click - select row, set cell focus.
            // Limit width so scrollbar area is excluded.
            constexpr float SB_MARGIN = 5.0f;
            constexpr float SB_W = 8.0f;
            float body_right = (float)width() - SB_W - SB_MARGIN;

            int clicked_row = -1;
            int clicked_col = -1;

            if (local.x() < body_right && m_model) {
                // Determine row
                float rel_y = (float)local.y() - header_height;
                float adjusted_y = rel_y + m_row_scroll_offset * (float)row_height;
                int row = m_first_visible_row + (int)std::floor(adjusted_y / (float)row_height);
                if (row >= 0 && row < (int)m_model->row_count())
                    clicked_row = row;

                // Determine column (account for h_offset)
                float h_offset_b = 0.0f;
                float total_col_w_b = 0.0f;
                for (int idx : m_column_order)
                    if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
                        total_col_w_b += m_columns[idx].width;
                if (total_col_w_b > (float)width())
                    h_offset_b = m_scroll.x() * (total_col_w_b - (float)width());

                float xb = -h_offset_b;
                for (int idx : m_column_order) {
                    if (idx < 0 || idx >= (int)m_columns.size()) continue;
                    if (!m_columns[idx].visible) continue;
                    if ((float)local.x() >= xb && (float)local.x() < xb + m_columns[idx].width) {
                        clicked_col = idx;
                        break;
                    }
                    xb += m_columns[idx].width;
                }
            }

            if (clicked_row >= 0) {
                // Detect double-click on the same cell -> start editing
                bool is_dbl_cell = dblclick_window &&
                                   !m_last_click_header &&
                                   m_last_click_row == clicked_row &&
                                   m_last_click_col == clicked_col;

                m_selected_rows = { clicked_row };
                m_selected_cols.clear();
                if (clicked_col >= 0) {
                    m_focused_row = clicked_row;
                    m_focused_col = clicked_col;
                }
                if (m_cell_clicked_cb)
                    m_cell_clicked_cb(clicked_row, clicked_col);
                screen()->redraw();
                request_focus(); // grab keyboard focus so arrow keys / Enter work

                if (is_dbl_cell && clicked_col >= 0 && m_editing_enabled) {
                    begin_edit(clicked_row, clicked_col);
                    // Reset double-click record so a third click doesn't re-trigger
                    m_last_click_time = 0.0;
                    m_last_click_col = -1;
                    m_last_click_row = -1;
                    return true;
                }
            }

            m_last_click_time = now_t;
            m_last_click_col = clicked_col;
            m_last_click_row = clicked_row;
            m_last_click_header = false;
            m_last_click_resize = false;
            return true;
        }
    }

    if (!down) {
        m_pressed_header_col = -1;
        m_dragging_vscroll = false;
        m_dragging_hscroll = false;
        m_resizing_col = -1;
    } else if (button == GLFW_MOUSE_BUTTON_1 && m_hscroll_thumb_w > 0.0f) {
        // Check if click is on the horizontal scrollbar thumb
        constexpr float SB_W = 8.0f;
        constexpr float SB_MARGIN = 5.0f;
        float hbar_y = (float)height() - SB_W - SB_MARGIN;
        if (local.y() >= hbar_y && local.y() < hbar_y + SB_W) {
            if (local.x() >= m_hscroll_thumb_x && local.x() < m_hscroll_thumb_x + m_hscroll_thumb_w) {
                m_dragging_hscroll = true;
                m_drag_start_scroll_x = m_scroll.x();
                m_drag_start_mouse_x = (float)local.x();
                return true;
            } else {
                float click_pos = (float)local.x() - m_hscroll_thumb_w * 0.5f;
                float t = (m_hscroll_track > 0.0f) ? click_pos / m_hscroll_track : 0.0f;
                m_scroll.x() = std::clamp(t, 0.0f, 1.0f);
                screen()->redraw();
                return true;
            }
        }
    } else if (button == GLFW_MOUSE_BUTTON_1 && m_vscroll_thumb_h > 0.0f) {
        // Check if click is on the vertical scrollbar thumb
        constexpr float SB_W = 8.0f;
        constexpr float SB_MARGIN = 5.0f;
        float sb_x = (float)width() - SB_W - SB_MARGIN;
        if (local.x() >= sb_x && local.x() < sb_x + SB_W) {
            if (local.y() >= m_vscroll_thumb_y && local.y() < m_vscroll_thumb_y + m_vscroll_thumb_h) {
                m_dragging_vscroll = true;
                m_drag_start_scroll = m_scroll.y();
                m_drag_start_mouse_y = (float)local.y();
                return true;
            } else if (local.y() >= 28) {
                // Click on track (below header) - jump to position
                float body_click = (float)local.y() - 28.f - m_vscroll_thumb_h * 0.5f;
                float t = (m_vscroll_track > 0.0f) ? body_click / m_vscroll_track : 0.0f;
                m_scroll.y() = std::clamp(t, 0.0f, 1.0f);
                update_visible_range();
                screen()->redraw();
                return true;
            }
        }
    }

    return false;
}

bool DataGrid::mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    if (Widget::mouse_drag_event(p, rel, button, modifiers))
        return true;

    if (m_resizing_col >= 0) {
        // Column resize in progress
        Vector2i local = p - m_pos;
        int delta = local.x() - m_last_mouse_pos.x();
        if (delta != 0) {
            int col = m_resizing_col;
            int new_width = m_columns[col].width + delta;

            // Bilateral clamping: consider left neighbor + column's own min/max
            int min_w = m_columns[col].min_width;
            int max_w = m_columns[col].max_width;
            // Find previous visible column
            int prev = -1;
            for (int j = 0; j < (int)m_column_order.size(); ++j) {
                if (m_column_order[j] == col) {
                    if (j > 0) prev = m_column_order[j-1];
                    break;
                }
            }
            if (prev >= 0 && m_columns[prev].visible) {
                int left_min = std::max(m_columns[prev].min_width,
                                        (int)(m_columns[prev].width * 0.25f));
                if (left_min > min_w) min_w = left_min;
            }

            bool clamped = false;
            if (new_width < min_w) { new_width = min_w; clamped = true; }
            if (new_width > max_w) { new_width = max_w; clamped = true; }

            m_columns[col].width = new_width;
            if (!clamped) m_last_mouse_pos = local;
            screen()->redraw();
        }
        return true;
    }

    if (button == GLFW_MOUSE_BUTTON_1 && m_pressed_header_col >= 0) {
        // Future: column reordering
        return true;
    }

    if (m_dragging_vscroll && m_vscroll_track > 0.0f) {
        float dy = (float)(p.y() - m_pos.y()) - m_drag_start_mouse_y;
        float new_scroll = m_drag_start_scroll + dy / m_vscroll_track;
        m_scroll.y() = std::clamp(new_scroll, 0.0f, 1.0f);
        update_visible_range();
        screen()->redraw();
        return true;
    }

    if (m_dragging_hscroll && m_hscroll_track > 0.0f) {
        float dx = (float)(p.x() - m_pos.x()) - m_drag_start_mouse_x;
        float new_scroll = m_drag_start_scroll_x + dx / m_hscroll_track;
        m_scroll.x() = std::clamp(new_scroll, 0.0f, 1.0f);
        screen()->redraw();
        return true;
    }

    return false;
}

bool DataGrid::mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    if (Widget::mouse_motion_event(p, rel, button, modifiers))
        return true;

    Vector2i local = p - m_pos;
    const int header_height = 28;

    m_hovered_resize_col = -1;

    // Only check for resize hot-zones when mouse is in the header
    if (local.y() >= 0 && local.y() < header_height) {
        float x = 0.0f;
        // Compute horizontal scroll offset
        float h_offset = 0.0f;
        float total_col_w = 0.0f;
        for (int idx : m_column_order) {
            if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
                total_col_w += m_columns[idx].width;
        }
        if (total_col_w > (float)width()) {
            float max_scroll = total_col_w - (float)width();
            h_offset = m_scroll.x() * max_scroll;
        }

        x = -h_offset;

        for (size_t i = 0; i < m_column_order.size(); ++i) {
            int col = m_column_order[i];
            if (col < 0 || col >= (int)m_columns.size()) continue;
            if (!m_columns[col].visible) continue;

            // Skip columns off left edge
            if (x + m_columns[col].width < 0) { x += m_columns[col].width; continue; }
            if (x > (float)width()) break;

            // Check if we are within ~4 pixels of the right edge of this column
            float divider_x = x + m_columns[col].width;
            if (std::abs(local.x() - divider_x) <= 6.0f) {
                m_hovered_resize_col = col;
                set_cursor(Cursor::HResize);
                return true;
            }

            x += m_columns[col].width;
        }
    }

    // Not over any divider → restore normal cursor
    if (cursor() == Cursor::HResize) {
        set_cursor(Cursor::Arrow);
    }
    return false;
}

bool DataGrid::scroll_event(const Vector2i& p, const Vector2f& rel) {
    if (!m_model) return Widget::scroll_event(p, rel);

    bool used = false;

    // Vertical (rows)
    if (rel.y() != 0.f && m_model->row_count() > (size_t)m_visible_rows) {
        m_vel_y = std::clamp(m_vel_y - rel.y() * 120.0f, -3500.0f, 3500.0f);
        used = true;
    }

    // Horizontal (columns) - accept rel.x() unconditionally (trackpad / mouse wheel)
    float total_col_w = 0.f;
    for (int idx : m_column_order)
        if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
            total_col_w += m_columns[idx].width;

    if (rel.x() != 0.f && total_col_w > (float)width()) {
        m_vel_x = std::clamp(m_vel_x - rel.x() * 120.0f, -3500.0f, 3500.0f);
        used = true;
    }

    if (used) {
        // Ensure any residual cached ancestor rebuilds; coasting needs draw().
        propagate_cache_dirty();
        screen()->redraw();
        return true;
    }
    return Widget::scroll_event(p, rel);
}

// Bring `row` into view by scrolling vertically (no-op if already visible).
static void ensure_row_visible(DataGrid* /*grid*/, int /*row*/) { /* placeholder unused */ }

bool DataGrid::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Widget::keyboard_event(key, scancode, action, modifiers))
        return true;

    // While an editor is open, the grid must stay out of the way of the
    // editor's keystrokes (Enter/Tab/letters/etc.). Screen::keyboard_event
    // walks the focus path outer->inner, so the grid would otherwise
    // intercept keys before the focused editor child sees them.
    // We do intercept Escape here so it always cancels the edit.
    if (m_editor) {
        if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) {
            end_edit(false);
            return true;
        }
        return false; // let the editor handle everything else
    }

    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return false;
    if (!m_model)
        return false;

    auto row_count_i = (int)m_model->row_count();

    // Helper: scroll so that `row` is at least partially visible.
    auto bring_row_into_view = [&](int row) {
        if (row < 0 || row_count_i <= 0) return;
        if (row < m_first_visible_row) {
            float total_scrollable = std::max(1, row_count_i - m_fully_visible_rows + 1);
            m_scroll.y() = std::clamp((float)row / total_scrollable, 0.0f, 1.0f);
            update_visible_range();
        } else if (row >= m_first_visible_row + m_fully_visible_rows) {
            float total_scrollable = std::max(1, row_count_i - m_fully_visible_rows + 1);
            int target_first = std::max(0, row - m_fully_visible_rows + 1);
            m_scroll.y() = std::clamp((float)target_first / total_scrollable, 0.0f, 1.0f);
            update_visible_range();
        }
    };

    // Helper: scroll so that column `col` is visible (rough).
    auto bring_col_into_view = [&](int col) {
        if (col < 0 || col >= (int)m_columns.size()) return;
        float total_col_w = 0.f;
        float left_of_col = 0.f, w_of_col = 0.f;
        for (int idx : m_column_order) {
            if (idx < 0 || idx >= (int)m_columns.size()) continue;
            if (!m_columns[idx].visible) continue;
            if (idx == col) { left_of_col = total_col_w; w_of_col = (float)m_columns[idx].width; }
            total_col_w += m_columns[idx].width;
        }
        if (total_col_w <= (float)width()) return;
        float max_scroll = total_col_w - (float)width();
        float cur = m_scroll.x() * max_scroll;
        if (left_of_col < cur)
            m_scroll.x() = left_of_col / max_scroll;
        else if (left_of_col + w_of_col > cur + (float)width())
            m_scroll.x() = (left_of_col + w_of_col - (float)width()) / max_scroll;
        m_scroll.x() = std::clamp(m_scroll.x(), 0.0f, 1.0f);
    };

    const bool has_focus_cell = (m_focused_row >= 0 && m_focused_col >= 0);

    // Find display index of currently focused column
    auto find_col_order_idx = [&](int col) -> int {
        for (int i = 0; i < (int)m_column_order.size(); ++i)
            if (m_column_order[i] == col) return i;
        return -1;
    };

    if (key == GLFW_KEY_DOWN) {
        if (has_focus_cell) {
            int nr = std::min(m_focused_row + 1, row_count_i - 1);
            m_focused_row = nr;
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        } else if (!m_selected_rows.empty()) {
            int nr = std::min(m_selected_rows[0] + 1, row_count_i - 1);
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        }
        screen()->redraw();
        return true;
    }
    if (key == GLFW_KEY_UP) {
        if (has_focus_cell) {
            int nr = std::max(m_focused_row - 1, 0);
            m_focused_row = nr;
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        } else if (!m_selected_rows.empty()) {
            int nr = std::max(m_selected_rows[0] - 1, 0);
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        }
        screen()->redraw();
        return true;
    }
    if (key == GLFW_KEY_PAGE_DOWN) {
        int page = std::max(1, m_fully_visible_rows);
        if (has_focus_cell) {
            int nr = std::min(m_focused_row + page, row_count_i - 1);
            m_focused_row = nr;
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        } else if (!m_selected_rows.empty()) {
            int nr = std::min(m_selected_rows[0] + page, row_count_i - 1);
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        }
        screen()->redraw();
        return true;
    }
    if (key == GLFW_KEY_PAGE_UP) {
        int page = std::max(1, m_fully_visible_rows);
        if (has_focus_cell) {
            int nr = std::max(m_focused_row - page, 0);
            m_focused_row = nr;
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        } else if (!m_selected_rows.empty()) {
            int nr = std::max(m_selected_rows[0] - page, 0);
            m_selected_rows = { nr };
            bring_row_into_view(nr);
        }
        screen()->redraw();
        return true;
    }
    if (key == GLFW_KEY_HOME) {
        if (has_focus_cell) {
            m_focused_row = 0;
            m_selected_rows = { 0 };
            bring_row_into_view(0);
            screen()->redraw();
            return true;
        }
    }
    if (key == GLFW_KEY_END) {
        if (has_focus_cell && row_count_i > 0) {
            m_focused_row = row_count_i - 1;
            m_selected_rows = { m_focused_row };
            bring_row_into_view(m_focused_row);
            screen()->redraw();
            return true;
        }
    }
    if (key == GLFW_KEY_ENTER) {
        if (m_editing_enabled) {
            if (has_focus_cell)
                begin_edit(m_focused_row, m_focused_col);
            else if (!m_selected_rows.empty())
                begin_edit(m_selected_rows[0], 0);
        }
        return true;
    }

    if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) {
        if (has_focus_cell) {
            int ord = find_col_order_idx(m_focused_col);
            int step = (key == GLFW_KEY_RIGHT) ? 1 : -1;
            int new_ord = ord;
            while (true) {
                new_ord += step;
                if (new_ord < 0 || new_ord >= (int)m_column_order.size()) {
                    new_ord = ord;
                    break;
                }
                int c = m_column_order[new_ord];
                if (c >= 0 && c < (int)m_columns.size() && m_columns[c].visible)
                    break;
            }
            if (new_ord != ord) {
                m_focused_col = m_column_order[new_ord];
                bring_col_into_view(m_focused_col);
                screen()->redraw();
            }
            return true;
        }

        // Fallback: horizontal scroll
        float total_col_w = 0.0f;
        for (int idx : m_column_order)
            if (idx >= 0 && idx < (int)m_columns.size() && m_columns[idx].visible)
                total_col_w += m_columns[idx].width;
        if (total_col_w > (float)width()) {
            float max_scroll = total_col_w - (float)width();
            float step = 120.0f / max_scroll;
            float delta = (key == GLFW_KEY_RIGHT) ? step : -step;
            m_scroll.x() = std::clamp(m_scroll.x() + delta, 0.0f, 1.0f);
            screen()->redraw();
        }
        return true;
    }

    return false;
}

NAMESPACE_END(nanogui)
