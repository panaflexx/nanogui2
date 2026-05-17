/*
    src/zoomscrollpanel.cpp -- A scroll panel that also supports smooth
    zooming (double precision) of its child. Adapted from
    src/scrollpanel.cpp.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/zoomscrollpanel.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <algorithm>
#include <cmath>

NAMESPACE_BEGIN(nanogui)

namespace {
    constexpr int kScrollbarMargin = 4;  // outer margin
    constexpr int kScrollbarThick  = 8;  // bar thickness
    constexpr int kScrollbarSlot   = 12; // total slot (thickness + margin)
}

/* ------------------------------------------------------------------ */
/*  Construction                                                       */
/* ------------------------------------------------------------------ */

ZoomScrollPanel::ZoomScrollPanel(Widget* parent)
    : ZoomScrollPanel(parent, ScrollTypes::Both) { }

ZoomScrollPanel::ZoomScrollPanel(Widget* parent, ScrollTypes scroll_type)
    : Widget(parent),
      m_child_preferred_size(0, 0),
      m_pan_offset(0.0, 0.0),
      m_zoom(1.0),
      m_zoom_min(0.05),
      m_zoom_max(64.0),
      m_zoom_wheel_step(1.1),
      m_zoom_enabled(true),
      m_scrolling_x(false),
      m_scrolling_y(false),
      m_panning(false),
      m_update_layout(false),
      m_scroll_type(scroll_type)
{
    DebugName = m_parent ? (m_parent->DebugName + ",ZmScrlPnl") : std::string("ZmScrlPnl");
}

/* ------------------------------------------------------------------ */
/*  Geometry helpers                                                   */
/* ------------------------------------------------------------------ */

ZoomScrollPanel::Vector2d ZoomScrollPanel::effective_child_size() const {
    return Vector2d(m_child_preferred_size.x() * m_zoom,
                    m_child_preferred_size.y() * m_zoom);
}

bool ZoomScrollPanel::over_vbar(const Vector2i& p) const {
    Vector2d e = effective_child_size();
    if (!VScrollable() || e.y() <= m_size.y())
        return false;
    return p.x() > m_pos.x() + m_size.x() - (kScrollbarSlot + 1) &&
           p.x() < m_pos.x() + m_size.x() - kScrollbarMargin;
}

bool ZoomScrollPanel::over_hbar(const Vector2i& p) const {
    Vector2d e = effective_child_size();
    if (!HScrollable() || e.x() <= m_size.x())
        return false;
    return p.y() > m_pos.y() + m_size.y() - (kScrollbarSlot + 1) &&
           p.y() < m_pos.y() + m_size.y() - kScrollbarMargin;
}

void ZoomScrollPanel::clamp_state() {
    m_zoom = std::clamp(m_zoom, m_zoom_min, m_zoom_max);
    Vector2d e = effective_child_size();

    // X clamp
    if (e.x() <= m_size.x()) {
        // Center horizontally if the child is smaller than the viewport.
        m_pan_offset.x() = (m_size.x() - e.x()) * 0.5;
    } else {
        double lo = (double)m_size.x() - e.x();   // most negative
        double hi = 0.0;
        m_pan_offset.x() = std::clamp(m_pan_offset.x(), lo, hi);
    }
    // Y clamp
    if (e.y() <= m_size.y()) {
        m_pan_offset.y() = (m_size.y() - e.y()) * 0.5;
    } else {
        double lo = (double)m_size.y() - e.y();
        double hi = 0.0;
        m_pan_offset.y() = std::clamp(m_pan_offset.y(), lo, hi);
    }
}

/* ------------------------------------------------------------------ */
/*  Coord conversion helpers                                           */
/* ------------------------------------------------------------------ */

Vector2i ZoomScrollPanel::to_child(const Vector2i& parent_pt) const {
    // parent_pt is in our parent's coord space (same space we receive
    // events in). Convert to child logical space.
    double lx = parent_pt.x() - m_pos.x();
    double ly = parent_pt.y() - m_pos.y();
    double cx = (lx - m_pan_offset.x()) / (m_zoom != 0.0 ? m_zoom : 1.0);
    double cy = (ly - m_pan_offset.y()) / (m_zoom != 0.0 ? m_zoom : 1.0);
    return Vector2i((int)std::lround(cx), (int)std::lround(cy));
}

