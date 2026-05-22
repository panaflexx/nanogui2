/*
    nanogui/widget.h -- Base class of all widgets

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/object.h>
#include <nanogui/theme.h>
#include <nanogui/fluent.h>
#include <algorithm>
#include <vector>


NAMESPACE_BEGIN(nanogui)

enum class Cursor; // do not put a docstring, this is already documented

enum class SizeMode {
    Minimum,   // Use min_size, no growth
    Preferred, // Use preferred_size, no growth/shrink
    Expanding, // Grow to fill available space
    Fixed      // Use fixed_size, no growth/shrink
};

/**
 * \class Widget widget.h nanogui/widget.h
 *
 * \brief Base class of all widgets.
 *
 * \ref Widget is the base class of all widgets in \c nanogui. It can
 * also be used as an panel to arrange an arbitrary number of child
 * widgets using a layout generator (see \ref Layout).
 */
class NANOGUI_EXPORT Widget : public Object {
public:
    std::string DebugName;
    /// Construct a new widget with the given parent widget
    Widget(Widget* parent);

    /// Return the parent widget
    Widget* parent() { return m_parent; }
    /// Return the parent widget
    const Widget* parent() const { return m_parent; }
    /// Set the parent widget
    Widget& set_parent(Widget* parent) { m_parent = parent; return *this; }

    /// Return the used \ref Layout generator
    Layout* layout() { return m_layout; }
    /// Return the used \ref Layout generator
    const Layout* layout() const { return m_layout.get(); }
    /// Set the used \ref Layout generator
    Widget& set_layout(Layout* layout) { m_layout = layout; return *this; }

    /// Return the \ref Theme used to draw this widget
    Theme* theme() { return m_theme; }
    /// Return the \ref Theme used to draw this widget
    const Theme* theme() const { return m_theme.get(); }
    /// Set the \ref Theme used to draw this widget
    virtual void set_theme(Theme* theme);

    /// Return the position relative to the parent widget
    const Vector2i& position() const { return m_pos; }
    /// Set the position relative to the parent widget
    Widget& set_position(const Vector2i& pos) { m_pos = pos; return *this; }

    /// Return the absolute position on screen
    Vector2i absolute_position() const {
        return m_parent ?
            (parent()->absolute_position() + m_pos) : m_pos;
    }

    /// Return the size of the widget
    //const Vector2i& size() const { if (this == NULL) return 0; else  return m_size; }
    const Vector2i& size() const { return m_size; }
    /// set the size of the widget
    Widget& set_size(const Vector2i& size) { m_size = size; return *this; }

	const Vector2i& min_size() const { return m_min_size; }
    Widget& set_min_size(const Vector2i& size) { m_min_size = size; return *this; }
	const Vector2i& max_size() const { return m_max_size; }
    Widget& set_max_size(const Vector2i& size) { m_max_size = size; return *this; }

    /// Return the width of the widget
    int width() const { return m_size.x(); }
    /// Set the width of the widget
    Widget& set_width(int width) { m_size.x() = width; m_min_size.x() = width; return *this; }

    /// Return the height of the widget
    int height() const { return m_size.y(); }
    /// Set the height of the widget
    Widget& set_height(int height) { m_size.y() = height; return *this; }

    /**
     * \brief Set the fixed size of this widget
     *
     * If nonzero, components of the fixed size attribute override any values
     * computed by a layout generator associated with this widget. Note that
     * just setting the fixed size alone is not enough to actually change its
     * size; this is done with a call to \ref set_size or a call to \ref perform_layout()
     * in the parent widget.
     */
    virtual void set_fixed_size(const Vector2i& fixed_size) { m_min_size = fixed_size; }

    /// Return the fixed size (see \ref set_fixed_size())

    const Vector2i& fixed_size() const { return m_min_size; }

    // Return the fixed width (see \ref set_fixed_size())
    int fixed_width() const { return m_min_size.x(); }
    // Return the fixed height (see \ref set_fixed_size())
    int fixed_height() const { return m_min_size.y(); }
    /// Set the fixed width (see \ref set_fixed_size())
    Widget& set_min_width(int width) { m_min_size.x() = width; return *this; }
    /// Set the fixed height (see \ref set_fixed_size())
    Widget& set_min_height(int height) { m_min_size.y() = height; return *this; }
	/// Set the fixed width (see \ref set_fixed_size())
    Widget& set_max_width(int width) { m_min_size.x() = width; return *this; }
    /// Set the fixed height (see \ref set_fixed_size())
    Widget& set_max_height(int height) { m_min_size.y() = height; return *this; }

