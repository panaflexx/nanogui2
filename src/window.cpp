/*
    src/window.cpp -- Top-level window widget

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/window.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/layout.h>
#include <nanogui/popup.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/messagedialog.h>
#include <nanogui/common.h>
#include <chrono>
#include <algorithm>
#include <cmath>

NAMESPACE_BEGIN(nanogui)

namespace {
// macOS traffic-light geometry (logical pixels)
constexpr float kTlRadius   = 6.0f;
constexpr float kTlGap      = 20.0f;  // center-to-center
constexpr float kTlLeft     = 16.0f;  // first center x from window left
constexpr float kTlHitPad   = 3.0f;   // extra hit radius beyond the disc
} // namespace

Window::Window(Widget* parent, const WindowConfig& config)
    : WidgetCRTP<Window>(parent), m_title(config.title),
      m_button_panel(nullptr), m_modal(false), m_drag(false),
      m_resize(false), m_resize_dir(Vector2i(0, 0)),
      m_min_size(Vector2i(0, 0)), m_first_size(0),
      m_draw_shadow(!config.title.empty() && !config.root),
      m_resizable(config.root ? false : config.resizable),
      m_can_move(!config.title.empty() && !config.root), m_snap_offset(8),
      m_can_snap(!config.title.empty() && !config.root),
      m_traffic_lights(config.traffic_lights && !config.title.empty() && !config.root),
      m_root(config.root)
{
    DebugName = m_parent ? m_parent->DebugName + ",Window" : "Window";
    m_pos = config.position;
    m_size = config.size;
    if (config.layout)
        set_layout(config.layout);
    if (m_root)
        set_root(true); // apply chrome + geometry
}

Window::Window(Widget* parent, const std::string& title, bool resizable)
    : WidgetCRTP<Window>(parent), m_title(title), m_button_panel(nullptr), m_modal(false), m_drag(false),
      m_resize(false), m_resize_dir(Vector2i(0, 0)), m_min_size(Vector2i(0, 0)), m_first_size(0),
      m_draw_shadow(!title.empty()), m_resizable(resizable),
      m_can_move(!title.empty()), m_snap_offset(8), m_can_snap(!title.empty()),
      m_traffic_lights(!title.empty()), m_root(false)
{
    DebugName = m_parent->DebugName + ",Window";
}

void Window::set_root(bool root) {
    m_root = root;
    if (!root)
        return;
    // Full-bleed content surface: no floating chrome.
    m_title.clear();
    m_draw_shadow = false;
    m_resizable = false;
    m_can_move = false;
    m_can_snap = false;
    m_traffic_lights = false;
    m_modal = false;
    m_minimized = false;
    m_maximized = false;
    m_drag = false;
    m_resize = false;
    sync_root_geometry();
}

void Window::sync_root_geometry() {
    if (!m_root || !m_parent)
        return;
    m_pos = Vector2i(0, 0);
    m_size = m_parent->size();
}

bool Window::traffic_lights_active() const {
    return m_traffic_lights && !m_title.empty() && m_theme &&
           m_theme->m_window_header_height > 0;
}

void Window::traffic_light_center(int index, float &cx, float &cy) const {
    int hh = m_theme ? m_theme->m_window_header_height : 36;
    cx = m_pos.x() + kTlLeft + index * kTlGap;
    cy = m_pos.y() + hh * 0.5f;
}

int Window::traffic_light_at(const Vector2i& p) const {
    if (!traffic_lights_active())
        return -1;
    for (int i = 0; i < 3; ++i) {
        if (!(m_traffic_mask & (1 << i)))
            continue;
        float cx, cy;
        traffic_light_center(i, cx, cy);
        float dx = p.x() - cx, dy = p.y() - cy;
        if (dx * dx + dy * dy <= (kTlRadius + kTlHitPad) * (kTlRadius + kTlHitPad))
            return i;
    }
    return -1;
}

void Window::save_restore_geometry() {
    if (!m_maximized && !m_minimized) {
        m_restore_pos = m_pos;
        m_restore_size = m_size;
    }
}

void Window::apply_maximized_geometry() {
    Widget *p = parent();
    if (!p)
        return;
    // Fill parent with a tiny margin so glass shadow remains visible
    constexpr int margin = 6;
    int x = margin, y = margin;
    int w = std::max(40, p->width() - 2 * margin);
    int h = std::max(40, p->height() - 2 * margin);

    // If a MenuBar strip is present at the top of the screen, sit below it
    if (auto *scr = dynamic_cast<Screen *>(p)) {
        for (Widget *child : scr->children()) {
            // MenuBar is a Window with header height 0 and top-left strip geometry
            Window *win = dynamic_cast<Window *>(child);
            if (!win || win == this || !win->visible())
                continue;
            if (win->position() == Vector2i(0, 0) &&
                win->width() >= scr->width() - 2 &&
                win->height() > 0 && win->height() < 64 &&
                !win->can_move()) {
                y = win->height() + 2;
                h = std::max(40, scr->height() - y - margin);
                break;
            }
        }
    }
    m_pos = Vector2i(x, y);
    m_size = Vector2i(w, h);
}

void Window::set_minimized(bool minimized) {
    if (m_minimized == minimized)
        return;
    if (minimized) {
        save_restore_geometry();
        m_minimized = true;
        m_maximized = false;
        int hh = m_theme ? m_theme->m_window_header_height : 36;
        m_size.y() = hh;
        // Hide content children (keep button_panel for layout bookkeeping)
        for (Widget *child : m_children) {
            if (child != m_button_panel)
                child->set_visible(false);
        }
    } else {
        m_minimized = false;
        if (m_restore_size.x() > 0 && m_restore_size.y() > 0) {
            m_pos = m_restore_pos;
            m_size = m_restore_size;
        }
        for (Widget *child : m_children) {
            if (child != m_button_panel)
                child->set_visible(true);
        }
    }
    if (Screen *scr = screen()) {
        perform_layout(scr->nvg_context());
        scr->redraw();
    }
}

void Window::set_maximized(bool maximized) {
    if (m_maximized == maximized && !m_minimized)
        return;
    // Coming out of minimize always restores content visibility
    if (m_minimized) {
        m_minimized = false;
        for (Widget *child : m_children) {
            if (child != m_button_panel)
                child->set_visible(true);
        }
    }
    if (maximized) {
        save_restore_geometry();
        m_maximized = true;
        apply_maximized_geometry();
    } else {
        m_maximized = false;
        if (m_restore_size.x() > 0 && m_restore_size.y() > 0) {
            m_pos = m_restore_pos;
            m_size = m_restore_size;
        }
    }
    if (Screen *scr = screen()) {
        perform_layout(scr->nvg_context());
        scr->redraw();
    }
}

void Window::toggle_maximize() {
    set_maximized(!m_maximized);
}

void Window::draw_traffic_lights(NVGcontext *ctx) {
    if (!traffic_lights_active())
        return;

    // Classic macOS palette
    const NVGcolor colors[3] = {
        nvgRGB(255, 95, 87),   // close
        nvgRGB(255, 189, 46),  // minimize
        nvgRGB(40, 200, 64),   // maximize
    };
    const NVGcolor borders[3] = {
        nvgRGB(224, 70, 62),
        nvgRGB(224, 160, 35),
        nvgRGB(30, 170, 50),
    };
    // Dim slightly when window is unfocused (macOS does this)
    float dim = m_focused || m_mouse_focus ? 1.f : 0.55f;
    bool show_glyphs = (m_traffic_hover >= 0) || m_focused;

    for (int i = 0; i < 3; ++i) {
        if (!(m_traffic_mask & (1 << i)))
            continue;
        float cx, cy;
        traffic_light_center(i, cx, cy);
        bool hover = (m_traffic_hover == i);
        float r = kTlRadius + (hover ? 0.5f : 0.f);

        // Soft contact shadow under each disc
        nvgBeginPath(ctx);
        nvgCircle(ctx, cx, cy + 0.6f, r);
        nvgFillColor(ctx, nvgRGBA(0, 0, 0, (int)(40 * dim)));
        nvgFill(ctx);

        // Filled disc
        NVGcolor fill = colors[i];
        fill.r *= dim; fill.g *= dim; fill.b *= dim;
        nvgBeginPath(ctx);
        nvgCircle(ctx, cx, cy, r);
        nvgFillColor(ctx, fill);
        nvgFill(ctx);

        // Specular highlight
        NVGpaint wash = nvgLinearGradient(ctx, cx, cy - r, cx, cy,
            nvgRGBA(255, 255, 255, (int)(110 * dim)), nvgRGBA(255, 255, 255, 0));
        nvgBeginPath(ctx);
        nvgCircle(ctx, cx, cy - r * 0.15f, r * 0.85f);
        nvgFillPaint(ctx, wash);
        nvgFill(ctx);

        // Rim
        nvgBeginPath(ctx);
        nvgCircle(ctx, cx, cy, r - 0.5f);
        nvgStrokeWidth(ctx, 0.75f);
        NVGcolor rim = borders[i];
        rim.r *= dim; rim.g *= dim; rim.b *= dim;
        nvgStrokeColor(ctx, rim);
        nvgStroke(ctx);

        // Glyphs on hover / focus (× − + / restore)
        if (show_glyphs) {
            nvgStrokeWidth(ctx, 1.35f);
            nvgStrokeColor(ctx, nvgRGBA(60, 30, 20, (int)(180 * dim)));
            nvgFillColor(ctx, nvgRGBA(60, 30, 20, (int)(180 * dim)));
            float s = 2.6f;
            if (i == 0) {
                // × close
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, cx - s, cy - s);
                nvgLineTo(ctx, cx + s, cy + s);
                nvgMoveTo(ctx, cx + s, cy - s);
                nvgLineTo(ctx, cx - s, cy + s);
                nvgStroke(ctx);
            } else if (i == 1) {
                // − minimize
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, cx - s - 0.5f, cy);
                nvgLineTo(ctx, cx + s + 0.5f, cy);
                nvgStroke(ctx);
            } else {
                // + maximize, or restore-box when maximized
                if (m_maximized) {
                    float a = 2.4f;
                    nvgBeginPath(ctx);
                    nvgRect(ctx, cx - a, cy - a + 1.2f, a * 1.4f, a * 1.4f);
                    nvgStroke(ctx);
                    nvgBeginPath(ctx);
                    nvgMoveTo(ctx, cx - a + 1.6f, cy - a + 1.2f);
                    nvgLineTo(ctx, cx - a + 1.6f, cy - a);
                    nvgLineTo(ctx, cx + a + 0.2f, cy - a);
                    nvgLineTo(ctx, cx + a + 0.2f, cy + a - 1.6f);
                    nvgLineTo(ctx, cx + a - 1.4f, cy + a - 1.6f);
                    nvgStroke(ctx);
                } else {
                    nvgBeginPath(ctx);
                    nvgMoveTo(ctx, cx - s, cy);
                    nvgLineTo(ctx, cx + s, cy);
                    nvgMoveTo(ctx, cx, cy - s);
                    nvgLineTo(ctx, cx, cy + s);
                    nvgStroke(ctx);
                }
            }
        }
    }
}

Vector2i Window::preferred_size(NVGcontext* ctx) const {
    if (m_root && m_parent)
        return m_parent->size();

    if (!m_resizable || m_size == 0)// calculate prefered size only if not resizable. else keep curr size
    {
        if (m_button_panel)
            m_button_panel->set_visible(false);
        Vector2i result = Widget::preferred_size(ctx);
        if (m_button_panel)
            m_button_panel->set_visible(true);

        nvgFontSize(ctx, 18.0f);
        nvgFontFace(ctx, "sans-bold");
        float bounds[4];
        nvgTextBounds(ctx, 0, 0, m_title.c_str(), nullptr, bounds);
        return Vector2i(
            std::max(result.x(), (int)(bounds[2] - bounds[0] + 20)),
            std::max(result.y(), (int)(bounds[3] - bounds[1]))
        );
    }
    else return m_size;
}

Widget* Window::button_panel() {

    if (!m_button_panel) {
        m_button_panel = new Widget(this);
        m_button_panel->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 0));
    }
    return m_button_panel;
}

void Window::perform_layout(NVGcontext* ctx) {
    if (m_root)
        sync_root_geometry();

    if (!m_button_panel) {

        if (m_children.size() == 1)
        {
            ScrollPanel* CanICastScrollPanel = dynamic_cast<ScrollPanel*>(m_children[0]);
            if (CanICastScrollPanel != NULL)
            {
                int TempX = m_size.x() - 10;
                int  TempY = m_size.y() - 10 - (!m_title.empty() ? m_theme->m_window_header_height : 0);
                if (TempX < 0)TempX = 0;
                if (TempY < 0)TempY = 0;
                CanICastScrollPanel->set_fixed_size(Vector2i(TempX, TempY));
            }
        }
        Widget::perform_layout(ctx);
    }
    else
    {
        m_button_panel->set_visible(false);
        Widget::perform_layout(ctx);
        for (auto w : m_button_panel->children()) {
            w->set_fixed_size(Vector2i(22, 22));
            w->set_font_size(15);
        }
        m_button_panel->set_visible(true);
        m_button_panel->set_size(Vector2i(width(), 22));
        m_button_panel->set_position(Vector2i(
            width() - (m_button_panel->preferred_size(ctx).x() - 2), 4));
        m_button_panel->perform_layout(ctx);
    }
    // Root windows track the Screen; no interactive min-size floor.
    if (m_root) {
        m_min_size = Vector2i(0, 0);
        return;
    }

    //// Calculate the minimum size that the window can resize to.
    // Do NOT use preferred_size() here for resizable windows — it returns the
    // current m_size and would ratchet the floor after every grow, preventing
    // shrink ("Selected image" + ImageView). Query the layout/content floor.
    if (m_first_size == Vector2i(0, 0))
        m_first_size = m_size;

    Vector2i content_min(40, 40);
    if (m_layout) {
        if (m_button_panel)
            m_button_panel->set_visible(false);
        content_min = Widget::preferred_size(ctx);
        if (m_button_panel)
            m_button_panel->set_visible(true);
        content_min.x() = std::max(40, content_min.x());
        content_min.y() = std::max(40, content_min.y());
    }
    m_min_size = content_min;

    // Scrollable content can shrink further; only enforce a small floor.
    if (m_children.size() == 1) {
        if (auto* sp = dynamic_cast<ScrollPanel*>(m_children[0])) {
            if (sp->VScrollable())
                m_min_size.y() = 40;
            if (sp->HScrollable())
                m_min_size.x() = 40;
        }
    }
}

void Window::draw(NVGcontext* ctx) {
    /* Root window: full-bleed panel, no floating chrome (shadow/radius/title). */
    if (m_root) {
        if (m_parent && (m_pos != Vector2i(0, 0) || m_size != m_parent->size()))
            sync_root_geometry();

        float fx = (float)m_pos.x(), fy = (float)m_pos.y();
        float fw = (float)m_size.x(), fh = (float)m_size.y();
        nvgBeginPath(ctx);
        nvgRect(ctx, fx, fy, fw, fh);
        nvgFillColor(ctx, m_theme->m_window_fill_focused);
        nvgFill(ctx);

#if defined(_DEBUG) || !defined(NDEBUG)
        auto t0 = std::chrono::high_resolution_clock::now();
#endif
        Widget::draw(ctx);
#if defined(_DEBUG) || !defined(NDEBUG)
        auto t1 = std::chrono::high_resolution_clock::now();
        m_last_drawtime_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
#endif
        return;
    }

    int ds = m_theme->m_window_drop_shadow_size, cr = m_theme->m_window_corner_radius;
    int hh = m_theme->m_window_header_height;
    float fx = (float)m_pos.x(), fy = (float)m_pos.y();
    float fw = (float)m_size.x(), fh = (float)m_size.y();

    nvgSave(ctx);

    /* Soft layered glass shadow */
    if (m_draw_shadow) {
        nvgSave(ctx);
        nvgResetScissor(ctx);
        m_theme->draw_glass_shadow(ctx, fx, fy, fw, fh, (float)cr, (float)ds);
        nvgRestore(ctx);
    }

    /* Frosted glass body */
    Color body = m_mouse_focus ? m_theme->m_window_fill_focused
                               : m_theme->m_window_fill_unfocused;
    m_theme->draw_glass_surface(ctx, fx, fy, fw, fh, (float)cr, body,
                                /*specular=*/true, /*border=*/true);

    if (!m_title.empty()) {
        /* Integrated glass title bar — soft header wash, hairline separator */
        nvgSave(ctx);
        nvgIntersectScissor(ctx, fx, fy, fw, (float)hh);
        NVGpaint header_paint = nvgLinearGradient(
            ctx, fx, fy, fx, fy + (float)hh,
            m_theme->m_window_header_gradient_top,
            m_theme->m_window_header_gradient_bot);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, fx, fy, fw, (float)hh + (float)cr, (float)cr);
        nvgFillPaint(ctx, header_paint);
        nvgFill(ctx);

        /* Top specular rim */
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, fx + (float)cr, fy + 0.75f);
        nvgLineTo(ctx, fx + fw - (float)cr, fy + 0.75f);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, m_theme->m_window_header_sep_top);
        nvgStroke(ctx);
        nvgRestore(ctx);

        /* Soft header divider (no hard bevel) */
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, fx + 10.f, fy + (float)hh - 0.5f);
        nvgLineTo(ctx, fx + fw - 10.f, fy + (float)hh - 0.5f);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, m_theme->m_window_header_sep_bot);
        nvgStroke(ctx);

        nvgFontSize(ctx, 14.0f);
        nvgFontFace(ctx, "sans-bold");
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontBlur(ctx, 0);
        nvgFillColor(ctx, m_focused ? m_theme->m_window_title_focused
                                    : m_theme->m_window_title_unfocused);
        nvgText(ctx, fx + fw * 0.5f, fy + hh * 0.5f, m_title.c_str(), nullptr);

        /* macOS traffic lights (left side of title bar) */
        draw_traffic_lights(ctx);
    }

    nvgRestore(ctx);

