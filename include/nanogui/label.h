/*
 nanogui/label.h -- Text label with an arbitrary font, color, and size

 NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
 The widget drawing code is based on the NanoVG demo application
 by Mikko Mononen.

 All rights reserved. Use of this source code is governed by a
 BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include <nanogui/widget.h>
#include <mutex>

NAMESPACE_BEGIN(nanogui)

/**
 * \class Label label.h nanogui/label.h
 *
 * \brief Text label widget for displaying text with customizable font, color, and size.
 *
 * Supports various line-breaking modes for text wrapping or truncation when a fixed width
 * is set via \ref Widget::set_fixed_width(). The label can wrap text at word or character
 * boundaries, clip text, or truncate it with an ellipsis at the head, tail, or middle.
 * Optionally, the text can be made selectable and copyable to the system clipboard.
 */
class NANOGUI_EXPORT Label : public WidgetCRTP<Label> {
public:
 /// Line breaking modes for text rendering.
 enum class LineBreakMode {
 LineBreakByWordWrapping, ///< Wrap text at word boundaries.
 LineBreakByCharWrapping, ///< Wrap text at any character.
 LineBreakByClipping, ///< Clip text that exceeds the width.
 LineBreakByTruncatingHead, ///< Truncate text from the beginning with "...".
 LineBreakByTruncatingTail, ///< Truncate text from the end with "...".
 LineBreakByTruncatingMiddle ///< Truncate text from the middle with "...".
 };

 /**
 * \brief Construct a label widget.
 * \param parent The parent widget.
 * \param caption The initial text to display.
 * \param font The font name (e.g., "sans", "sans-bold"). Defaults to "sans".
 * \param font_size The font size in pixels. If negative, uses the theme's standard font size.
 */
 Label(Widget *parent, const std::string &caption,
 const std::string &font = "sans", int font_size = -1);

 /// Get the label's text caption.
 const std::string &caption() const { return m_caption; }

 /**
 * \brief Set the label's text caption and invalidate cached text.
 * \param caption The new text to display.
 */
 void set_caption(const std::string &caption);

 /**
 * \brief Set the font for rendering text.
 * \param font The font name (e.g., "sans", "sans-bold").
 */
 void set_font(const std::string &font);

 /// Get the currently active font.
 const std::string &font() const { return m_font; }

 /// Get the label's text color.
 Color color() const { return m_color; }

 /// Set the label's text color.
 void set_color(const Color& color) { m_color = color; }

 /// Get the line break mode.
 LineBreakMode line_break_mode() const { return m_line_break_mode; }

 /**
 * \brief Set the line break mode for text rendering.
 * \param mode The line breaking mode to use.
 */
 void set_line_break_mode(LineBreakMode mode);

 /**
 * \brief Set whether the label's text is selectable and copyable.
 * \param selectable If true, enables text selection and copying to clipboard.
 */
 void set_selectable(bool selectable);

 /// Get whether the label's text is selectable and copyable.
 bool selectable() const { return m_selectable; }

 /// Set the theme and update font size and color.
 virtual void set_theme(Theme *theme) override;

 /// Compute the preferred size needed to display the label.
 virtual Vector2i preferred_size(NVGcontext *ctx) const override;

 /// Draw the label with the current text and settings.
 virtual void draw(NVGcontext *ctx) override;

 /// Set the fixed size and invalidate cached text.
 virtual void set_fixed_size(const Vector2i &fixed_size) override;

 /// Handle mouse button events for text selection.
 virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override;

 /// Handle mouse motion events for text selection.
 virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override;

 /// Handle keyboard events for copying text to clipboard.
 virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override;

protected:
 /**
 * \brief Process text according to the line break mode.
 * \param ctx NanoVG context for text measurement.
 * \param text The input text to process.
 * \param available_width The width available for text rendering.
 * \return The processed text string.
 */
 std::string process_text_for_mode(NVGcontext *ctx, const std::string &text, float available_width) const;

 /**
 * \brief Wrap text by character boundaries.
 * \param ctx NanoVG context.
 * \param text Input text.
 * \param available_width Available width for wrapping.
 * \return Wrapped text with newline characters.
 */
 std::string wrap_by_character(NVGcontext *ctx, const std::string &text, float available_width) const;

 /**
 * \brief Truncate text with ellipsis at the beginning.
 * \param ctx NanoVG context.
 * \param text Input text.
 * \param available_width Available width.
 * \return Truncated text with ellipsis prefix.
 */
 std::string truncate_head(NVGcontext *ctx, const std::string &text, float available_width) const;

 /**
 * \brief Truncate text with ellipsis at the end.
 * \param ctx NanoVG context.
 * \param text Input text.
 * \param available_width Available width.
 * \return Truncated text with ellipsis suffix.
 */
 std::string truncate_tail(NVGcontext *ctx, const std::string &text, float available_width) const;

 /**
 * \brief Truncate text with ellipsis in the middle.
 * \param ctx NanoVG context.
 * \param text Input text.
 * \param available_width Available width.
 * \return Truncated text with ellipsis in the middle.
 */
 std::string truncate_middle(NVGcontext *ctx, const std::string &text, float available_width) const;

 /**
 * \brief Clip text to fit within the available width without ellipsis.
 * \param ctx NanoVG context.
 * \param text Input text.
 * \param available_width Available width.
 * \return Clipped text.
 */
 std::string clip_text(NVGcontext *ctx, const std::string &text, float available_width) const;

 /**
 * \brief Find the character index at a given mouse position.
 * \param ctx NanoVG context.
 * \param pos Mouse position relative to the label.
 * \return The character index or -1 if outside the text.
 */
 int find_char_index(NVGcontext *ctx, const Vector2i &pos) const;

 /**
 * \brief Get the bounding box for a range of characters.
 * \param ctx NanoVG context.
 * \param start Start character index.
 * \param end End character index.
 * \param[out] bounds Array of bounding boxes for each line in the selection.
 * \return Number of bounding boxes (lines) in the selection.
 */
 int get_selection_bounds(NVGcontext *ctx, int start, int end, std::vector<float> &bounds) const;

 std::string m_caption; ///< The label's text content.
 std::string m_font; ///< The font used for rendering.
 mutable std::string m_processed_text; ///< Cached processed text for rendering.
 Color m_color; ///< The text color.
 LineBreakMode m_line_break_mode; ///< The line breaking mode.
 mutable Vector2i m_cached_size; ///< Cached preferred size for performance.
 mutable bool m_cache_valid; ///< Flag indicating if cached data is valid.
 mutable std::mutex m_cache_mutex; ///< Mutex for thread-safe cache access.
 bool m_selectable; ///< Whether text is selectable and copyable.
 Color m_selection_color; ///< Color for the selection highlight.
 int m_selection_start; ///< Start index of the text selection.
 int m_selection_end; ///< End index of the text selection.
 bool m_selecting; ///< Whether a selection is being dragged.
 Vector2i m_last_click_pos; ///< Position of the last mouse click for double-click detection.
 double m_last_interaction; ///< Time of the last interaction for double-click detection.
};

NAMESPACE_END(nanogui)