	// New flex sizing
    Widget& set_width_flex(SizeMode mode) { m_width_mode = mode; return *this; }
    Widget& set_height_flex(SizeMode mode) { m_height_mode = mode; return *this; }
    SizeMode width_mode() const { return m_width_mode; }
    SizeMode height_mode() const { return m_height_mode; }

    /// Return whether or not the widget is currently visible (assuming all parents are visible)
    bool visible() const { return m_visible; }
    /// Set whether or not the widget is currently visible (assuming all parents are visible)
    Widget& set_visible(bool visible) { m_visible = visible; return *this; }

    /// Check if this widget is currently visible, taking parent widgets into account
    bool visible_recursive() const {
        bool visible = true;
        const Widget* widget = this;
        while (widget) {
            visible &= widget->visible();
            widget = widget->parent();
        }
        return visible;
    }

    /// Return the number of child widgets
    int child_count() const { return (int)m_children.size(); }

    /// Return the list of child widgets of the current widget
    const std::vector<Widget*>& children() const { return m_children; }

    /**
     * \brief Add a child widget to the current widget at
     * the specified index.
     *
     * This function almost never needs to be called by hand,
     * since the constructor of \ref Widget automatically
     * adds the current widget to its parent
     */
    virtual void add_child(int index, Widget* widget);

    /// Convenience function which appends a widget at the end
    void add_child(Widget* widget);

    /// Remove a child widget by index
    void remove_child_at(int index);

    /// Remove a child widget by value
    void remove_child(const Widget* widget);

    /// Retrieves the child at the specific position
    const Widget* child_at(int index) const { return m_children[index]; }

    /// Retrieves the child at the specific position
    Widget* child_at(int index) { return m_children[index]; }

    /// Returns the index of a specific child or -1 if not found
    int child_index(Widget* widget) const;

    /// Walk up the hierarchy and return the parent window
    Window* window();
    /// Walk up the hierarchy and return the parent window (const version)
    const Window* window() const;

    /// Walk up the hierarchy and return the parent screen
    Screen* screen();
    /// Walk up the hierarchy and return the parent screen (const version)
    const Screen* screen() const;

    /// Return whether or not this widget is currently enabled
    bool enabled() const { return m_enabled; }
    /// Set whether or not this widget is currently enabled
    Widget& set_enabled(bool enabled) { m_enabled = enabled; return *this; }

    /// Return whether or not this widget is currently focused
    bool focused() const { return m_focused; }
    /// Set whether or not this widget is currently focused
    Widget& set_focused(bool focused) { m_focused = focused; return *this; }
    /// Request the focus to be moved to this widget
    void request_focus();

    const std::string& tooltip() const { return m_tooltip; }
    Widget& set_tooltip(const std::string& tooltip) { m_tooltip = tooltip; return *this; }

    const std::string& id() const { return m_id; }
    Widget& set_id(const std::string& id) { m_id = id; return *this; }

    /// Return current font size. If not set the default of the current theme will be returned
    int font_size() const;
    /// Set the font size of this widget
    Widget& set_font_size(int font_size) { m_font_size = font_size; return *this; }
    /// Return whether the font size is explicitly specified for this widget
    bool has_font_size() const { return m_font_size > 0; }

    /**
     * The amount of extra scaling applied to *icon* fonts.
     * See \ref nanogui::Widget::m_icon_extra_scale.
     */
    float icon_extra_scale() const { return m_icon_extra_scale; }

    /**
     * Sets the amount of extra scaling applied to *icon* fonts.
     * See \ref nanogui::Widget::m_icon_extra_scale.
     */
    Widget& set_icon_extra_scale(float scale) { m_icon_extra_scale = scale; return *this; }

    /// Return a pointer to the cursor of the widget
    Cursor cursor() const { return m_cursor; }
    /// Set the cursor of the widget
    Widget& set_cursor(Cursor cursor) { m_cursor = cursor; return *this; }

    /// Check if the widget contains a certain position
    bool contains(const Vector2i& p) const {
        Vector2i d = p - m_pos;
        return d.x() >= 0 && d.y() >= 0 &&
            d.x() < m_size.x() && d.y() < m_size.y();
    }

    /// Determine the widget located at the given position value (recursive)
    Widget* find_widget(const Vector2i& p);
    const Widget* find_widget(const Vector2i& p) const;

