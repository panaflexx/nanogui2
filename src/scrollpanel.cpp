/*
    src/ScrollPanel.cpp -- Adds a vertical scrollbar around a widget
    that is too big to fit into a certain area

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/scrollpanel.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <stdexcept>

NAMESPACE_BEGIN(nanogui)

ScrollPanel::ScrollPanel(Widget* parent)
    : WidgetCRTP<ScrollPanel>(parent), m_child_preferred_size(Vector2i(0, 0)),
    m_scroll(0.f, 0.f), m_update_layout(false), m_scroll_type(ScrollTypes::Vertical) {
    DebugName = m_parent->DebugName + ",ScrlPnl";
}

ScrollPanel::ScrollPanel(Widget* parent, ScrollTypes scroll_type)
    : WidgetCRTP<ScrollPanel>(parent), m_child_preferred_size(Vector2i(0, 0)),
    m_scroll(0.f, 0.f), m_update_layout(false), m_scroll_type(scroll_type) {
    DebugName = m_parent->DebugName + ",ScrlPnl";
}


void ScrollPanel::perform_layout(NVGcontext* ctx) {
    if (m_children.empty())
        return;
    if (m_children.size() > 1)
        throw std::runtime_error("ScrollPanel should have one child.");

    // Honor Widget::set_fill_parent (grow with parent's content area).
    apply_fill_parent();

    Widget* child = m_children[0];
    Vector2i available = m_size;

    // FIRST: Give child the available space as a constraint
    child->set_size(available);
    child->perform_layout(ctx);

    // THEN: Get child's preferred size within that constraint
    Vector2i constrained_preferred = child->preferred_size(ctx);

    // Update stored preferred size for scrolling calculations
    m_child_preferred_size = constrained_preferred;

    // Determine final child size:
    // - If child needs more space than available: give it what it needs (enable scrolling)
    // - If child needs less space than available: let it fill the available space
    int child_height = constrained_preferred.y() > available.y() ?
                       constrained_preferred.y() : available.y();
    int child_width = constrained_preferred.x() > available.x() ?
                      constrained_preferred.x() : available.x();

    // Position child for scroll amount
    int offset_x = HScrollable() && constrained_preferred.x() > available.x()
        ? -(int)(m_scroll.x() * (constrained_preferred.x() - available.x()))
        : 0;
    int offset_y = VScrollable() && constrained_preferred.y() > available.y()
        ? -(int)(m_scroll.y() * (constrained_preferred.y() - available.y()))
        : 0;

    child->set_position(Vector2i(offset_x, offset_y));
    child->set_size(Vector2i(child_width, child_height));

    // Final layout with correct size and position
    child->perform_layout(ctx);
}

// FIXME: Need to be able to capture side-scroll events DOES NOT WORK WITH TEXTBOX (focus problem)
bool ScrollPanel::keyboard_event(int key, int scancode, int action, int modifiers) {
	//printf("keyboard_event: key=%d action=%d focused=%s\n", key, action,
	//	focused()?"TRUE":"FALSE");

    /*
    if (focused()) {
		if(modifiers == GLFW_MOD_SHIFT)
			printf("Got shift!\n");
	}
	*/

    auto child = m_children[0];
	return child->keyboard_event(key, scancode, action, modifiers);
}

Vector2i ScrollPanel::preferred_size(NVGcontext* ctx) const {
    if (m_children.empty())
        return Vector2i(0);
    return m_children[0]->preferred_size(ctx) + Vector2i(12, 0);
}

bool ScrollPanel::mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    if (m_scrolling_y) m_vel_y = 0.0f;
    if (m_scrolling_x) m_vel_x = 0.0f;

    if ((m_scrolling_y || m_scrolling_x) && !m_children.empty() && (m_child_preferred_size.y() > m_size.y() || m_child_preferred_size.x() > m_size.x())) {
        if (m_scrolling_y && m_child_preferred_size.y() > m_size.y() && VScrollable())
        {
            float scrollh = height() * std::min(1.f, height() / (float)m_child_preferred_size.y());
            m_scroll.y() = std::max(0.f, std::min(1.f, m_scroll.y() + rel.y() / (m_size.y() - 8.f - scrollh)));
        }
        if (m_scrolling_x && m_child_preferred_size.x() > m_size.x() && HScrollable())
        {
            float scrollw = width() * std::min(1.f, width() / (float)m_child_preferred_size.x());
            m_scroll.x() = std::max(0.f, std::min(1.f, m_scroll.x() + rel.x() / (m_size.x() - 8.f - scrollw)));
        }
        m_update_layout = true;
        return true;
    }
    return Widget::mouse_drag_event(p, rel, button, modifiers);
}

