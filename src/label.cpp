/*
    nanogui/label.cpp -- Text label with an arbitrary font, color, and size

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/label.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <algorithm>
#include <sstream>
#include <numeric>
#include <GLFW/glfw3.h>

NAMESPACE_BEGIN(nanogui)

namespace {
    const char* ELLIPSIS = "..."; ///< Ellipsis string for truncation.
    const float TEXT_MARGIN = 2.0f; ///< Margin for non-fixed-size text width.
    static const Color DEFAULT_SELECTION_COLOR(0.0f, 0.5f, 1.0f, 0.5f); ///< Default selection highlight color (semi-transparent blue).
}

/**
 * \brief Helper function to measure text width using NanoVG.
 * \param ctx NanoVG context.
 * \param text Text to measure.
 * \return The width of the text in pixels.
 */
static float measure_text_width(NVGcontext* ctx, const std::string& text) {
    float bounds[4];
    nvgTextBounds(ctx, 0, 0, text.c_str(), nullptr, bounds);
    return bounds[2] - bounds[0];
}

Label::Label(Widget *parent, const std::string &caption, const std::string &font, int font_size)
    : WidgetCRTP<Label>(parent), m_caption(caption), m_font(font.empty() ? "sans" : font),
      m_line_break_mode(LineBreakMode::LineBreakByWordWrapping), m_cache_valid(false),
      m_selectable(false), m_selection_color(DEFAULT_SELECTION_COLOR),
      m_selection_start(-1), m_selection_end(-1), m_selecting(false),
      m_last_click_pos(0, 0) {
    DebugName = m_parent ? m_parent->DebugName + ",Label" : "Label";
    if (m_theme) {
        m_font_size = m_theme->m_standard_font_size;
        m_color = m_theme->m_text_color;
    }
    if (font_size >= 0) {
        m_font_size = font_size;
    } else if (m_font_size < 0) {
        m_font_size = 16; // Fallback default font size
    }
    m_processed_text = caption; // Initialize with raw caption
}

void Label::set_theme(Theme *theme) {
    Widget::set_theme(theme);
    if (m_theme) {
        m_font_size = m_theme->m_standard_font_size;
        m_color = m_theme->m_text_color;
        m_selection_color = DEFAULT_SELECTION_COLOR; // Reset to default
    }
    m_cache_valid = false; // Invalidate cache on theme change
    m_selection_start = m_selection_end = -1; // Clear selection
}

void Label::set_caption(const std::string &caption) {
    if (m_caption != caption) {
        m_caption = caption;
        m_processed_text = caption; // Reset processed text
        m_cache_valid = false; // Invalidate cache
        m_selection_start = m_selection_end = -1; // Clear selection
    }
}

void Label::set_font(const std::string &font) {
    if (m_font != font && !font.empty()) {
        m_font = font;
        m_cache_valid = false; // Invalidate cache
        m_selection_start = m_selection_end = -1; // Clear selection
    }
}

void Label::set_line_break_mode(LineBreakMode mode) {
    if (m_line_break_mode != mode) {
        m_line_break_mode = mode;
        m_cache_valid = false; // Invalidate cache
        m_selection_start = m_selection_end = -1; // Clear selection
    }
}

void Label::set_fixed_size(const Vector2i &fixed_size) {
    if (m_min_size != fixed_size) {
        Widget::set_fixed_size(fixed_size);
        m_cache_valid = false; // Invalidate cache
        m_selection_start = m_selection_end = -1; // Clear selection
    }
}

void Label::set_selectable(bool selectable) {
    if (m_selectable != selectable) {
        m_selectable = selectable;
        m_cache_valid = false; // Invalidate cache for rendering changes
        m_selection_start = m_selection_end = -1; // Clear selection
    }
}

