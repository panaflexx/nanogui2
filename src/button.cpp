/*
    src/button.cpp -- [Normal/Toggle/Radio/Popup] Button widget

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/button.h>
#include <nanogui/popupbutton.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <algorithm>
#include <cmath>

NAMESPACE_BEGIN(nanogui)

Button::Button(Widget* parent, const std::string& caption, int icon)
    : WidgetCRTP<Button>(parent), m_caption(caption), m_icon(icon), m_icon_position(IconPosition::LeftCentered),
    m_pushed(false), m_flags(NormalButton),
    m_background_color(Color(0, 0)), m_make_transparent(false),
    m_text_color(Color(0, 0)) {
    DebugName = m_parent->DebugName + ",Butt";
}

Vector2i Button::preferred_size(NVGcontext* ctx) const {
    int font_size = m_font_size == -1 ? m_theme->m_button_font_size : m_font_size;
    int control_h = m_theme ? m_theme->m_control_height : (font_size + 10);
    nvgFontSize(ctx, font_size);
    nvgFontFace(ctx, "sans-bold");
    float tw = nvgTextBounds(ctx, 0, 0, m_caption.c_str(), nullptr, nullptr);
    float iw = 0.0f, ih = font_size;

    if (m_icon) {
        if (nvg_is_font_icon(m_icon)) {
            ih *= icon_scale();
            nvgFontFace(ctx, "icons");
            nvgFontSize(ctx, ih);
            iw = nvgTextBounds(ctx, 0, 0, utf8(m_icon).data(), nullptr, nullptr)
                + control_h * 0.15f;
        }
        else {
            int w, h;
            ih *= 0.9f;
            nvgImageSize(ctx, m_icon, &w, &h);
            iw = w * ih / h;
        }
    }
    int height = std::max(control_h, font_size + 10);
    return Vector2i((int)(tw + iw) + 20, height);
}

bool Button::mouse_enter_event(const Vector2i& p, bool enter) {
    Widget::mouse_enter_event(p, enter);
    return true;
}

/*
bool Button::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    Widget::mouse_button_event(p, button, down, modifiers);
    // Temporarily increase the reference count of the button in case the
    //  button causes the parent window to be destructed
    ref<Button> self = this;

    if (m_enabled == 1 &&
        ((button == GLFW_MOUSE_BUTTON_1 && !(m_flags & MenuButton)) ||
            (button == GLFW_MOUSE_BUTTON_2 && (m_flags & MenuButton)))) {
        bool pushed_backup = m_pushed;
        if (down) {
            if (m_flags & RadioButton) {
                if (m_button_group.empty()) {
                    for (auto widget : parent()->children()) {
                        Button* b = dynamic_cast<Button*>(widget);
                        if (b != this && b && (b->flags() & RadioButton) && b->m_pushed) {
                            b->set_pushed(false);
                            if (b->m_change_callback)
                                b->m_change_callback(false);
                        }
                    }
                }
                else {
                    for (auto b : m_button_group) {
                        if (b != this && (b->flags() & RadioButton) && b->m_pushed) {
                            b->set_pushed(false);
                            if (b->m_change_callback)
                                b->m_change_callback(false);
                        }
                    }
                }
            }
            if (m_flags & PopupButton) {
                for (auto widget : parent()->children()) {
                    Button* b = dynamic_cast<Button*>(widget);
                    if (b != this && b && (b->flags() & PopupButton) && b->m_pushed) {
                        b->set_pushed(false);
                        if (b->m_change_callback)
                            b->m_change_callback(false);
                    }
                }
                dynamic_cast<nanogui::PopupButton*>(this)->popup()->request_focus();
            }
            if (m_flags & ToggleButton)
                set_pushed(!m_pushed);
            else
                set_pushed(true);
        }
        else if (m_pushed || (m_flags & MenuButton)) {
            if (contains(p) && m_callback)
                m_callback();
            if (m_flags & NormalButton)
                set_pushed(false);
        }
        if (pushed_backup != m_pushed && m_change_callback)
            m_change_callback(m_pushed);

        return true;
    }
    return false;
}

bool Button::keyboard_event(int key, int scancode, int action, int modifiers) {
        if (Screen().keyboard_event(key, scancode, action, modifiers))
            return true;
        //FIXME: Add pushed on spacebar, reset on escape.
        return false;
}
*/
bool Button::handle_event(bool active, bool contains_point) {
    bool pushed_backup = m_pushed;
	// Protect against self-deletion during callbacks (common cause of crashes
    // when callbacks modify the widget tree).
    ref<Button> self = this;

    if (active) {
        if (m_flags & RadioButton) {
            if (m_button_group.empty()) {
                for (auto widget : parent()->children()) {
                    Button* b = dynamic_cast<Button*>(widget);
                    if (b != this && b && (b->flags() & RadioButton) && b->m_pushed) {
                        b->set_pushed(false);
                        if (b->m_change_callback)
                            b->m_change_callback(false);
                    }
                }
            }
            else {
                for (auto b : m_button_group) {
                    if (b != this && (b->flags() & RadioButton) && b->m_pushed) {
                        b->set_pushed(false);
                        if (b->m_change_callback)
                            b->m_change_callback(false);
                    }
                }
            }
        }
        if (m_flags & PopupButton) {
            for (auto widget : parent()->children()) {
                Button* b = dynamic_cast<Button*>(widget);
                if (b != this && b && (b->flags() & PopupButton) && b->m_pushed) {
                    b->set_pushed(false);
                    if (b->m_change_callback)
                        b->m_change_callback(false);
                }
            }
            dynamic_cast<nanogui::PopupButton*>(this)->popup()->request_focus();
        }
        if (m_flags & ToggleButton)
            set_pushed(!m_pushed);
        else
            set_pushed(true);
    }
    else if (m_pushed || (m_flags & MenuButton)) {
        if (contains_point && m_callback)
            m_callback();
        if (m_flags & NormalButton)
            set_pushed(false);
    }

    if (pushed_backup != m_pushed && m_change_callback)
        m_change_callback(m_pushed);

    return true;
}