Vector2i ZoomScrollPanel::delta_to_child(const Vector2i& rel) const {
    double inv = (m_zoom != 0.0 ? 1.0 / m_zoom : 1.0);
    return Vector2i((int)std::lround(rel.x() * inv),
                    (int)std::lround(rel.y() * inv));
}

/* ------------------------------------------------------------------ */
/*  Zoom / scroll public API                                           */
/* ------------------------------------------------------------------ */

void ZoomScrollPanel::set_zoom(double z) {
    m_zoom = z;
    clamp_state();
    m_update_layout = true;
}

void ZoomScrollPanel::set_zoom_about(double z, const Vector2i& anchor) {
    // anchor is in panel-local pixels (i.e. relative to m_pos top-left).
    double new_zoom = std::clamp(z, m_zoom_min, m_zoom_max);
    if (m_zoom <= 0.0) {
        m_zoom = new_zoom;
    } else {
        // Keep the point under the anchor fixed:
        // new_pan = anchor - (anchor - old_pan) * (new_zoom / old_zoom)
        double k = new_zoom / m_zoom;
        m_pan_offset.x() = anchor.x() - (anchor.x() - m_pan_offset.x()) * k;
        m_pan_offset.y() = anchor.y() - (anchor.y() - m_pan_offset.y()) * k;
        m_zoom = new_zoom;
    }
    clamp_state();
    m_update_layout = true;
}

void ZoomScrollPanel::reset_view() {
    m_zoom = 1.0;
    m_pan_offset = Vector2d(0.0, 0.0);
    clamp_state();
    m_update_layout = true;
}

Vector2f ZoomScrollPanel::scroll() const {
    Vector2d e = effective_child_size();
    float sx = 0.f, sy = 0.f;
    if (e.x() > m_size.x())
        sx = (float)(-m_pan_offset.x() / (e.x() - m_size.x()));
    if (e.y() > m_size.y())
        sy = (float)(-m_pan_offset.y() / (e.y() - m_size.y()));
    return Vector2f(std::clamp(sx, 0.f, 1.f), std::clamp(sy, 0.f, 1.f));
}

void ZoomScrollPanel::set_scroll(const Vector2f& s) {
    Vector2d e = effective_child_size();
    if (e.x() > m_size.x())
        m_pan_offset.x() = -(double)std::clamp(s.x(), 0.f, 1.f) * (e.x() - m_size.x());
    if (e.y() > m_size.y())
        m_pan_offset.y() = -(double)std::clamp(s.y(), 0.f, 1.f) * (e.y() - m_size.y());
    clamp_state();
    m_update_layout = true;
}

/* ------------------------------------------------------------------ */
/*  Layout                                                             */
/* ------------------------------------------------------------------ */

void ZoomScrollPanel::perform_layout(NVGcontext* ctx) {
    if (m_children.empty())
        return;
    if (m_children.size() > 1)
        throw std::runtime_error("ZoomScrollPanel should have one child.");

    Widget* child = m_children[0];

    // Give the child as much space as it wants. Unlike ScrollPanel, we
    // do NOT constrain the child to the viewport: the panel can also
    // *zoom in*, so the child might want to be larger than the viewport
    // at scale 1.0 OR smaller than it at high zoom. The viewport
    // constraint applies only to drawing/scissoring.
    child->set_position(Vector2i(0, 0));

    // First pass: ask the child its natural preferred size.
    Vector2i pref = child->preferred_size(ctx);
    // Make sure the child has at least the viewport-equivalent (in logical units) available.
    int min_w = (int)std::ceil(m_size.x() / std::max(m_zoom, 1e-9));
    int min_h = (int)std::ceil(m_size.y() / std::max(m_zoom, 1e-9));
    Vector2i child_size(std::max(pref.x(), min_w), std::max(pref.y(), min_h));

    child->set_size(child_size);
    child->perform_layout(ctx);
    m_child_preferred_size = child->preferred_size(ctx);

    clamp_state();
}