#if defined(_DEBUG) || !defined(NDEBUG)
    auto t0 = std::chrono::high_resolution_clock::now();
#endif
    Widget::draw(ctx);
#if defined(_DEBUG) || !defined(NDEBUG)
    auto t1 = std::chrono::high_resolution_clock::now();
    m_last_drawtime_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
#endif

    /* Subtle macOS-style resize corner marks (hidden while minimized) */
    if (m_resizable && !m_minimized) {
        nvgSave(ctx);
        nvgResetScissor(ctx);
        float rx = fx + fw - 3.f;
        float ry = fy + fh - 3.f;
        nvgStrokeWidth(ctx, 1.25f);
        nvgStrokeColor(ctx, m_theme->m_border_medium);
        for (int i = 0; i < 3; ++i) {
            float o = 3.f + i * 3.5f;
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, rx - o, ry);
            nvgLineTo(ctx, rx, ry - o);
            nvgStroke(ctx);
        }
        nvgRestore(ctx);
    }
}

void Window::dispose() {
    Widget* widget = this;
    while (widget->parent())
        widget = widget->parent();
    ((Screen*)widget)->dispose_window(this);
}

void Window::center() {
    Widget* widget = this;
    while (widget->parent())
        widget = widget->parent();
    ((Screen*)widget)->center_window(this);
}