    /// Handle a mouse button event (default implementation: propagate to children)
    virtual bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers);

    /// Handle a mouse motion event (default implementation: propagate to children)
    virtual bool mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers);

    /// Handle a mouse drag event (default implementation: do nothing)
    virtual bool mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers);

    /// Handle a mouse enter/leave event (default implementation: record this fact, but do nothing)
    virtual bool mouse_enter_event(const Vector2i& p, bool enter);

    /// Handle a mouse scroll event (default implementation: propagate to children)
    virtual bool scroll_event(const Vector2i& p, const Vector2f& rel);

    /// Handle a pinch-to-zoom gesture (default: propagate to children)
    virtual bool zoom_event(double magnification, const Vector2i& pos);

    /// Handle a focus change event (default implementation: record the focus status, but do nothing)
    virtual bool focus_event(bool focused);

    /// Handle a keyboard event (default implementation: do nothing)
    virtual bool keyboard_event(int key, int scancode, int action, int modifiers);

    /// Handle text input (UTF-32 format) (default implementation: do nothing)
    virtual bool keyboard_character_event(unsigned int codepoint);

    /// Compute the preferred size of the widget
    virtual Vector2i preferred_size(NVGcontext* ctx) const;

    /// Invoke the associated layout generator to properly place child widgets, if any
    virtual void perform_layout(NVGcontext* ctx);

    /// Draw the widget (and all child widgets)
    virtual void draw(NVGcontext* ctx);

	// Animation
	enum class AnimationType {
        None,
        Sproing,    // Bounce/scale effect (expands then contracts with decay)
        Warble,     // Oscillating/wavy scale effect
        Rotate,     // Smooth rotation effect
        SlideOpen,  // Slide in horizontally (translate from left + fade in)
        SlideClose, // Slide out horizontally (translate to left + fade out)
        SlideUp,    // Collapse vertically to 0 height + fade out (hides at end)
        SlideDown   // Expand vertically from 0 height + fade in
        // Add more types here as needed, e.g., Fade, Pulse
    };

	void set_animation_type(AnimationType type) { m_animation_type = type; }
    void set_animation_duration(double duration) { m_animation_duration = duration; }
    AnimationType animation_type() const { return m_animation_type; }
    double animation_duration() const { return m_animation_duration; }
    bool animating() const { return m_animation_start >= 0.0; }
    void start_animation(AnimationType type = AnimationType::None);
    /// Stop any running animation immediately
    void stop_animation();
    /// Alias for stop_animation (some older headers used the plural form)
    void stop_animations();

    /**
     * Returns true if a currently-running animation wants to override the
     * widget's effective layout size (e.g. SlideUp / SlideDown which drive
     * the widget's height directly). Layouts that honor this override
     * (BoxLayout, FlexLayout) ask the widget via \ref layout_min_size and
     * \ref layout_preferred_size rather than reading m_min_size / calling
     * preferred_size() directly.
     */
    bool animation_overrides_layout_size() const {
        return animating() &&
               (m_animation_type == AnimationType::SlideUp ||
                m_animation_type == AnimationType::SlideDown);
    }

    /// Min size a layout should use for this widget. Identical to
    /// min_size() except that SlideUp / SlideDown drop the height floor
    /// to 0 so the widget can shrink past its normal minimum.
    Vector2i layout_min_size() const {
        Vector2i ms = m_min_size;
        if (animation_overrides_layout_size())
            ms.y() = 0;
        return ms;
    }

    /// Preferred size a layout should use for this widget. Identical to
    /// preferred_size(ctx) except that SlideUp / SlideDown substitute the
    /// current animated height so the layout honors the collapse / expand
    /// effect instead of snapping back to the intrinsic preferred height.
    Vector2i layout_preferred_size(NVGcontext* ctx) const {
        Vector2i ps = preferred_size(ctx);
        if (animation_overrides_layout_size())
            ps.y() = m_size.y();
        return ps;
    }

	template <typename T, typename... Args>
    Fluent<T> add(Args&&... args) {
        T* child = new T(this, std::forward<Args>(args)...);
        return Fluent<T>(child);
    }

