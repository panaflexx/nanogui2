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
#include <stdexcept>

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

namespace {
void uncache_zoom_ancestors(Widget* w) {
    for (; w; w = w->parent()) if (w->cached()) w->set_cached(false);
}
}

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
    set_live(true);
    uncache_zoom_ancestors(m_parent);
}

/* ------------------------------------------------------------------ */
/*  Geometry helpers                                                   */
/* ------------------------------------------------------------------ */

ZoomScrollPanel::Vector2d ZoomScrollPanel::effective_child_size() const {
    if (m_children.empty())
        return Vector2d(0,0);
    double prefW = (double)m_child_preferred_size.x();
    double prefH = (double)m_child_preferred_size.y();
    // HtmlDocument zeroes preferred width (p.x=0) so logical width is the
    // laid-out child size (view_w). Without this, e.x==0 and H-scroll/pan
    // and the horizontal scrollbar never appear when zoomed.
    if (prefW <= 0.5) {
        prefW = (double)m_children[0]->size().x();
        if (prefW <= 0.5) prefW = (double)m_size.x();
    }
    if (prefH <= 0.5) {
        prefH = (double)m_children[0]->size().y();
        if (prefH <= 0.5) prefH = (double)m_size.y();
    }
    return Vector2d(prefW * m_zoom, prefH * m_zoom);
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
    // When the child is smaller than the viewport: canvas-style zoom
    // (reflow_on_zoom) centers it; document/email mode pins to the
    // top-left so a short message is not floating in the middle of the
    // pane (and so Widget::absolute_position matches what is drawn).
    if (e.x() <= m_size.x()) {
        if (!m_reflow_on_zoom) m_pan_offset.x() = 0.0;
        else                   m_pan_offset.x() = (m_size.x() - e.x()) * 0.5;
    } else {
        double lo = (double)m_size.x() - e.x();
        double hi = 0.0;
        m_pan_offset.x() = std::clamp(m_pan_offset.x(), lo, hi);
    }
    if (e.y() <= m_size.y()) {
        if (!m_reflow_on_zoom) m_pan_offset.y() = 0.0;
        else                   m_pan_offset.y() = (m_size.y() - e.y()) * 0.5;
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
    double old = m_zoom;
    m_zoom = z;
    clamp_state();
    if (!m_reflow_on_zoom && old > 1e-9 && m_zoom > 1e-9) { /* visual only */ }
    else m_update_layout = true;
    if (Screen* s = screen()) s->redraw();
}

void ZoomScrollPanel::set_zoom_about(double z, const Vector2i& anchor) {
    double old = m_zoom;
    double new_zoom = std::clamp(z, m_zoom_min, m_zoom_max);
    if (m_zoom <= 0.0) {
        m_zoom = new_zoom;
    } else {
        double k = new_zoom / m_zoom;
        m_pan_offset.x() = anchor.x() - (anchor.x() - m_pan_offset.x()) * k;
        m_pan_offset.y() = anchor.y() - (anchor.y() - m_pan_offset.y()) * k;
        m_zoom = new_zoom;
    }
    clamp_state();
    if (m_reflow_on_zoom || std::abs(old - m_zoom) < 1e-9) m_update_layout = true;
    else if (Screen* s = screen()) s->redraw();
    else m_update_layout = true;
}

void ZoomScrollPanel::reset_view() {
    m_zoom = 1.0;
    m_pan_offset = Vector2d(0.0, 0.0);
    clamp_state();
    m_update_layout = true;
    if (Screen* s = screen()) s->redraw();
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
    if (m_reflow_on_zoom) m_update_layout = true;
    else if (Screen* s = screen()) s->redraw();
    else m_update_layout = true;
}

/* ------------------------------------------------------------------ */
/*  Layout                                                             */
/* ------------------------------------------------------------------ */

void ZoomScrollPanel::perform_layout(NVGcontext* ctx) {
    if (m_children.empty())
        return;
    if (m_children.size() > 1)
        throw std::runtime_error("ZoomScrollPanel should have one child.");

    // Honor Widget::set_fill_parent (grow with parent's content area).
    apply_fill_parent();

    Widget* child = m_children[0];

    child->set_position(Vector2i(0, 0));

    // Viewport size expressed in the child's (pre-zoom) logical units.
    // When reflow_on_zoom is false (email), keep logical width at panel width: wrap
    // stays, zoom is pure visual (H-scroll appears when zoomed).
    int view_w, view_h;
    if (!m_reflow_on_zoom) { view_w = m_size.x(); view_h = m_size.y(); }
    else { view_w = (int)std::ceil(m_size.x() / std::max(m_zoom, 1e-9)); view_h = (int)std::ceil(m_size.y() / std::max(m_zoom, 1e-9)); }

    // First pass: constrain the child to the viewport so layouts that
    // can shrink (e.g. FlexLayout with flex_shrink, AlignItems::Stretch)
    // get a chance to do so. Without this step, the child would always
    // report its natural (unconstrained) preferred size and we would
    // never shrink below that.
    child->set_size(Vector2i(view_w, view_h));
    child->perform_layout(ctx);

    // Now ask the child what it actually wants inside that constraint.
    Vector2i pref = child->preferred_size(ctx);

    // Final child size: at least the viewport (so it visually fills the
    // panel when the content is smaller), but larger if the child wants
    // more (in which case scrolling kicks in). The viewport constraint
    // here is in pre-zoom logical units, matching `pref`.
    // In no-reflow email mode, don't grow logical width beyond view_w —
    // horizontal overflow when zoomed is provided by Zoom's scale transform
    // (effective_child_size = m_child_preferred_size * zoom). Growing here
    // would push the text block off-screen to the right.
    Vector2i child_size;
    if (!m_reflow_on_zoom) {
        child_size = Vector2i(view_w, std::max(pref.y(), view_h));
    } else {
        child_size = Vector2i(std::max(pref.x(), view_w), std::max(pref.y(), view_h));
    }

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

Widget* ZoomScrollPanel::find_widget(const Vector2i& p) {
    if (!m_visible) return nullptr;
    if (!m_children.empty() && m_children[0]->visible() && contains(p)) {
        Widget* child = m_children[0];
        Vector2i cp = to_child(p);
        // cp is in child's logical (0,0) space; delegate to child's subtree
        if (Widget* hit = child->find_widget(cp))
            return hit;
    }
    return contains(p) ? this : nullptr;
}

const Widget* ZoomScrollPanel::find_widget(const Vector2i& p) const {
    if (!m_visible) return nullptr;
    if (!m_children.empty() && m_children[0]->visible() && contains(p)) {
        const Widget* child = m_children[0];
        Vector2i cp = to_child(p);
        if (const Widget* hit = child->find_widget(cp))
            return hit;
    }
    return contains(p) ? this : nullptr;
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
        m_vel_y = 0.0; m_vel_x = 0.0;

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
        m_vel_y = 0.0; m_vel_x = 0.0;

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
    if (m_scrolling_y || m_scrolling_x) { m_vel_x = 0.0; m_vel_y = 0.0; }
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

    bool shift = (glfwGetKey(screen()->glfw_window(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                  glfwGetKey(screen()->glfw_window(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

    bool used = false;

    // True 2-finger horizontal uses rel.x; GLFW/libinput normalizes it oddly on X,
    // but H-scroll must work immediately when zoomed (e.x > size.x via effective size
    // derived from the laid-out child width), not just when logical pref says so.
    // So use the effective size above for both axes.
    bool can_v = VScrollable() && e.y() > m_size.y();
    bool can_h = HScrollable() && e.x() > m_size.x();
    if (rel.x() != 0.f && can_h) {
        // Direct pan for touchpad (no inertia latency); also prime vel for coasting.
        double dx = (double)rel.x() * 18.0; // tune: 18 px per wheel tick ~ native feel
        m_pan_offset.x() += dx;
        clamp_state();
        m_vel_x = std::clamp((double)rel.x() * m_size.x() * 0.6, -3500.0, 3500.0);
        used = true;
    }
    if (!shift && can_v && rel.y() != 0.f) {
        m_vel_y = std::clamp(m_vel_y + (double)rel.y() * m_size.y() * 0.6, -3500.0, 3500.0);
        used = true;
    }
    if (shift && can_h && rel.y() != 0.f) {
        double dx = (double)rel.y() * 18.0;
        m_pan_offset.x() += dx; clamp_state();
        m_vel_x = std::clamp(m_vel_x + (double)rel.y() * m_size.x() * 0.6, -3500.0, 3500.0);
        used = true;
    }

    if (used) {
        if (Screen* s = screen()) s->redraw();
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

    // ---- Inertia integration ----
    {
        double now = glfwGetTime();
        double dt  = (m_last_t > 0.0) ? std::min(now - m_last_t, 0.05) : 0.0;
        m_last_t = now;
        bool moving = false;

        Vector2d e = effective_child_size();

        if (std::abs(m_vel_y) > 0.5 && VScrollable() && e.y() > m_size.y()) {
            m_pan_offset.y() += m_vel_y * dt;
            clamp_state();
            m_vel_y *= std::exp(-8.0 * dt);
            if (std::abs(m_vel_y) < 0.5) m_vel_y = 0.0;
            moving = true;
        }
        if (std::abs(m_vel_x) > 0.5 && HScrollable() && e.x() > m_size.x()) {
            m_pan_offset.x() += m_vel_x * dt;
            clamp_state();
            m_vel_x *= std::exp(-8.0 * dt);
            if (std::abs(m_vel_x) < 0.5) m_vel_x = 0.0;
            moving = true;
        }
        if (moving) { m_update_layout = true; screen()->redraw(); }
    }

    if (m_update_layout) {
        m_update_layout = false;
        child->perform_layout(ctx);
        m_child_preferred_size = child->preferred_size(ctx);
        clamp_state();
    }

    Vector2d e = effective_child_size();
    bool show_vbar = VScrollable() && e.y() > m_size.y();
    bool show_hbar = HScrollable() && e.x() > m_size.x();

    child->set_position(Vector2i(0, 0));

    // Child rendering — scissor covers full panel (scrollbar overlays)
    nvgSave(ctx);
    nvgTranslate(ctx, (float)m_pos.x(), (float)m_pos.y());
    nvgIntersectScissor(ctx, 0, 0, (float)m_size.x(), (float)m_size.y());
    nvgTranslate(ctx, (float)m_pan_offset.x(), (float)m_pan_offset.y());
    nvgScale(ctx, (float)m_zoom, (float)m_zoom);
    if (child->visible())
        child->draw(ctx);
    nvgRestore(ctx);

    // ---- Pill-style overlay scrollbars (no track) ----
    constexpr float SB_W      = 6.0f;
    constexpr float SB_MARGIN = 3.0f;
    constexpr float SB_MIN    = 28.0f;

    if (show_vbar) {
        float vis   = (float)m_size.y() / (float)e.y();
        float th    = std::max(SB_MIN, (float)m_size.y() * vis);
        float track = (float)m_size.y() - th;
        float sy    = (float)scroll().y();
        float ty    = m_pos.y() + sy * track;
        float tx    = m_pos.x() + (float)m_size.x() - SB_W - SB_MARGIN;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, tx, ty + 3.f, SB_W, th - 6.f, SB_W * 0.5f);
        Color thumb = m_theme
            ? (m_scrolling_y ? m_theme->m_scrollbar_thumb_active : m_theme->m_scrollbar_thumb)
            : Color(150, 180);
        nvgFillColor(ctx, thumb);
        nvgFill(ctx);
    }
    if (show_hbar) {
        float vis   = (float)m_size.x() / (float)e.x();
        float tw    = std::max(SB_MIN, (float)m_size.x() * vis);
        float track = (float)m_size.x() - tw;
        float sx    = (float)scroll().x();
        float tx    = m_pos.x() + sx * track;
        float ty    = m_pos.y() + (float)m_size.y() - SB_W - SB_MARGIN;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, tx + 3.f, ty, tw - 6.f, SB_W, SB_W * 0.5f);
        Color thumb = m_theme
            ? (m_scrolling_x ? m_theme->m_scrollbar_thumb_active : m_theme->m_scrollbar_thumb)
            : Color(150, 180);
        nvgFillColor(ctx, thumb);
        nvgFill(ctx);
    }
}

NAMESPACE_END(nanogui)
