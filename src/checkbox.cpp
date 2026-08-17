/*
    src/checkbox.cpp -- Two-state check box widget

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/checkbox.h>
#include <nanogui/opengl.h>
#include <nanogui/theme.h>
#include <algorithm>

NAMESPACE_BEGIN(nanogui)

CheckBox::CheckBox(Widget *parent, const std::string &caption,
                   const std::function<void(bool) > &callback)
    : WidgetCRTP<CheckBox>(parent), m_caption(caption), m_pushed(false), m_checked(false),
      m_callback(callback) {
    DebugName = m_parent->DebugName + ",Check";
    m_icon_extra_scale = 1.2f; // widget override
}

bool CheckBox::mouse_button_event(const Vector2i &p, int button, bool down,
                                int modifiers) {
    Widget::mouse_button_event(p, button, down, modifiers);
    if (!m_enabled)
        return false;

    if (button == GLFW_MOUSE_BUTTON_1) {
        if (down) {
            m_pushed = true;
        } else if (m_pushed) {
            if (contains(p)) {
                m_checked = !m_checked;
                if (m_callback)
                    m_callback(m_checked);
            }
            m_pushed = false;
        }
        return true;
    }
    return false;
}

Vector2i CheckBox::preferred_size(NVGcontext *ctx) const {
    if (m_min_size != Vector2i(0))
        return m_min_size;
    nvgFontSize(ctx, font_size());
    nvgFontFace(ctx, "sans");
    return Vector2i(
        nvgTextBounds(ctx, 0, 0, m_caption.c_str(), nullptr, nullptr) +
            1.8f * font_size(),
        font_size() * 1.3f);
}

void CheckBox::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    nvgFontSize(ctx, font_size());
    nvgFontFace(ctx, "sans");
    nvgFillColor(ctx,
                 m_enabled ? m_theme->m_text_color : m_theme->m_disabled_text_color);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(ctx, m_pos.x() + 1.6f * font_size(), m_pos.y() + m_size.y() * 0.5f,
            m_caption.c_str(), nullptr);

    // macOS-style rounded square checkbox
    float box = std::min((float)m_size.y() - 2.f, font_size() * 1.15f);
    float bx = m_pos.x() + 1.f;
    float by = m_pos.y() + (m_size.y() - box) * 0.5f;
    float cr = std::max(4.f, box * 0.28f);

    if (m_checked) {
        // Filled system-blue with soft specular
        Color fill = m_enabled ? m_theme->m_accent_color
                               : Color(m_theme->m_accent_color.r(),
                                       m_theme->m_accent_color.g(),
                                       m_theme->m_accent_color.b(), 0.4f);
        if (m_pushed)
            fill.a() *= 0.85f;

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, bx, by, box, box, cr);
        nvgFillColor(ctx, fill);
        nvgFill(ctx);

        NVGpaint wash = nvgLinearGradient(ctx, bx, by, bx, by + box * 0.55f,
            nvgRGBA(255, 255, 255, 70), nvgRGBA(255, 255, 255, 0));
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, bx + 0.5f, by + 0.5f, box - 1.f, box * 0.55f, cr);
        nvgFillPaint(ctx, wash);
        nvgFill(ctx);

        // White check mark
        nvgFontSize(ctx, icon_scale() * box * 0.95f);
        nvgFontFace(ctx, "icons");
        nvgFillColor(ctx, nvgRGBA(255, 255, 255, m_enabled ? 255 : 180));
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(ctx, bx + box * 0.5f, by + box * 0.5f,
                utf8(m_theme->m_check_box_icon).data(), nullptr);
    } else {
        // Glass empty box
        Color bg = m_pushed ? Color(m_theme->m_checkbox_bg.r(),
                                    m_theme->m_checkbox_bg.g(),
                                    m_theme->m_checkbox_bg.b(),
                                    std::min(1.f, m_theme->m_checkbox_bg.a() * 1.4f))
                            : m_theme->m_checkbox_bg;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, bx, by, box, box, cr);
        nvgFillColor(ctx, bg);
        nvgFill(ctx);

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, bx + 0.5f, by + 0.5f, box - 1.f, box - 1.f, cr - 0.5f);
        nvgStrokeWidth(ctx, 1.25f);
        nvgStrokeColor(ctx, m_enabled ? m_theme->m_checkbox_border
                                      : m_theme->m_disabled_text_color);
        nvgStroke(ctx);
    }

    if (focused() && m_enabled)
        m_theme->draw_focus_ring(ctx, bx - 1.f, by - 1.f, box + 2.f, box + 2.f, cr + 1.f);
}

NAMESPACE_END(nanogui)