protected:
    /// Free all resources used by the widget and any children
    virtual ~Widget();

    /**
     * Convenience definition for subclasses to get the full icon scale for this
     * class of Widget.  It simple returns the value
     * ``m_theme->m_icon_scale * this->m_icon_extra_scale``.
     *
     * \remark
     *     See also: \ref nanogui::Theme::m_icon_scale and
     *     \ref nanogui::Widget::m_icon_extra_scale.  This tiered scaling
     *     strategy may not be appropriate with fonts other than ``entypo.ttf``.
     */
    float icon_scale() const { return m_theme->m_icon_scale * m_icon_extra_scale; }

    Widget* m_parent;
    ref<Theme> m_theme;
    ref<Layout> m_layout;
    //Vector2i m_pos, m_size, m_fixed_size;
	Vector2i m_pos, m_size, m_min_size, m_max_size;
    std::vector<Widget*> m_children;

	SizeMode m_width_mode = SizeMode::Preferred;
    SizeMode m_height_mode = SizeMode::Preferred;

    /**
     * Whether or not this Widget is currently visible.  When a Widget is not
     * currently visible, no time is wasted executing its drawing method.
     */
    bool m_visible;

    /**
     * Whether or not this Widget is currently enabled.  Various different kinds
     * of derived types use this to determine whether or not user input will be
     * accepted.  For example, when ``m_enabled == false``, the state of a
     * CheckBox cannot be changed, or a TextBox will not allow new input.
     */
    bool m_enabled;
    bool m_focused, m_mouse_focus;
    std::string m_tooltip, m_id;
    int m_font_size;

    /**
     * \brief The amount of extra icon scaling used in addition the the theme's
     *        default icon font scale.  Default value is ``1.0``, which implies
     *        that \ref nanogui::Widget::icon_scale simply returns the value
     *        of \ref nanogui::Theme::m_icon_scale.
     *
     * Most widgets do not need extra scaling, but some (e.g., CheckBox, TextBox)
     * need to adjust the Theme's default icon scaling
     * (\ref nanogui::Theme::m_icon_scale) to properly display icons within their
     * bounds (upscale, or downscale).
     *
     * \rst
     * .. note::
     *
     *    When using ``nvgFontSize`` for icons in subclasses, make sure to call
     *    the :func:`nanogui::Widget::icon_scale` function.  Expected usage when
     *    *drawing* icon fonts is something like:
     *
     *    .. code-block:: cpp
     *
     *       virtual void draw(NVGcontext *ctx) {
     *           // fontSize depends on the kind of Widget.  Search for `FontSize`
     *           // in the Theme class (e.g., standard vs button)
     *           float ih = font_size;
     *           // assuming your Widget has a declared `mIcon`
     *           if (nvgIsFontIcon(mIcon)) {
     *               ih *= icon_scale();
     *               nvgFontFace(ctx, "icons");
     *               nvgFontSize(ctx, ih);
     *               /// remaining drawing code (see button.cpp for more)
     *           }
     *       }
     * \endrst
     */
    float m_icon_extra_scale;
    Cursor m_cursor;

	// Animation support
	AnimationType m_animation_type = AnimationType::None;
    double m_animation_start = -1.0;
    double m_animation_duration = 0.5;  // Default duration in seconds
    // Captured at start_animation() time; used by SlideUp / SlideDown to
    // restore the widget's size / fixed-size when the animation ends.
    Vector2i m_animation_original_size = Vector2i(0, 0);
    Vector2i m_animation_original_min_size = Vector2i(0, 0);
	virtual void apply_animation_transform(NVGcontext* ctx, float progress);
	virtual void end_animation();
	std::pair<bool, float> get_animation_progress();
};

template <typename T, typename... Args>
Fluent<T> Make(Widget* parent, Args&&... args) {
    return parent->add<T>(std::forward<Args>(args)...);
}

/**
 * CRTP mixin that provides fluent setters returning the derived type.
 * Concrete widgets should inherit from WidgetCRTP<TheirClass>.
 */
template <typename Derived>
class WidgetCRTP : public Widget {
public:
    using Widget::Widget;

    Derived& set_size(const Vector2i& size) {
        Widget::set_size(size);
        return static_cast<Derived&>(*this);
    }

    Derived& set_position(const Vector2i& pos) {
        Widget::set_position(pos);
        return static_cast<Derived&>(*this);
    }

    Derived& set_enabled(bool enabled) {
        Widget::set_enabled(enabled);
        return static_cast<Derived&>(*this);
    }

    Derived& set_visible(bool visible) {
        Widget::set_visible(visible);
        return static_cast<Derived&>(*this);
    }
};

NAMESPACE_END(nanogui)