bool Window::mouse_enter_event(const Vector2i& p, bool enter) {
    Widget::mouse_enter_event(p, enter);
    if (!enter && m_traffic_hover != -1) {
        m_traffic_hover = -1;
        if (Screen *scr = screen())
            scr->redraw();
    }
    return true;
}

bool Window::mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int  modifiers) {
    if (m_can_move && m_drag && (button & (1 << GLFW_MOUSE_BUTTON_1)) != 0) {
        if (m_can_snap)
        {
            int MinLR = INT_MAX;
            int MinRL = INT_MAX;
            int MinTB = INT_MAX;
            int MinBT = INT_MAX;

            m_snap_tot_rel += rel;
            Vector2i temp_position = m_snap_init + m_snap_tot_rel;
            int Top = temp_position.y();
            int Bottom = temp_position.y() + size().y();
            int Left = temp_position.x();
            int Right = temp_position.x() + size().x();
            for (Widget* ChildWindow : screen()->children())
            {
                // Make sure the child ius a true window and not a hidden popup or a message box (derived classes)
                Window* CanICastWindow = dynamic_cast<Window*>(ChildWindow);
                Popup* CanICastPopup = dynamic_cast<Popup*>(ChildWindow);
                MessageDialog* CanICastDialog = dynamic_cast<MessageDialog*>(ChildWindow);
                bool IsWindow = (CanICastWindow != NULL && CanICastPopup == NULL && CanICastDialog == NULL);
                if (ChildWindow == this || !IsWindow || !ChildWindow->visible())continue;// continue if the window is itself or the widget is not a window
                int child_Top = ChildWindow->position().y();
                int child_Bottom = ChildWindow->position().y() + ChildWindow->size().y();
                int child_Left = ChildWindow->position().x();
                int child_Right = ChildWindow->position().x() + ChildWindow->size().x();
                bool CheckX =
                    ((Bottom >= child_Top) && (Bottom <= child_Bottom))
                    || ((Top >= child_Top) && (Top <= child_Bottom))
                    || ((Top <= child_Top) && (Bottom >= child_Bottom));
                bool CheckY =
                    ((Right >= child_Left) && (Right <= child_Right))
                    || ((Left >= child_Left) && (Left <= child_Right))
                    || ((Left <= child_Left) && (Right >= child_Right));
                if (CheckX)
                {
                    int LR = abs(Left - child_Right);
                    int RL = abs(Right - child_Left);
                    if (LR < MinLR && LR < m_snap_offset) { MinLR = LR;  temp_position.x() = child_Right; }
                    if (RL < MinRL && RL < m_snap_offset) { MinRL = RL; temp_position.x() = child_Left - size().x(); }
                }
                if (CheckY)
                {
                    int TB = abs(Top - child_Bottom);
                    int BT = abs(Bottom - child_Top);
                    if (TB < MinTB && TB < m_snap_offset) { MinTB = TB; temp_position.y() = child_Bottom; }
                    if (BT < MinBT && BT < m_snap_offset) { MinBT = BT; temp_position.y() = child_Top - size().y(); }
                }
            }
            m_pos = temp_position;
        }
        else
            m_pos += rel;
        m_pos = max(m_pos, Vector2i(0));
		// If screen size, let the move extend past the edges
		if(parent() == dynamic_cast<Widget*>(this->screen())) {
			m_pos = min(m_pos, parent()->size() - Vector2i(25));
		} else {
			m_pos = min(m_pos, parent()->size() - m_size);
		}
        return true;
    }
    else if (m_resizable && m_resize && (button & (1 << GLFW_MOUSE_BUTTON_1)) != 0) {
        const Vector2i& lowerRightCorner = m_pos + m_size;
        //const Vector2i& upperLeftCorner = m_pos;
        NVGcontext* ctx = static_cast<Screen*>(parent())->nvg_context();
        bool resized = false;


        if (m_resize_dir.x() == 1) {
            if ((rel.x() > 0 && p.x() >= lowerRightCorner.x()) || (rel.x() < 0)) {
                m_size.x() += rel.x();
                m_snap_tot_rel.x() += rel.x();
                resized = true;
            }
        }

        if (m_resize_dir.y() == 1) {
            if ((rel.y() > 0 && p.y() >= lowerRightCorner.y()) || (rel.y() < 0)) {
                m_size.y() += rel.y();
                m_snap_tot_rel.y() += rel.y();
                resized = true;
            }
        }

        if (m_can_snap)
        {
            int MinRL = INT_MAX;
            int MinBT = INT_MAX;

            if (m_snap_init.x() + m_snap_tot_rel.x() <= m_min_size.x())
                m_snap_tot_rel.x() = m_min_size.x() - m_snap_init.x();
            if (m_snap_init.y() + m_snap_tot_rel.y() <= m_min_size.y())
                m_snap_tot_rel.y() = m_min_size.y() - m_snap_init.y();
            Vector2i temp_size = m_snap_init + m_snap_tot_rel;
            int Top = position().y();
            int Bottom = position().y() + temp_size.y();
            int Left = position().x();
            int Right = position().x() + temp_size.x();
            for (Widget* ChildWindow : screen()->children())
            {
                // Make sure the child is a true window and not a hidden popup or a message box (derived classes)
                Window* CanICastWindow = dynamic_cast<Window*>(ChildWindow);
                Popup* CanICastPopup = dynamic_cast<Popup*>(ChildWindow);
                MessageDialog* CanICastDialog = dynamic_cast<MessageDialog*>(ChildWindow);
                bool IsWindow = (CanICastWindow != NULL && CanICastPopup == NULL && CanICastDialog == NULL);
                if (ChildWindow == this || !IsWindow || !ChildWindow->visible())continue;// continue if the window is itself or the widget is not a window
                int child_Top = ChildWindow->position().y();
                int child_Bottom = ChildWindow->position().y() + ChildWindow->size().y();
                int child_Left = ChildWindow->position().x();
                int child_Right = ChildWindow->position().x() + ChildWindow->size().x();
                bool CheckX =
                    ((Bottom >= child_Top) && (Bottom <= child_Bottom))
                    || ((Top >= child_Top) && (Top <= child_Bottom))
                    || ((Top <= child_Top) && (Bottom >= child_Bottom));
                bool CheckY =
                    ((Right >= child_Left) && (Right <= child_Right))
                    || ((Left >= child_Left) && (Left <= child_Right))
                    || ((Left <= child_Left) && (Right >= child_Right));
                if (CheckX)
                {
                    int RL = abs(Right - child_Left);
                    if (RL < MinRL && RL < m_snap_offset) {
                        MinRL = RL;
                        m_size.x() = child_Left - Left;
                    }
                }
                if (CheckY)
                {
                    int BT = abs(Bottom - child_Top);
                    if (BT < MinBT && BT < m_snap_offset) {
                        MinBT = BT;
                        m_size.y() = child_Top - Top;
                    }
                }

            }
            {// Do a final check for the screen boarders
                int RL = abs(Right - screen()->size().x());
                if (RL < MinRL && RL < m_snap_offset) {
                    MinRL = RL;
                    m_size.x() = screen()->size().x() - Left;
                }
                int BT = abs(Bottom - screen()->size().y());
                if (BT < MinBT && BT < m_snap_offset) {
                    MinBT = BT;
                    m_size.y() = screen()->size().y() - Top;
                }

            }
            if (MinRL == INT_MAX)
                m_size.x() = temp_size.x();
            if (MinBT == INT_MAX)
                m_size.y() = temp_size.y();
        }

        // Clamp to min size (only enforce non-zero components)
        if (m_min_size.x() > 0) m_size.x() = std::max(m_size.x(), m_min_size.x());
        if (m_min_size.y() > 0) m_size.y() = std::max(m_size.y(), m_min_size.y());
        // Clamp to max size (only enforce non-zero components)
        if (m_max_size.x() > 0) m_size.x() = std::min(m_size.x(), m_max_size.x());
        if (m_max_size.y() > 0) m_size.y() = std::min(m_size.y(), m_max_size.y());

        if (resized)
            perform_layout(ctx);
        return true;
    }

    if (Widget::mouse_drag_event(p, rel, button, modifiers))
        return true;

    return false;
}