Vector2i ZoomScrollPanel::preferred_size(NVGcontext* ctx) const {
    if (m_children.empty())
        return Vector2i(0);
    Vector2i p = m_children[0]->preferred_size(ctx);
    // Reserve a bit of room for scrollbars.
    return p + Vector2i(kScrollbarSlot, kScrollbarSlot);
}

/* ------------------------------------------------------------------ */
/*  Events                                                             */
/* ------------------------------------------------------------------ */

bool ZoomScrollPanel::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (!m_children.empty())
        return m_children[0]->keyboard_event(key, scancode, action, modifiers);
    return false;
}

bool ZoomScrollPanel::mouse_button_event(const Vector2i& p, int button, bool down,
                                         int modifiers) {
    bool on_vbar = over_vbar(p);
    bool on_hbar = over_hbar(p);

    Vector2d e = effective_child_size();

    // ---- Vertical scrollbar interaction ----
    if (down && button == GLFW_MOUSE_BUTTON_1 && !m_children.empty() &&
        VScrollable() && on_vbar && e.y() > m_size.y())
    {
        m_scrolling_y = true;

        double scrollh = m_size.y() * std::min(1.0, m_size.y() / e.y());
        double cur_norm = scroll().y();
        double start = m_pos.y() + kScrollbarMargin + 1 +
                       (m_size.y() - 8 - scrollh) * cur_norm;

        double delta = 0.0;
        if (p.y() < start)
            delta = -m_size.y() / e.y();
        else if (p.y() > start + scrollh)
            delta =  m_size.y() / e.y();

        Vector2f s = scroll();
        s.y() = std::clamp(s.y() + (float)(delta * 0.98), 0.f, 1.f);
        set_scroll(s);
        return true;
    }
    else if (!down) {
        m_scrolling_y = false;
    }

    // ---- Horizontal scrollbar interaction ----
    if (down && button == GLFW_MOUSE_BUTTON_1 && !m_children.empty() &&
        HScrollable() && on_hbar && e.x() > m_size.x())
    {
        m_scrolling_x = true;

        double scrollw = m_size.x() * std::min(1.0, m_size.x() / e.x());
        double cur_norm = scroll().x();
        double start = m_pos.x() + kScrollbarMargin + 1 +
                       (m_size.x() - 8 - scrollw) * cur_norm;

        double delta = 0.0;
        if (p.x() < start)
            delta = -m_size.x() / e.x();
        else if (p.x() > start + scrollw)
            delta =  m_size.x() / e.x();

        Vector2f s = scroll();
        s.x() = std::clamp(s.x() + (float)(delta * 0.98), 0.f, 1.f);
        set_scroll(s);
        return true;
    }
    else if (!down) {
        m_scrolling_x = false;
    }

    // ---- Middle-button pan start ----
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        m_panning = down;
        if (down)
            return true;
    }

    // If we clicked over a scrollbar but no axis matched, swallow it.
    if (on_vbar || on_hbar)
        return true;

    // ---- Forward to child in logical coords ----
    if (!m_children.empty()) {
        Widget* child = m_children[0];
        Vector2i cp = to_child(p);
        if (child->visible() && child->contains(cp))
            return child->mouse_button_event(cp, button, down, modifiers);
    }
    return false;
}

bool ZoomScrollPanel::mouse_motion_event(const Vector2i& p, const Vector2i& rel,
                                         int button, int modifiers) {
    if (m_children.empty())
        return false;
    Widget* child = m_children[0];
    Vector2i cp  = to_child(p);
    Vector2i crel = delta_to_child(rel);

    bool contained = child->contains(cp);
    bool prev_contained = child->contains(cp - crel);
    if (contained || prev_contained)
        return child->mouse_motion_event(cp, crel, button, modifiers);
    return false;
}

