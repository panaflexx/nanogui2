/*
    nanogui/window.h -- Top-level window widget

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/widget.h>
#include <functional>

NAMESPACE_BEGIN(nanogui)

struct WindowConfig {
    std::string title = "Untitled";
    Vector2i position = Vector2i(0, 0);
    Vector2i size = Vector2i(100, 100);
    bool resizable = true;
    bool traffic_lights = true;
    /**
     * When true, this window is a root content surface: pinned to (0,0), sized
     * to its parent (usually a \ref Screen), no title bar / traffic lights /
     * shadow / drag / resize. \ref Screen::resize_event keeps it in sync.
     * Prefer \ref RootWindow for this pattern.
     */
    bool root = false;
    Layout* layout = nullptr;
};

/**
 * \class Window window.h nanogui/window.h
 *
 * \brief Top-level window widget.
 *
 * Titled windows optionally show macOS-style traffic-light controls in the
 * title bar (close / minimize / maximize). Use \ref set_traffic_lights to
 * enable or disable them (MenuBar disables them automatically).
 *
 * For an app that owns the whole \ref Screen (no floating chrome), use
 * \ref set_root or \ref RootWindow so the window fills the screen and tracks
 * resizes automatically.
 */
class NANOGUI_EXPORT Window : public WidgetCRTP<Window> {
    friend class Popup;
    friend class Screen;
public:
    Window(Widget* parent, const WindowConfig& config);
    Window(Widget* parent, const std::string& title = "Untitled", bool resizable = true);

    /// Return the window title
    const std::string& title() const { return m_title; }
    /// Set the window title
    void set_title(const std::string& title) { m_title = title; }

    /// Is this a model dialog?
    bool modal() const { return m_modal; }
    /// Set whether or not this is a modal dialog
    void set_modal(bool modal) { m_modal = modal; }
    /// Draw size for this window?
    bool draw_shadow() const { return m_draw_shadow; }
    /// Set whether or not to draw shadow for this window
    void set_draw_shadow(bool draw_shadow) { m_draw_shadow = draw_shadow; }
    /// Is this a resizable window?
    bool resizable() const { return m_resizable; }
    /// Set whether or not this window is resizable
    void set_resizable(bool resizable) { m_resizable = resizable; }
    /// Can this window be moved?
    bool can_move() const { return m_can_move; }
    /// Set whether the window can move
    void set_can_move(bool can_move) { m_can_move = can_move; }
    /// Can this window snap to other windows?
    bool can_snap() const { return m_can_snap; }
    /// Set whether the window can snap
    void set_can_snap(bool can_snap) { m_can_snap = can_snap; }

    /**
     * Show macOS-style red / yellow / green title-bar controls.
     * Enabled by default when the window has a non-empty title.
     */
    bool traffic_lights() const { return m_traffic_lights; }
    void set_traffic_lights(bool enabled) { m_traffic_lights = enabled; }

    /**
     * Which traffic-light buttons are shown: bit 0 = close (red),
     * bit 1 = minimize (yellow), bit 2 = maximize (green).
     * Default 0x7 shows all three; use 0x1 for a close-only dialog.
     */
    int traffic_lights_mask() const { return m_traffic_mask; }
    void set_traffic_lights_mask(int mask) { m_traffic_mask = mask; }

    /**
     * Root / full-screen content window: fills the parent \ref Screen, no
     * floating chrome. Geometry is kept in sync on screen resize.
     */
    bool is_root() const { return m_root; }
    void set_root(bool root);

    /// Pin position/size to the parent (no-op unless \ref is_root).
    void sync_root_geometry();

    /// Collapsed to title bar only (yellow traffic light).
    bool minimized() const { return m_minimized; }
    void set_minimized(bool minimized);

    /// Expanded to fill the parent (green traffic light).
    bool maximized() const { return m_maximized; }
    void set_maximized(bool maximized);
    void toggle_maximize();

    /// Optional close hook; if unset, close disposes the window.
    void set_close_callback(const std::function<void()> &cb) { m_close_callback = cb; }
    const std::function<void()> &close_callback() const { return m_close_callback; }

    /// Return the panel used to house window buttons (right side of title bar)
    Widget* button_panel();

    /// Dispose the window
    void dispose();

    /// Center the window in the current \ref Screen
    void center();

