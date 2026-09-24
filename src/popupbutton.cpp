/*
    src/popupbutton.cpp -- Button which launches a popup widget

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/popupbutton.h>
#include <nanogui/theme.h>
#include <nanogui/screen.h>
#include <nanogui/opengl.h>

NAMESPACE_BEGIN(nanogui)

PopupButton::PopupButton(Widget* parent, const std::string& caption, int button_icon)
    : Button(parent, caption, button_icon) {


    set_flags(Flags::ToggleButton | Flags::PopupButton);

    m_popup = new Popup(screen(), window(), this);
    m_popup->set_size(Vector2i(320, 250));
    m_popup->set_visible(false);
    set_side(Popup::Right);

    m_icon_extra_scale = 0.5f; // widget override
}

PopupButton::~PopupButton()
{
	// m_popup must be manually disposed
	// since it isn't in the widget tree
	// WARNING: set_pushed has caused crashes in the past
	//this->set_pushed(false);
	this->m_popup->dispose();
}

void PopupButton::set_pushed(bool pushed) {
    m_pushed = pushed;
    if (!m_popup)
        return;

    // Hide immediately. Waiting until draw() left the popup's focused child
    // on screen for the focus-ring pass, which strokes the ring after the
    // popup is gone.
    m_popup->set_visible(pushed);

    Screen* scr = screen();
    if (!scr)
        return;

    // Track the popup panel itself (not this button) with the screen, so
    // outside-click detection can check the panel's own (screen-absolute)
    // bounds rather than this button's.
    if (pushed) {
        scr->set_popup_visible(m_popup);
        return;
    }

    scr->remove_popup_visible(m_popup);

    if (scr->m_focus_path.empty())
        return;
    for (Widget* w = scr->m_focus_path.front(); w; w = w->parent()) {
        if (w == m_popup) {
            scr->update_focus(nullptr);
            break;
        }
    }
}

Vector2i PopupButton::preferred_size(NVGcontext* ctx) const {
    return Button::preferred_size(ctx) + Vector2i(15, 0);
}

void PopupButton::draw(NVGcontext* ctx) {
    if (!m_enabled && m_pushed) {
        // Don't call set_pushed() here: it drops focus, and this runs while
        // the screen is iterating children.
        m_pushed = false;
        if (Screen* scr = screen())
            scr->remove_popup_visible(m_popup);
    }

    m_popup->set_visible(m_pushed);
    Button::draw(ctx);

    if (m_chevron_icon) {
        auto icon = utf8(m_chevron_icon);
        NVGcolor text_color =
            m_text_color.w() == 0 ? m_theme->m_text_color : m_text_color;

        nvgFontSize(ctx, (m_font_size < 0 ? m_theme->m_button_font_size : m_font_size) * icon_scale());
        nvgFontFace(ctx, "icons");
        nvgFillColor(ctx, m_enabled ? text_color : NVGcolor(m_theme->m_disabled_text_color));
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

        float iw = nvgTextBounds(ctx, 0, 0, icon.data(), nullptr, nullptr);
        Vector2f icon_pos(0, m_pos.y() + m_size.y() * 0.5f - 1);

        if (m_popup->side() == Popup::Left)
            icon_pos[0] = m_pos.x() + 8;
        else
            icon_pos[0] = m_pos.x() + m_size.x() - iw - 8;

        nvgText(ctx, icon_pos.x(), icon_pos.y(), icon.data(), nullptr);
    }
}

void PopupButton::perform_layout(NVGcontext* ctx) {
    Widget::perform_layout(ctx);

    m_popup->perform_layout(ctx);
}

void PopupButton::set_side(Popup::Side side) {
    m_popup->set_side(side);
    if (m_popup->side() == Popup::Side::Right)
        set_chevron_icon(m_theme->m_popup_chevron_right_icon);
    else if (m_popup->side() == Popup::Side::Left)
        set_chevron_icon(m_theme->m_popup_chevron_left_icon);
    else
        set_chevron_icon(m_theme->m_text_box_down_icon);
    m_popup->set_side(side);
}

NAMESPACE_END(nanogui)