Vector2i Label::preferred_size(NVGcontext *ctx) const {
    std::lock_guard<std::mutex> lock(m_cache_mutex); // Ensure thread safety

    if (!m_cache_valid) {
        if (m_caption.empty()) {
            m_cached_size = Vector2i(0);
            m_processed_text = "";
            m_cache_valid = true;
            return m_cached_size;
        }

        nvgFontFace(ctx, m_font.c_str());
        nvgFontSize(ctx, static_cast<float>(font_size()));

        if (m_min_size.x() > 0) {
                    m_processed_text = process_text_for_mode(ctx, m_caption, m_min_size.x());
            float bounds[4];
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

            switch (m_line_break_mode) {
                case LineBreakMode::LineBreakByWordWrapping:
                case LineBreakMode::LineBreakByCharWrapping:
                    nvgTextBoxBounds(ctx, 0, 0, m_min_size.x(), m_processed_text.c_str(), nullptr, bounds);
                    m_cached_size = Vector2i(m_min_size.x(), bounds[3] - bounds[1]);
                    break;

                case LineBreakMode::LineBreakByClipping:
                case LineBreakMode::LineBreakByTruncatingHead:
                case LineBreakMode::LineBreakByTruncatingTail:
                case LineBreakMode::LineBreakByTruncatingMiddle:
                    m_cached_size = Vector2i(m_min_size.x(), font_size());
                    break;
            }
        } else {
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            m_processed_text = m_caption; // No processing needed without fixed width
            m_cached_size = Vector2i(measure_text_width(ctx, m_caption) + TEXT_MARGIN, font_size());
        }
        m_cache_valid = true;
    }
    return m_cached_size;
}

void Label::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    std::lock_guard<std::mutex> lock(m_cache_mutex); // Ensure thread safety

    nvgFontFace(ctx, m_font.c_str());
    nvgFontSize(ctx, static_cast<float>(font_size()));
    nvgFillColor(ctx, m_color);

    // Draw selection highlight if selectable and text is selected
    if (m_selectable && m_selection_start != m_selection_end && !m_processed_text.empty()) {
        std::vector<float> bounds;
        int line_count = get_selection_bounds(ctx, m_selection_start, m_selection_end, bounds);
        nvgFillColor(ctx, m_selection_color);
        for (int i = 0; i < line_count; ++i) {
            nvgBeginPath(ctx);
            nvgRect(ctx, bounds[i * 4], bounds[i * 4 + 1], bounds[i * 4 + 2] - bounds[i * 4], bounds[i * 4 + 3] - bounds[i * 4 + 1]);
            nvgFill(ctx);
        }
    }

    // Draw text
    if (m_min_size.x() > 0) {
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        switch (m_line_break_mode) {
            case LineBreakMode::LineBreakByWordWrapping:
            case LineBreakMode::LineBreakByCharWrapping:
                nvgTextBox(ctx, m_pos.x(), m_pos.y(), m_min_size.x(), m_processed_text.c_str(), nullptr);
                break;

            case LineBreakMode::LineBreakByClipping:
            case LineBreakMode::LineBreakByTruncatingHead:
            case LineBreakMode::LineBreakByTruncatingTail:
            case LineBreakMode::LineBreakByTruncatingMiddle:
                nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgText(ctx, m_pos.x(), m_pos.y() + m_size.y() * 0.5f, m_processed_text.c_str(), nullptr);
                break;
        }
    } else {
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, m_pos.x(), m_pos.y() + m_size.y() * 0.5f, m_processed_text.c_str(), nullptr);
    }
}

bool Label::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    if (!m_selectable || !visible() || !enabled()) {
        return Widget::mouse_button_event(p, button, down, modifiers);
    }

    std::lock_guard<std::mutex> lock(m_cache_mutex);

    if (button == GLFW_MOUSE_BUTTON_1 && down) {
        if (!contains(p)) {
            m_selection_start = m_selection_end = -1;
            m_selecting = false;
            return false;
        }

        // Request focus
        request_focus();

        // Start selection
        m_selecting = true;
        Screen* sc = screen();
        NVGcontext* ctx = sc ? sc->nvg_context() : nullptr;
        int char_index = ctx ? find_char_index(ctx, p - m_pos) : -1;
        m_selection_start = m_selection_end = char_index;

        // Handle double-click to select all text
        if (sc) {
            double current_time = sc->m_last_interaction;
            if (current_time - m_last_interaction < 0.3 && p == m_last_click_pos && char_index >= 0) {
                m_selection_start = 0;
                m_selection_end = static_cast<int>(m_processed_text.length());
            }
            m_last_interaction = current_time;
        }
        m_last_click_pos = p;
        return true;
    } else if (button == GLFW_MOUSE_BUTTON_1 && !down && m_selecting) {
        m_selecting = false;
        return true;
    }
    return Widget::mouse_button_event(p, button, down, modifiers);
}