    /// Draw the window
    virtual void draw(NVGcontext* ctx) override;
    /// Handle mouse enter/leave events
    virtual bool mouse_enter_event(const Vector2i& p, bool enter) override;
    /// Handle window drag events
    virtual bool mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override;
    /// Handle a mouse motion event (default implementation: propagate to children)
    virtual bool mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override;
    /// Handle mouse events recursively and bring the current window to the top
    virtual bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override;
    /// Accept scroll events and propagate them to the widget under the mouse cursor
    virtual bool scroll_event(const Vector2i& p, const Vector2f& rel) override;
    /// Compute the preferred size of the widget
    virtual Vector2i preferred_size(NVGcontext* ctx) const override;
    /// Invoke the associated layout generator to properly place child widgets, if any
    virtual void perform_layout(NVGcontext* ctx) override;
    /// Content origin is below the title bar (when one is present)
    virtual Vector2i content_offset() const override {
        if (!m_title.empty() && m_theme)
            return Vector2i(0, m_theme->m_window_header_height);
        return Vector2i(0, 0);
    }
    /// Determine the widget located at the given position value (recursive).
    /// Overridden so that the window-resize hot zones always claim the
    /// point, even when child widgets visually overlap them.
    virtual Widget* find_widget(const Vector2i& p) override;
    virtual const Widget* find_widget(const Vector2i& p) const override;
protected:
    /// Internal helper function to maintain nested window position values; overridden in \ref Popup
    virtual void refresh_relative_placement();
    virtual bool check_horizontal_resize(const Vector2i& mousePos);
    virtual bool check_vertical_resize(const Vector2i& mousePos);

    /// Traffic-light geometry / hit-test helpers (local to window).
    bool traffic_lights_active() const;
    /// Index 0=close, 1=minimize, 2=maximize, or -1 if none.
    int traffic_light_at(const Vector2i& p) const;
    void traffic_light_center(int index, float &cx, float &cy) const;
    void draw_traffic_lights(NVGcontext *ctx);
    void save_restore_geometry();
    void apply_maximized_geometry();

protected:
    std::string m_title;
    Widget* m_button_panel = nullptr;
    bool m_modal;
    bool m_drag;
    bool m_resize;
    Vector2i m_resize_dir;
    Vector2i m_min_size;
    Vector2i m_first_size;
    bool m_draw_shadow;
    bool m_resizable;
    bool m_can_move;
    int m_snap_offset;
    bool m_can_snap;
    Vector2i m_snap_tot_rel;
    Vector2i m_snap_init;

    bool m_traffic_lights = true;
    int  m_traffic_mask = 0x7;
    bool m_root = false;
    bool m_minimized = false;
    bool m_maximized = false;
    /// Geometry to restore after un-maximize / un-minimize.
    Vector2i m_restore_pos{0, 0};
    Vector2i m_restore_size{0, 0};
    /// Which traffic light is hovered: -1 none, 0 red, 1 yellow, 2 green.
    int m_traffic_hover = -1;
    /// Title-bar double-click timer for maximize toggle.
    double m_last_title_click = 0.0;
    std::function<void()> m_close_callback;
public:
#if defined(_DEBUG) || !defined(NDEBUG)
    float m_last_drawtime_ms = 0.0f;
#endif
};

/**
 * \class RootWindow window.h nanogui/window.h
 *
 * \brief Window that owns the full \ref Screen client area.
 *
 * Equivalent to a \ref Window with \ref WindowConfig::root set. Use this as
 * the single content surface for document-style apps (menu bar + form, etc.)
 * instead of manually syncing size in \ref Screen::resize_event.
 *
 * Example:
 * \code
 *   RootWindow *root = new RootWindow(screen, new FlexLayout(...));
 *   new MenuBar(root, "");
 *   // ... more content ...
 *   perform_layout();
 * \endcode
 */
class NANOGUI_EXPORT RootWindow : public Window {
public:
    /**
     * \param parent  Typically a \ref Screen
     * \param layout  Optional layout applied immediately
     */
    explicit RootWindow(Widget *parent, Layout *layout = nullptr)
        : Window(parent, WindowConfig{
              .title = "",
              .position = Vector2i(0, 0),
              .size = parent ? parent->size() : Vector2i(100, 100),
              .resizable = false,
              .traffic_lights = false,
              .root = true,
              .layout = layout
          }) {}
};

NAMESPACE_END(nanogui)
