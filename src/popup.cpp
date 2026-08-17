/*
    src/popup.cpp -- Simple popup widget which is attached to another given
    window (can be nested)

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/popup.h>
#include <nanogui/popupbutton.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <algorithm>

NAMESPACE_BEGIN(nanogui)

Popup::Popup(Widget* parent, Window* parent_window, PopupButton* parent_button)
    : Window(parent, ""), m_parent_window(parent_window), m_parent_button(parent_button), m_anchor_pos(Vector2i(0)),
    m_anchor_offset(30), m_anchor_size(10), m_side(Side::Right) { }

void Popup::perform_layout(NVGcontext* ctx) {
    if (m_layout || m_children.size() != 1) {
        Widget::perform_layout(ctx);
    }
    else {
        m_children[0]->set_position(Vector2i(0));
        m_children[0]->set_size(m_size);
        m_children[0]->perform_layout(ctx);
    }
}

void Popup::refresh_relative_placement() {
    if (!m_parent_window)
        return;
    m_parent_window->refresh_relative_placement();
    m_visible &= m_parent_window->visible_recursive();

    Vector2i TempPos;
    Vector2i AnchorPos;
    // calculate anchor position here.
    if (side() == Popup::Right)
    {
        TempPos = m_parent_button->absolute_position() + Vector2i(m_parent_button->size().x() + m_anchor_size, (m_parent_button->size().y() - m_size.y()) / 2);
        AnchorPos = m_parent_button->absolute_position() + Vector2i(m_parent_button->size().x(), m_parent_button->size().y() / 2);
        if (TempPos.y() < 0)//if the popup does not fit the top of the  screen, then move it lower
            TempPos = Vector2i(m_parent_button->absolute_position().x() + m_parent_button->size().x() + m_anchor_size, 0);
    }
    else if (side() == Popup::Left)
    {
        TempPos = m_parent_button->absolute_position() + Vector2i(-m_size.x() - m_anchor_size, (m_parent_button->size().y() - m_size.y()) / 2);
        AnchorPos = m_parent_button->absolute_position() + Vector2i(-m_anchor_size, m_parent_button->size().y() / 2);
        if (TempPos.x() < 0)//if the popup does not fit in the left of screen, then move it below the button
        {
            TempPos = Vector2i(0, m_parent_button->absolute_position().y() + m_anchor_size + m_parent_button->size().y());
            AnchorPos = m_parent_button->absolute_position() + Vector2i(0, m_parent_button->size().y() + m_anchor_size);
        }
        else if (TempPos.y() < 0)//if the popup does not fit the top of the  screen, then move it lower
            TempPos = Vector2i(m_parent_button->absolute_position().x() - m_size.x() - m_anchor_size, 0);
    }
    else// bottom
    {
        TempPos = m_parent_button->absolute_position() + Vector2i((m_parent_button->size().x() - m_size.x()) / 2, m_parent_button->size().y() + m_anchor_size);
        AnchorPos = m_parent_button->absolute_position() + Vector2i(m_parent_button->size().x() / 2, m_parent_button->size().y());
        if (TempPos.x() < 0)//if the popup does not fit the top of the  screen, then move it lower
            TempPos = Vector2i(0, m_parent_button->absolute_position().y() + m_parent_button->size().y() + m_anchor_size);
    }

    set_anchor_pos(AnchorPos);
    m_pos = TempPos;

}

void Popup::draw(NVGcontext* ctx) {
    refresh_relative_placement();

    if (!m_visible)
        return;

    int ds = m_theme->m_window_drop_shadow_size;
    // Popovers are slightly tighter than windows but still glass-rounded
    float cr = std::max(10.f, (float)m_theme->m_window_corner_radius - 2.f);
    float fx = (float)m_pos.x(), fy = (float)m_pos.y();
    float fw = (float)m_size.x(), fh = (float)m_size.y();

    nvgSave(ctx);
    nvgResetScissor(ctx);

    m_theme->draw_glass_shadow(ctx, fx, fy, fw, fh, cr, (float)ds);

    /* Glass body + anchor beak as one filled path */
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
    if (m_side == Side::Right) {
        nvgMoveTo(ctx, m_anchor_pos.x() + m_anchor_size, m_anchor_pos.y());
        nvgLineTo(ctx, m_anchor_pos.x() - 1, m_anchor_pos.y() - m_anchor_size);
        nvgLineTo(ctx, m_anchor_pos.x() - 1, m_anchor_pos.y() + m_anchor_size);
    } else if (m_side == Side::Left) {
        nvgMoveTo(ctx, m_anchor_pos.x() + m_anchor_size, m_anchor_pos.y());
        nvgLineTo(ctx, m_anchor_pos.x(), m_anchor_pos.y() - m_anchor_size);
        nvgLineTo(ctx, m_anchor_pos.x() - m_anchor_size, m_anchor_pos.y());
        nvgLineTo(ctx, m_anchor_pos.x(), m_anchor_pos.y() + m_anchor_size);
    } else {
        nvgMoveTo(ctx, m_anchor_pos.x(), m_anchor_pos.y() + m_anchor_size);
        nvgLineTo(ctx, m_anchor_pos.x() - m_anchor_size, m_anchor_pos.y() - 1);
        nvgLineTo(ctx, m_anchor_pos.x() + m_anchor_size, m_anchor_pos.y() - 1);
    }
    nvgFillColor(ctx, m_theme->m_window_popup);
    nvgFill(ctx);

    /* Hairline glass edge on body */
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, fx + 0.5f, fy + 0.5f, fw - 1.f, fh - 1.f, cr - 0.5f);
    nvgStrokeWidth(ctx, 1.f);
    nvgStrokeColor(ctx, m_theme->m_glass_border);
    nvgStroke(ctx);

    /* Soft specular on popup top */
    float band = std::min(fh * 0.35f, 18.f);
    NVGpaint wash = nvgLinearGradient(ctx, fx, fy, fx, fy + band,
                                      m_theme->m_glass_specular, m_theme->m_transparent);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, fx + 1.f, fy + 1.f, fw - 2.f, band, cr - 1.f);
    nvgFillPaint(ctx, wash);
    nvgFill(ctx);

    nvgRestore(ctx);

    Widget::draw(ctx);
}

NAMESPACE_END(nanogui)