bool Label::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (!m_selectable || !visible() || !enabled()) {
        return Widget::mouse_motion_event(p, rel, button, modifiers);
    }

    std::lock_guard<std::mutex> lock(m_cache_mutex);

    if (m_selecting && (button & (1 << GLFW_MOUSE_BUTTON_1))) {
        Screen* sc = screen();
        NVGcontext* ctx = sc ? sc->nvg_context() : nullptr;
        if (ctx && contains(p)) {
            m_selection_end = find_char_index(ctx, p - m_pos);
            // Ensure selection is ordered
            if (m_selection_start > m_selection_end) {
                std::swap(m_selection_start, m_selection_end);
            }
        } else {
            m_selection_end = m_selection_start; // Clear selection if outside bounds
        }
        return true;
    }
    return Widget::mouse_motion_event(p, rel, button, modifiers);
}

bool Label::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (!m_selectable || !focused() || !visible() || !enabled()) {
        return Widget::keyboard_event(key, scancode, action, modifiers);
    }

    std::lock_guard<std::mutex> lock(m_cache_mutex);

    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        bool ctrl_pressed = modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER); // Ctrl or Cmd
        if (key == GLFW_KEY_C && ctrl_pressed && m_selection_start != m_selection_end) {
            // Copy selected text to clipboard
            Screen* sc = screen();
            if (sc && m_selection_start >= 0 && m_selection_end <= static_cast<int>(m_processed_text.length())) {
                glfwSetClipboardString(sc->glfw_window(),
                                      m_processed_text.substr(m_selection_start, m_selection_end - m_selection_start).c_str());
            }
            return true;
        }
    }
    return Widget::keyboard_event(key, scancode, action, modifiers);
}