bool ScrollPanel::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {

    bool OverVertical = m_child_preferred_size.y() > m_size.y() &&
        p.x() > m_pos.x() + m_size.x() - 13 &&
        p.x() < m_pos.x() + m_size.x() - 4;
    bool OverHorizontal = m_child_preferred_size.x() > m_size.x() &&
        p.y() > m_pos.y() + m_size.y() - 13 &&
        p.y() < m_pos.y() + m_size.y() - 4;
    if (down && button == GLFW_MOUSE_BUTTON_1 && !m_children.empty() && VScrollable() && OverVertical) {

        m_scrolling_y = true;
        m_vel_y = 0.0f; m_vel_x = 0.0f;

        int scrollh = (int)(height() *
            std::min(1.f, height() / (float)m_child_preferred_size.y()));
        int start = (int)(m_pos.y() + 4 + 1 + (m_size.y() - 8 - scrollh) * m_scroll.y());

        float delta = 0.f;

        if (p.y() < start)
            delta = -m_size.y() / (float)m_child_preferred_size.y();
        else if (p.y() > start + scrollh)
            delta = m_size.y() / (float)m_child_preferred_size.y();

        m_scroll.y() = std::max(0.f, std::min(1.f, m_scroll.y() + delta * 0.98f));

        m_children[0]->set_position(
            Vector2i(0, -m_scroll.y() * (m_child_preferred_size.y() - m_size.y())));
        m_update_layout = true;
        return true;
    }
    else m_scrolling_y = false;
    if (down && button == GLFW_MOUSE_BUTTON_1 && !m_children.empty() && HScrollable() && OverHorizontal) {

        m_scrolling_x = true;
        m_vel_y = 0.0f; m_vel_x = 0.0f;
        int scrollw = (int)(width() *
            std::min(1.f, width() / (float)m_child_preferred_size.x()));
        int start = (int)(m_pos.x() + 4 + 1 + (m_size.x() - 8 - scrollw) * m_scroll.x());

        float delta = 0.f;

        if (p.x() < start)
            delta = -m_size.x() / (float)m_child_preferred_size.x();
        else if (p.x() > start + scrollw)
            delta = m_size.x() / (float)m_child_preferred_size.x();

        m_scroll.x() = std::max(0.f, std::min(1.f, m_scroll.x() + delta * 0.98f));

        m_children[0]->set_position(
            Vector2i(0, -m_scroll.x() * (m_child_preferred_size.x() - m_size.x())));
        m_update_layout = true;
        return true;
    }
    else m_scrolling_x = false;
    if (OverVertical || OverHorizontal)//if mouse is on scrollbar, dont check the other widgets

        return true;
    if (Widget::mouse_button_event(p, button, down, modifiers))
        return true;

    return false;
}

bool ScrollPanel::scroll_event(const Vector2i& p, const Vector2f& rel) {
    bool used = false;
    if (!m_children.empty() && m_child_preferred_size.y() > m_size.y() && VScrollable() && rel.y() != 0.f) {
        m_vel_y = std::clamp(m_vel_y - rel.y() * (float)m_size.y() * 0.6f, -3500.f, 3500.f);
        used = true;
    }
    if (!m_children.empty() && m_child_preferred_size.x() > m_size.x() && HScrollable() && rel.y() != 0.f) {
        m_vel_x = std::clamp(m_vel_x - rel.y() * (float)m_size.x() * 0.6f, -3500.f, 3500.f);
        used = true;
    }
    if (!m_children.empty() && m_child_preferred_size.x() > m_size.x() && HScrollable() && rel.x() != 0.f) {
        m_vel_x = std::clamp(m_vel_x - rel.x() * (float)m_size.x() * 0.6f, -3500.f, 3500.f);
        used = true;
    }
    if (used) { screen()->redraw(); return true; }
    return Widget::scroll_event(p, rel);
}