bool Button::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    Widget::mouse_button_event(p, button, down, modifiers);
    ref<Button> self = this;

    if (m_enabled == 1 &&
        ((button == GLFW_MOUSE_BUTTON_1 && !(m_flags & MenuButton)) ||
            (button == GLFW_MOUSE_BUTTON_2 && (m_flags & MenuButton)))) {
        return handle_event(down, contains(p));
    }
    return false;
}

bool Button::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Widget::keyboard_event(key, scancode, action, modifiers))
        return true;

    if (!m_enabled)
        return false;

	ref<Button> self = this;  // protect during potential callback
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_SPACE) {
            return handle_event(true, true);
        }
        else if (key == GLFW_KEY_ESCAPE) {
            if (m_pushed && (m_flags & (PopupButton | MenuButton))) {
                set_pushed(false);
                if (m_change_callback)
                    m_change_callback(false);
                return true;
            }
        }
    }
    else if (action == GLFW_RELEASE && (key == GLFW_KEY_ENTER || key == GLFW_KEY_SPACE)) {
        return handle_event(false, true);
    }

    return false;
}

void Button::draw(NVGcontext* ctx) {
    Widget::draw(ctx);

    float fx = m_pos.x() + 0.5f, fy = m_pos.y() + 0.5f;
    float fw = m_size.x() - 1.f, fh = m_size.y() - 1.f;
    // Pill-ish continuous curve: radius approaches half-height
    float cr = std::min((float)m_theme->m_button_corner_radius, fh * 0.5f);

    NVGcolor grad_top = m_make_transparent ? m_theme->m_transparent
                                           : m_theme->m_button_gradient_top_unfocused;
    NVGcolor grad_bot = m_make_transparent ? m_theme->m_transparent
                                           : m_theme->m_button_gradient_bot_unfocused;

    if (m_pushed || (m_mouse_focus && (m_flags & MenuButton))) {
        grad_top = m_theme->m_button_gradient_top_pushed;
        grad_bot = m_theme->m_button_gradient_bot_pushed;
    } else if (m_mouse_focus && m_enabled) {
        grad_top = m_theme->m_button_gradient_top_focused;
        grad_bot = m_theme->m_button_gradient_bot_focused;
    }

    bool solid_bg = m_background_color.w() != 0;
    if (solid_bg) {
        // Semantic / solid accent button — soft filled pill with specular
        Color fill(m_background_color[0], m_background_color[1],
                   m_background_color[2], m_background_color[3]);
        if (m_pushed)
            fill.a() *= 0.85f;
        else if (!m_enabled)
            fill.a() *= 0.45f;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
        nvgFillColor(ctx, fill);
        nvgFill(ctx);
        // Top specular
        NVGpaint wash = nvgLinearGradient(ctx, fx, fy, fx, fy + fh * 0.55f,
            nvgRGBA(255, 255, 255, m_pushed ? 20 : 55), nvgRGBA(255, 255, 255, 0));
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, fx + 0.5f, fy + 0.5f, fw - 1.f, fh * 0.55f, cr);
        nvgFillPaint(ctx, wash);
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, m_theme->m_glass_border);
        nvgStroke(ctx);
    } else if (!m_make_transparent || m_pushed || (m_mouse_focus && m_enabled)) {
        // Frosted glass button
        NVGpaint bg = nvgLinearGradient(ctx, fx, fy, fx, fy + fh, grad_top, grad_bot);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
        nvgFillPaint(ctx, bg);
        nvgFill(ctx);

        if (!m_make_transparent || m_mouse_focus || m_pushed) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
            nvgStrokeWidth(ctx, 1.f);
            nvgStrokeColor(ctx, m_theme->m_glass_border);
            nvgStroke(ctx);
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, fx + 0.5f, fy + 0.5f, fw - 1.f, fh - 1.f,
                           std::max(0.f, cr - 0.5f));
            nvgStrokeWidth(ctx, 0.5f);
            nvgStrokeColor(ctx, m_theme->m_border_dark);
            nvgStroke(ctx);
        }
    }

    // Keyboard focus ring
    if (focused() && m_enabled)
        m_theme->draw_focus_ring(ctx, fx, fy, fw, fh, cr);

    int font_size = m_font_size == -1 ? m_theme->m_button_font_size : m_font_size;
    nvgFontSize(ctx, font_size);
    nvgFontFace(ctx, "sans-bold");
    float tw = nvgTextBounds(ctx, 0, 0, m_caption.c_str(), nullptr, nullptr);

    Vector2f center = Vector2f(m_pos) + Vector2f(m_size) * 0.5f;
    Vector2f text_pos(center.x() - tw * 0.5f, center.y());
    NVGcolor text_color =
        m_text_color.w() == 0 ? m_theme->m_text_color : m_text_color;
    NVGcolor icon_color = m_theme->m_icon_color;

    // Auto-contrast caption when a solid background color is set and the
    // caller has not overridden text color. Prefer light text on dark fills.
    if (m_enabled && m_text_color.w() == 0 && m_background_color.w() > 0.5f) {
        float lum = 0.2126f * m_background_color.r() +
                    0.7152f * m_background_color.g() +
                    0.0722f * m_background_color.b();
        text_color = lum < 0.55f ? (NVGcolor)m_theme->m_button_text_on_solid
                                 : (NVGcolor)m_theme->m_text_color;
        icon_color = text_color;
    }

    if (!m_enabled) {
        text_color = m_theme->m_disabled_text_color;
        icon_color = m_theme->m_disabled_icon_color;
    }

    if (m_icon) {
        auto icon = utf8(m_icon);

        float iw, ih = font_size;
        if (nvg_is_font_icon(m_icon)) {
            ih *= icon_scale();
            nvgFontSize(ctx, ih);
            nvgFontFace(ctx, "icons");
            iw = nvgTextBounds(ctx, 0, 0, icon.data(), nullptr, nullptr);
        } else {
            int w, h;
            ih *= 0.9f;
            nvgImageSize(ctx, m_icon, &w, &h);
            iw = w * ih / h;
        }
        if (m_caption != "")
            iw += m_size.y() * 0.15f;
        nvgFillColor(ctx, icon_color);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        Vector2f icon_pos = center;

        if (m_icon_position == IconPosition::LeftCentered) {
            icon_pos.x() -= (tw + iw) * 0.5f;
            text_pos.x() += iw * 0.5f;
        } else if (m_icon_position == IconPosition::RightCentered) {
            text_pos.x() -= iw * 0.5f;
            icon_pos.x() += tw * 0.5f;
        } else if (m_icon_position == IconPosition::Left) {
            icon_pos.x() = m_pos.x() + 8;
        } else if (m_icon_position == IconPosition::Right) {
            icon_pos.x() = m_pos.x() + m_size.x() - iw - 8;
        }

        if (nvg_is_font_icon(m_icon)) {
            nvgText(ctx, icon_pos.x(), icon_pos.y(), icon.data(), nullptr);
        } else {
            NVGpaint img_paint = nvgImagePattern(ctx,
                icon_pos.x(), icon_pos.y() - ih / 2, iw, ih, 0, m_icon, m_enabled ? 0.5f : 0.25f);
            nvgFillPaint(ctx, img_paint);
            nvgFill(ctx);
        }
    }

    nvgFontSize(ctx, font_size);
    nvgFontFace(ctx, "sans-bold");
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(ctx, text_color);
    nvgText(ctx, text_pos.x(), text_pos.y(), m_caption.c_str(), nullptr);
}

NAMESPACE_END(nanogui)
