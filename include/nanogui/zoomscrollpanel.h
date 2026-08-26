/*
    nanogui/zoomscrollpanel.h -- A scroll panel that also supports
    smooth zooming (and eventually pinch gestures) of its child.

    Adapted from ScrollPanel. The zoom factor is stored as a double so
    that very smooth incremental updates from trackpad/pinch gestures
    are possible without precision loss.

    The child is laid out in "logical" (unscaled) coordinates. The panel
    visually applies a uniform scale (m_zoom) and a pan offset
    (m_pan_offset, measured in panel-local pixels) when drawing, and
    transforms all incoming mouse / scroll events to the child's logical
    coordinate space before forwarding them.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/widget.h>

NAMESPACE_BEGIN(nanogui)

/**
 * \class ZoomScrollPanel zoomscrollpanel.h nanogui/zoomscrollpanel.h
 *
 * \brief A container that adds independent X/Y scrolling AND a smooth
 *        uniform zoom factor (double precision) around its single child.
 *
 * Designed to be a drop-in upgrade of ScrollPanel for cases that also
 * want zoom/pinch support. The zoom uses double precision so that very
 * small incremental updates (e.g. from a trackpad pinch gesture or
 * inertial scroll) accumulate cleanly without drifting.
 *
 * Default input bindings (subject to change):
 *   - Mouse wheel              : vertical scroll
 *   - Shift + Mouse wheel      : horizontal scroll
 *   - Ctrl  + Mouse wheel      : zoom about cursor position
 *   - Middle-drag              : pan
 *   - Click on scroll bar      : jump-scroll
 *   - Drag scroll bar          : scroll
 *
 * The child is positioned at (0,0) in its own logical coordinate space;
 * the panel handles all visual translation and scaling itself.
 */
class NANOGUI_EXPORT ZoomScrollPanel : public Widget {
public:
    enum class ScrollTypes {
        Horizontal,
        Vertical,
        Both,
        None
    };

    /// Double-precision 2D vector for pan / zoom state.
    using Vector2d = Array<double, 2>;

    ZoomScrollPanel(Widget* parent);
    ZoomScrollPanel(Widget* parent, ScrollTypes scroll_type);

    bool VScrollable() const {
        return (m_scroll_type == ScrollTypes::Vertical ||
                m_scroll_type == ScrollTypes::Both);
    }
    bool HScrollable() const {
        return (m_scroll_type == ScrollTypes::Horizontal ||
                m_scroll_type == ScrollTypes::Both);
    }

    /// Return the current zoom factor (1.0 == no zoom).
    double zoom() const { return m_zoom; }

    /**
     * Set the zoom factor directly. The pan is preserved relative to
     * the top-left of the panel. Use \ref set_zoom_about() to zoom
     * around a specific anchor point.
     */
    void set_zoom(double z);

    /**
     * Set the zoom factor while keeping the given panel-local pixel
     * \p anchor stationary on screen. This is the operation you want
     * for pinch and Ctrl+wheel zoom.
     */
    void set_zoom_about(double z, const Vector2i& anchor);

    /// Multiply current zoom by \p factor about the given anchor (panel-local pixels).
    void zoom_by(double factor, const Vector2i& anchor) {
        set_zoom_about(m_zoom * factor, anchor);
    }

    /// Reset zoom to 1.0 and pan to (0,0).
    void reset_view();

    double zoom_min() const { return m_zoom_min; }
    double zoom_max() const { return m_zoom_max; }
    void set_zoom_range(double lo, double hi) { m_zoom_min = lo; m_zoom_max = hi; clamp_state(); }

    /// Enable/disable interactive zoom (Ctrl+wheel / pinch).
    bool zoom_enabled() const { return m_zoom_enabled; }
    void set_zoom_enabled(bool e) { m_zoom_enabled = e; }

    /// Pan offset in panel-local pixels (negative values move the child up/left).
    Vector2d pan_offset() const { return m_pan_offset; }
    void set_pan_offset(const Vector2d& p) { m_pan_offset = p; clamp_state(); }

    /**
     * Return scroll amount as a normalized 0..1 value derived from the
     * pan offset and the effective (scaled) child size. Provided for
     * compatibility with code that expects ScrollPanel's interface.
     */
    Vector2f scroll() const;

    /// Set scroll amount as a normalized 0..1 value (per axis).
    void set_scroll(const Vector2f& s);

    void set_scroll_type(ScrollTypes t) { m_scroll_type = t; }
    ScrollTypes scroll_type() const { return m_scroll_type; }

    void set_scroll(float y) { set_scroll(Vector2f(scroll().x(), y)); }

    bool reflow_on_zoom() const { return m_reflow_on_zoom; }
    void set_reflow_on_zoom(bool v) { m_reflow_on_zoom = v; }

    /* ---- coordinate helpers ---- */

    /// Convert a point in this panel's parent coord space to child logical space.
    Vector2i to_child(const Vector2i& parent_pt) const;

    /// Convert a delta in pixels (panel space) to a delta in child logical units.
    Vector2i delta_to_child(const Vector2i& rel) const;

    /* ---- Widget overrides ---- */

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override;
    virtual void perform_layout(NVGcontext* ctx) override;
    virtual Vector2i preferred_size(NVGcontext* ctx) const override;
    virtual bool mouse_button_event(const Vector2i& p, int button, bool down,
                                    int modifiers) override;
    virtual bool mouse_motion_event(const Vector2i& p, const Vector2i& rel,
                                    int button, int modifiers) override;
    virtual bool mouse_drag_event(const Vector2i& p, const Vector2i& rel,
                                  int button, int modifiers) override;
    virtual bool scroll_event(const Vector2i& p, const Vector2f& rel) override;
    virtual bool zoom_event(double magnification, const Vector2i& pos) override;
    virtual void draw(NVGcontext* ctx) override;
    virtual Widget* find_widget(const Vector2i& p) override;
    virtual const Widget* find_widget(const Vector2i& p) const override;

protected:
    /// Effective (scaled) child size in panel-local pixels.
    Vector2d effective_child_size() const;

    /// Returns true if the cursor is over the vertical scroll bar.
    bool over_vbar(const Vector2i& p) const;
    /// Returns true if the cursor is over the horizontal scroll bar.
    bool over_hbar(const Vector2i& p) const;

    /// Clamp m_zoom and m_pan_offset to legal values.
    void clamp_state();

protected:
    Vector2i  m_child_preferred_size;
    Vector2d  m_pan_offset;        ///< In panel-local pixels.
    double    m_zoom;              ///< 1.0 == no zoom.
    double    m_zoom_min;
    double    m_zoom_max;
    double    m_zoom_wheel_step;   ///< Multiplicative factor per wheel tick when zooming.
    bool      m_zoom_enabled;
    bool      m_reflow_on_zoom = true;

    bool      m_scrolling_x;
    bool      m_scrolling_y;
    bool      m_panning;           ///< Middle-mouse pan in progress.
    bool      m_update_layout;
    ScrollTypes m_scroll_type;

    double    m_vel_x  = 0.0;   // inertia scroll velocity (px/s)
    double    m_vel_y  = 0.0;
    double    m_last_t = 0.0;   // glfwGetTime() at last draw
};

NAMESPACE_END(nanogui)