int Label::find_char_index(NVGcontext *ctx, const Vector2i &pos) const {
    if (!ctx || m_processed_text.empty()) {
        return -1;
    }

    nvgFontFace(ctx, m_font.c_str());
    nvgFontSize(ctx, static_cast<float>(font_size()));
    nvgTextAlign(ctx, m_min_size.x() > 0 && (m_line_break_mode == LineBreakMode::LineBreakByWordWrapping ||
                                               m_line_break_mode == LineBreakMode::LineBreakByCharWrapping)
                     ? NVG_ALIGN_LEFT | NVG_ALIGN_TOP
                     : NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    float x = static_cast<float>(pos.x());
    float y = static_cast<float>(pos.y());
    float line_height = font_size();
    std::vector<std::string> lines;
    std::istringstream iss(m_processed_text);
    std::string line;
    while (std::getline(iss, line, '\n')) {
        lines.push_back(line);
    }

    if (m_min_size.x() > 0 && (m_line_break_mode == LineBreakMode::LineBreakByWordWrapping ||
                                 m_line_break_mode == LineBreakMode::LineBreakByCharWrapping)) {
        // Multi-line text
        int line_index = static_cast<int>(y / line_height);
        if (line_index < 0 || line_index >= static_cast<int>(lines.size())) {
            return -1;
        }
        const std::string &current_line = lines[line_index];
        float bounds[4];
        for (size_t i = 0; i <= current_line.length(); ++i) {
            std::string substr = current_line.substr(0, i);
            nvgTextBounds(ctx, 0, 0, substr.c_str(), nullptr, bounds);
            if (x < bounds[2]) {
                return static_cast<int>(std::accumulate(lines.begin(), lines.begin() + line_index, size_t(0),
                                                        [](size_t sum, const std::string &l) { return sum + l.length() + 1; }) + i);
            }
        }
        return static_cast<int>(std::accumulate(lines.begin(), lines.begin() + line_index + 1, size_t(0),
                                                [](size_t sum, const std::string &l) { return sum + l.length() + 1; }));
    } else {
        // Single-line text
        float bounds[4];
        for (size_t i = 0; i <= m_processed_text.length(); ++i) {
            std::string substr = m_processed_text.substr(0, i);
            nvgTextBounds(ctx, 0, 0, substr.c_str(), nullptr, bounds);
            if (x < bounds[2]) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(m_processed_text.length());
    }
}

int Label::get_selection_bounds(NVGcontext *ctx, int start, int end, std::vector<float> &bounds) const {
    if (!ctx || start < 0 || end > static_cast<int>(m_processed_text.length()) || start >= end) {
        return 0;
    }

    nvgFontFace(ctx, m_font.c_str());
    nvgFontSize(ctx, static_cast<float>(font_size()));
    nvgTextAlign(ctx, m_min_size.x() > 0 && (m_line_break_mode == LineBreakMode::LineBreakByWordWrapping ||
                                               m_line_break_mode == LineBreakMode::LineBreakByCharWrapping)
                     ? NVG_ALIGN_LEFT | NVG_ALIGN_TOP
                     : NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    float line_height = font_size();
    std::vector<std::string> lines;
    std::istringstream iss(m_processed_text);
    std::string line;
    while (std::getline(iss, line, '\n')) {
        lines.push_back(line);
    }

    int char_count = 0;
    int start_line = -1, end_line = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        int line_start = char_count;
        int line_end = char_count + static_cast<int>(lines[i].length());
        char_count += lines[i].length() + 1; // Include newline
        if (start >= line_start && start <= line_end && start_line == -1) {
            start_line = static_cast<int>(i);
        }
        if (end > line_start && end <= line_end + 1) {
            end_line = static_cast<int>(i);
            break;
        }
    }
    if (start_line == -1 || end_line == -1) {
        return 0;
    }

    bounds.clear();
    float y_offset = (m_line_break_mode == LineBreakMode::LineBreakByWordWrapping ||
                      m_line_break_mode == LineBreakMode::LineBreakByCharWrapping) ? 0 : m_size.y() * 0.5f;

    for (int i = start_line; i <= end_line; ++i) {
        int line_start = static_cast<int>(std::accumulate(lines.begin(), lines.begin() + i, size_t(0),
                                                          [](size_t sum, const std::string &l) { return sum + l.length() + 1; }));
        int line_end = line_start + static_cast<int>(lines[i].length());
        int sel_start = std::max(start, line_start);
        int sel_end = std::min(end, line_end);

        if (sel_start < sel_end) {
            float bounds_start[4], bounds_end[4];
            std::string substr_start = m_processed_text.substr(line_start, sel_start - line_start);
            std::string substr_end = m_processed_text.substr(line_start, sel_end - line_start);
            nvgTextBounds(ctx, m_pos.x(), m_pos.y() + y_offset + i * line_height, substr_start.c_str(), nullptr, bounds_start);
            nvgTextBounds(ctx, m_pos.x(), m_pos.y() + y_offset + i * line_height, substr_end.c_str(), nullptr, bounds_end);
            bounds.push_back(bounds_start[0]); // x_min
            bounds.push_back(m_pos.y() + i * line_height); // y_min
            bounds.push_back(bounds_end[2]); // x_max
            bounds.push_back(m_pos.y() + (i + 1) * line_height); // y_max
        }
    }
    return static_cast<int>(bounds.size() / 4);
}

std::string Label::process_text_for_mode(NVGcontext *ctx, const std::string &text, float available_width) const {
    switch (m_line_break_mode) {
        case LineBreakMode::LineBreakByWordWrapping:
            return text; // Handled by nvgTextBox
        case LineBreakMode::LineBreakByCharWrapping:
            return wrap_by_character(ctx, text, available_width);
        case LineBreakMode::LineBreakByClipping:
            return clip_text(ctx, text, available_width);
        case LineBreakMode::LineBreakByTruncatingHead:
            return truncate_head(ctx, text, available_width);
        case LineBreakMode::LineBreakByTruncatingTail:
            return truncate_tail(ctx, text, available_width);
        case LineBreakMode::LineBreakByTruncatingMiddle:
            return truncate_middle(ctx, text, available_width);
        default:
            return text;
    }
}

std::string Label::wrap_by_character(NVGcontext *ctx, const std::string &text, float available_width) const {
    if (text.empty() || available_width <= 0) {
        return text;
    }

    std::string result;
    result.reserve(text.size() + text.size() / 10); // Pre-allocate with estimated newlines
    std::string current_line;
    current_line.reserve(64); // Reasonable initial capacity for a line

    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        current_line += c;

        if (measure_text_width(ctx, current_line) > available_width && !current_line.empty()) {
            result += current_line.substr(0, current_line.size() - 1) + '\n';
            current_line = c;
        }
    }
    if (!current_line.empty()) {
        result += current_line;
    }
    return result;
}

std::string Label::truncate_head(NVGcontext *ctx, const std::string &text, float available_width) const {
    if (text.empty() || available_width <= 0) {
        return text;
    }

    float text_width = measure_text_width(ctx, text);
    if (text_width <= available_width) {
        return text;
    }

    float ellipsis_width = measure_text_width(ctx, ELLIPSIS);
    float target_width = available_width - ellipsis_width;
    if (target_width <= 0) {
        return ELLIPSIS;
    }

    size_t left = 0, right = text.length();
    std::string best_fit;
    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        std::string candidate = text.substr(text.length() - mid);
        if (measure_text_width(ctx, candidate) <= target_width) {
            best_fit = candidate;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return ELLIPSIS + best_fit;
}

std::string Label::truncate_tail(NVGcontext *ctx, const std::string &text, float available_width) const {
    if (text.empty() || available_width <= 0) {
        return text;
    }

    float text_width = measure_text_width(ctx, text);
    if (text_width <= available_width) {
        return text;
    }

    float ellipsis_width = measure_text_width(ctx, ELLIPSIS);
    float target_width = available_width - ellipsis_width;
    if (target_width <= 0) {
        return ELLIPSIS;
    }

    size_t left = 0, right = text.length();
    std::string best_fit;
    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        std::string candidate = text.substr(0, mid);
        if (measure_text_width(ctx, candidate) <= target_width) {
            best_fit = candidate;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return best_fit + ELLIPSIS;
}

std::string Label::truncate_middle(NVGcontext *ctx, const std::string &text, float available_width) const {
    if (text.empty() || available_width <= 0) {
        return text;
    }

    float text_width = measure_text_width(ctx, text);
    if (text_width <= available_width) {
        return text;
    }

    float ellipsis_width = measure_text_width(ctx, ELLIPSIS);
    float target_width = available_width - ellipsis_width;
    if (target_width <= 0) {
        return ELLIPSIS;
    }

    size_t left = 0, right = text.length() / 2;
    std::string best_result = ELLIPSIS;
    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        size_t head_len = mid;
        size_t tail_len = mid;
        if (head_len + tail_len >= text.length()) {
            right = mid - 1;
            continue;
        }

        std::string candidate = text.substr(0, head_len) + text.substr(text.length() - tail_len);
        if (measure_text_width(ctx, candidate) <= target_width) {
            best_result = text.substr(0, head_len) + ELLIPSIS + text.substr(text.length() - tail_len);
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return best_result;
}

std::string Label::clip_text(NVGcontext *ctx, const std::string &text, float available_width) const {
    if (text.empty() || available_width <= 0) {
        return text;
    }

    float text_width = measure_text_width(ctx, text);
    if (text_width <= available_width) {
        return text;
    }

    size_t left = 0, right = text.length();
    std::string best_fit;
    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        std::string candidate = text.substr(0, mid);
        if (measure_text_width(ctx, candidate) <= available_width) {
            best_fit = candidate;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return best_fit;
}

NAMESPACE_END(nanogui)