void ScrollPanel::draw(NVGcontext* ctx) {
    if (m_children.empty())
        return;
    Widget* child = m_children[0];

    // ---- Inertia integration ----
    {
        double now = glfwGetTime();
        float dt = (m_last_t > 0.0) ? std::min((float)(now - m_last_t), 0.05f) : 0.0f;
        m_last_t = now;
        bool moving = false;

        if (std::abs(m_vel_y) > 0.5f && VScrollable() && m_child_preferred_size.y() > m_size.y()) {
            float range = (float)(m_child_preferred_size.y() - m_size.y());
            float cur   = m_scroll.y() * range + m_vel_y * dt;
            cur = std::clamp(cur, 0.f, range);
            m_scroll.y() = cur / range;
            if (cur <= 0.f || cur >= range) m_vel_y = 0.f;
            else {
                m_vel_y *= std::exp(-8.0f * dt);
                if (std::abs(m_vel_y) < 0.5f) m_vel_y = 0.f;
            }
            moving = true;
        }
        if (std::abs(m_vel_x) > 0.5f && HScrollable() && m_child_preferred_size.x() > m_size.x()) {
            float range = (float)(m_child_preferred_size.x() - m_size.x());
            float cur   = m_scroll.x() * range + m_vel_x * dt;
            cur = std::clamp(cur, 0.f, range);
            m_scroll.x() = cur / range;
            if (cur <= 0.f || cur >= range) m_vel_x = 0.f;
            else {
                m_vel_x *= std::exp(-8.0f * dt);
                if (std::abs(m_vel_x) < 0.5f) m_vel_x = 0.f;
            }
            moving = true;
        }
        if (moving) { m_update_layout = true; screen()->redraw(); }
    }

    if (m_update_layout) {
        m_update_layout = false;
        child->perform_layout(ctx);
    }

    int yoffset = 0, xoffset = 0;
    if (m_child_preferred_size.y() > m_size.y() && VScrollable())
        yoffset = -(int)(m_scroll.y() * (m_child_preferred_size.y() - m_size.y()));
    if (m_child_preferred_size.x() > m_size.x() && HScrollable())
        xoffset = -(int)(m_scroll.x() * (m_child_preferred_size.x() - m_size.x()));

    child->set_position(Vector2i(xoffset, yoffset));

    // Draw child, clipped to full panel area (scrollbar overlays)
    nvgSave(ctx);
    nvgTranslate(ctx, m_pos.x(), m_pos.y());
    nvgIntersectScissor(ctx, 0, 0, (float)m_size.x(), (float)m_size.y());
    if (child->visible())
        child->draw(ctx);
    nvgRestore(ctx);

    // ---- Pill-style overlay scrollbar (no track drawn) ----
    constexpr float SB_W      = 6.0f;
    constexpr float SB_MARGIN = 3.0f;
    constexpr float SB_MIN    = 28.0f;

    if (m_child_preferred_size.y() > m_size.y() && VScrollable()) {
        float vis   = (float)m_size.y() / (float)m_child_preferred_size.y();
        float th    = std::max(SB_MIN, (float)m_size.y() * vis);
        float track = (float)m_size.y() - th;
        float ty    = m_pos.y() + m_scroll.y() * track;
        float tx    = m_pos.x() + (float)m_size.x() - SB_W - SB_MARGIN;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, tx, ty + 3.f, SB_W, th - 6.f, SB_W * 0.5f);
        nvgFillColor(ctx, m_scrolling_y ? nvgRGBA(100,110,130,230) : nvgRGBA(150,155,165,180));
        nvgFill(ctx);
    }
    if (m_child_preferred_size.x() > m_size.x() && HScrollable()) {
        float vis   = (float)m_size.x() / (float)m_child_preferred_size.x();
        float tw    = std::max(SB_MIN, (float)m_size.x() * vis);
        float track = (float)m_size.x() - tw;
        float tx    = m_pos.x() + m_scroll.x() * track;
        float ty    = m_pos.y() + (float)m_size.y() - SB_W - SB_MARGIN;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, tx + 3.f, ty, tw - 6.f, SB_W, SB_W * 0.5f);
        nvgFillColor(ctx, m_scrolling_x ? nvgRGBA(100,110,130,230) : nvgRGBA(150,155,165,180));
        nvgFill(ctx);
    }
}

NAMESPACE_END(nanogui)