bool Window::mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    int tl = traffic_light_at(p);
    if (tl != m_traffic_hover) {
        m_traffic_hover = tl;
        if (Screen *scr = screen())
            scr->redraw();
    }
    if (tl >= 0) {
        m_cursor = Cursor::Hand;
        return true;
    }

    if (m_resizable && !m_minimized && check_horizontal_resize(p) && check_vertical_resize(p))
        m_cursor = Cursor::HVResize;
    else if (m_resizable && !m_minimized && check_horizontal_resize(p))
        m_cursor = Cursor::HResize;
    else if (m_resizable && !m_minimized && check_vertical_resize(p))
        m_cursor = Cursor::VResize;
    else {
        m_cursor = Cursor::Arrow;

        // Only forward to children if we're not over a resize zone.
        if (m_resizable && !m_minimized &&
            (check_horizontal_resize(p) || check_vertical_resize(p)))
            return true;

        return Widget::mouse_motion_event(p, rel, button, modifiers);
    }
    return true;
}

bool Window::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    // Traffic lights take precedence over drag / children
    if (button == GLFW_MOUSE_BUTTON_1 && traffic_lights_active()) {
        int tl = traffic_light_at(p);
        if (tl >= 0) {
            if (down) {
                request_focus();
                return true;
            }
            // Activate on release while still over the same button
            if (traffic_light_at(p) == tl) {
                if (tl == 0) {
                    // Close — defer dispose so the current event stack unwinds
                    if (m_close_callback) {
                        m_close_callback();
                    } else {
                        Window *win = this;
                        async([win] {
                            if (win && win->parent())
                                win->dispose();
                        });
                    }
                } else if (tl == 1) {
                    // Minimize / restore
                    set_minimized(!m_minimized);
                } else {
                    // Maximize / restore (also un-minimizes)
                    if (m_minimized)
                        set_minimized(false);
                    toggle_maximize();
                }
            }
            return true;
        }
    }

    // Double-click title bar (not on traffic lights) toggles maximize — macOS-ish
    if (button == GLFW_MOUSE_BUTTON_1 && down && !m_title.empty() && m_theme &&
        (p.y() - m_pos.y()) < m_theme->m_window_header_height &&
        traffic_light_at(p) < 0) {
        double now = glfwGetTime();
        if (m_last_title_click > 0.0 && (now - m_last_title_click) < 0.35) {
            m_last_title_click = 0.0;
            if (m_minimized)
                set_minimized(false);
            else
                toggle_maximize();
            return true;
        }
        m_last_title_click = now;
    }

    // Give resize precedence over child widgets.
    if (m_resizable && !m_minimized && down && button == GLFW_MOUSE_BUTTON_1) {
        if (check_horizontal_resize(p) || check_vertical_resize(p)) {
            m_resize_dir.x() = check_horizontal_resize(p) ? 1 : 0;
            m_resize_dir.y() = check_vertical_resize(p) ? 1 : 0;
            m_resize = true;
            m_snap_init = size();
            m_snap_tot_rel = Vector2f(0, 0);
            request_focus();
            return true;
        }
    }

    if (Widget::mouse_button_event(p, button, down, modifiers))
        return true;

    if (button == GLFW_MOUSE_BUTTON_1) {
        m_drag = down && m_can_move && !m_title.empty() &&
                 (p.y() - m_pos.y()) < m_theme->m_window_header_height &&
                 traffic_light_at(p) < 0;
        if (down)
            request_focus();
        if (m_drag) {
            // Dragging a maximized window demotes it back to floating
            if (m_maximized) {
                m_maximized = false;
                // Keep current width/height ratio feel: restore size but pin under cursor
                if (m_restore_size.x() > 0) {
                    float relx = m_size.x() > 0
                        ? (p.x() - m_pos.x()) / (float)m_size.x() : 0.5f;
                    m_size = m_restore_size;
                    m_pos.x() = p.x() - (int)(relx * m_size.x());
                    m_pos.y() = m_pos.y();
                }
            }
            m_snap_init = position();
            m_snap_tot_rel = Vector2f(0, 0);
            return true;
        } else if (m_resizable && !m_minimized && down) {
            m_resize_dir.x() = check_horizontal_resize(p) ? 1 : 0;
            m_resize_dir.y() = check_vertical_resize(p) ? 1 : 0;
            m_resize = m_resize_dir.x() != 0 || m_resize_dir.y() != 0;
            m_snap_init = size();
            m_snap_tot_rel = Vector2f(0, 0);
            if (m_resize)
                return true;
        }
    }

    return false;
}