bool ZoomScrollPanel::mouse_drag_event(const Vector2i& p, const Vector2i& rel,
                                       int button, int modifiers) {
    Vector2d e = effective_child_size();

    // Scrollbar drag (axis-specific because m_scrolling_* gets set in mouse_button_event).
    if ((m_scrolling_y || m_scrolling_x) && !m_children.empty()) {
        if (m_scrolling_y && VScrollable() && e.y() > m_size.y()) {
            double scrollh = m_size.y() * std::min(1.0, m_size.y() / e.y());
            Vector2f s = scroll();
            s.y() = std::clamp(s.y() + (float)(rel.y() / (m_size.y() - 8.0 - scrollh)),
                               0.f, 1.f);
            set_scroll(s);
        }
        if (m_scrolling_x && HScrollable() && e.x() > m_size.x()) {
            double scrollw = m_size.x() * std::min(1.0, m_size.x() / e.x());
            Vector2f s = scroll();
            s.x() = std::clamp(s.x() + (float)(rel.x() / (m_size.x() - 8.0 - scrollw)),
                               0.f, 1.f);
            set_scroll(s);
        }
        return true;
    }

    // Middle-button pan.
    if (m_panning) {
        m_pan_offset.x() += rel.x();
        m_pan_offset.y() += rel.y();
        clamp_state();
        m_update_layout = true;
        return true;
    }

    // Otherwise propagate to child.
    if (!m_children.empty()) {
        Widget* child = m_children[0];
        Vector2i cp = to_child(p);
        Vector2i crel = delta_to_child(rel);
        if (child->visible() && (child->contains(cp) || child->contains(cp - crel)))
            return child->mouse_drag_event(cp, crel, button, modifiers);
    }

    return Widget::mouse_drag_event(p, rel, button, modifiers);
}