bool Window::scroll_event(const Vector2i& p, const Vector2f& rel) {
    Widget::scroll_event(p, rel);
    return true;
}

void Window::refresh_relative_placement() {
    /* Overridden in \ref Popup */
}

Widget* Window::find_widget(const Vector2i& p) {
    /* `p` is expressed in the parent's coordinate frame; convert to the
       absolute (screen) frame that check_horizontal_resize /
       check_vertical_resize expect. */
    Vector2i abs_p = m_parent ? m_parent->absolute_position() + p : p;
    // Traffic lights claim the hit so title-bar children don't steal clicks
    // p is in the parent coordinate frame (same as m_pos)
    if (visible() && contains(p) && traffic_light_at(p) >= 0)
        return this;
    if (m_resizable && !m_minimized && visible() && contains(p) &&
        (check_horizontal_resize(abs_p) || check_vertical_resize(abs_p)))
        return this;
    return Widget::find_widget(p);
}

const Widget* Window::find_widget(const Vector2i& p) const {
    Vector2i abs_p = m_parent ? m_parent->absolute_position() + p : p;
    /* check_horizontal_resize / check_vertical_resize are non-const helpers,
       so route through the non-const overload for the resize-zone test. */
    Window* self = const_cast<Window*>(this);
    if (visible() && contains(p) && self->traffic_light_at(p) >= 0)
        return this;
    if (m_resizable && !m_minimized && visible() && contains(p) &&
        (self->check_horizontal_resize(abs_p) ||
         self->check_vertical_resize(abs_p)))
        return this;
    return Widget::find_widget(p);
}
bool Window::check_horizontal_resize(const Vector2i& mousePos) {
    int offset = m_theme->m_resize_area_offset;
    Vector2i lowerRightCorner = absolute_position() + size();
    int headerLowerLeftCornerY = absolute_position().y() + m_theme->m_window_header_height;

    if (mousePos.y() > headerLowerLeftCornerY &&
        mousePos.x() >= lowerRightCorner.x() - offset && mousePos.x() <= lowerRightCorner.x()) {
        return true;
    }

    return false;
}

bool Window::check_vertical_resize(const Vector2i& mousePos) {
    int offset = m_theme->m_resize_area_offset;
    Vector2i lowerRightCorner = absolute_position() + size();

    // Do not check for resize area on top of the window. It is to prevent conflict drag and resize event.
    if (mousePos.y() >= lowerRightCorner.y() - offset && mousePos.y() <= lowerRightCorner.y()) {
        return true;
    }

    return false;
}
NAMESPACE_END(nanogui)