bool ZoomScrollPanel::scroll_event(const Vector2i& p, const Vector2f& rel) {
    // Ctrl + wheel = zoom about cursor.
    if (m_zoom_enabled && (glfwGetKey(screen()->glfw_window(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                           glfwGetKey(screen()->glfw_window(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS))
    {
        double factor = std::pow(m_zoom_wheel_step, (double)rel.y());
        Vector2i anchor(p.x() - m_pos.x(), p.y() - m_pos.y());
        zoom_by(factor, anchor);
        return true;
    }

    Vector2d e = effective_child_size();

    // Shift+wheel = horizontal scroll. Otherwise vertical.
    bool shift = (glfwGetKey(screen()->glfw_window(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                  glfwGetKey(screen()->glfw_window(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

    bool used = false;

    if (!shift && VScrollable() && e.y() > m_size.y() && rel.y() != 0.f) {
        m_pan_offset.y() += rel.y() * m_size.y() * 0.25;
        used = true;
    }
    if (shift && HScrollable() && e.x() > m_size.x() && rel.y() != 0.f) {
        m_pan_offset.x() += rel.y() * m_size.x() * 0.25;
        used = true;
    }
    if (!shift && HScrollable() && e.x() > m_size.x() && rel.x() != 0.f) {
        // True 2D scroll deltas (trackpads, horizontal wheels, etc.)
        m_pan_offset.x() += rel.x() * m_size.x() * 0.25;
        used = true;
    }

    if (used) {
        clamp_state();
        m_update_layout = true;
        return true;
    }

    return Widget::scroll_event(p, rel);
}

bool ZoomScrollPanel::zoom_event(double magnification, const Vector2i& pos) {
    if (!m_zoom_enabled)
        return false;

    // pos is already in panel-local coordinates
    zoom_by(1.0 + magnification * 5.0, pos);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

void ZoomScrollPanel::draw(NVGcontext* ctx) {
    if (m_children.empty())
        return;
    Widget* child = m_children[0];

    if (m_update_layout) {
        m_update_layout = false;
        child->perform_layout(ctx);
        m_child_preferred_size = child->preferred_size(ctx);
        clamp_state();
    }

    Vector2d e = effective_child_size();

    bool show_vbar = VScrollable() && e.y() > m_size.y();
    bool show_hbar = HScrollable() && e.x() > m_size.x();

    // We always keep child position at (0,0); pan/zoom is in the matrix.
    child->set_position(Vector2i(0, 0));

    // --- Child rendering with transform + scissor ---
    nvgSave(ctx);
    nvgTranslate(ctx, (float)m_pos.x(), (float)m_pos.y());
    // Scissor in panel-local pixel space (before zoom transform):
    nvgIntersectScissor(ctx,
                        0, 0,
                        (float)(m_size.x() - (show_vbar ? kScrollbarSlot - 2 : 0)),
                        (float)(m_size.y() - (show_hbar ? kScrollbarSlot - 2 : 0)));
    nvgTranslate(ctx, (float)m_pan_offset.x(), (float)m_pan_offset.y());
    nvgScale(ctx, (float)m_zoom, (float)m_zoom);
    if (child->visible())
        child->draw(ctx);
    nvgRestore(ctx);

    // --- Scrollbars (drawn in screen/panel-local pixel space) ---
    if (show_vbar) {
        double scrollh = m_size.y() * std::min(1.0, m_size.y() / e.y());
        float sy = (float)scroll().y();

        NVGpaint paint = nvgBoxGradient(
            ctx, m_pos.x() + m_size.x() - kScrollbarSlot + 1,
                 m_pos.y() + kScrollbarMargin + 1,
                 kScrollbarThick, m_size.y() - 8, 3, 4,
                 Color(0, 32), Color(0, 92));
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + m_size.x() - kScrollbarSlot,
                       m_pos.y() + kScrollbarMargin,
                       kScrollbarThick, m_size.y() - 8, 3);
        nvgFillPaint(ctx, paint);
        nvgFill(ctx);

        paint = nvgBoxGradient(
            ctx, m_pos.x() + m_size.x() - kScrollbarSlot - 1,
                 m_pos.y() + kScrollbarMargin +
                     (float)((m_size.y() - 8 - scrollh) * sy) - 1,
                 kScrollbarThick, (float)scrollh,
                 3, 4, Color(220, 100), Color(128, 100));
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + m_size.x() - kScrollbarSlot + 1,
                       m_pos.y() + kScrollbarMargin + 1 +
                           (float)((m_size.y() - 8 - scrollh) * sy),
                       kScrollbarThick - 2, (float)(scrollh - 2), 2);
        nvgFillPaint(ctx, paint);
        nvgFill(ctx);
    }

    if (show_hbar) {
        double scrollw = m_size.x() * std::min(1.0, m_size.x() / e.x());
        float sx = (float)scroll().x();

        NVGpaint paint = nvgBoxGradient(
            ctx, m_pos.x() + kScrollbarMargin + 1,
                 m_pos.y() + m_size.y() - kScrollbarSlot + 1,
                 m_size.x() - 8, kScrollbarThick, 3, 4,
                 Color(0, 32), Color(0, 92));
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + kScrollbarMargin,
                       m_pos.y() + m_size.y() - kScrollbarSlot,
                       m_size.x() - 8, kScrollbarThick, 3);
        nvgFillPaint(ctx, paint);
        nvgFill(ctx);

        paint = nvgBoxGradient(
            ctx, m_pos.x() + kScrollbarMargin +
                     (float)((m_size.x() - 8 - scrollw) * sx) - 1,
                 m_pos.y() + m_size.y() - kScrollbarSlot - 1,
                 (float)scrollw, kScrollbarThick, 3, 4,
                 Color(220, 100), Color(128, 100));
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx,
                       m_pos.x() + kScrollbarMargin + 1 +
                           (float)((m_size.x() - 8 - scrollw) * sx),
                       m_pos.y() + m_size.y() - kScrollbarSlot + 1,
                       (float)(scrollw - 2), kScrollbarThick - 2, 2);
        nvgFillPaint(ctx, paint);
        nvgFill(ctx);
    }
}

NAMESPACE_END(nanogui)
