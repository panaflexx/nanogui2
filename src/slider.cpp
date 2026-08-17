/*
    nanogui/slider.cpp -- Fractional slider widget with mouse control

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/slider.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <algorithm>

NAMESPACE_BEGIN(nanogui)

Slider::Slider(Widget* parent)
    : WidgetCRTP<Slider>(parent), m_value(0.0f), m_range(0.f, 1.f),
    m_highlighted_range(0.f, 0.f) {
    DebugName = m_parent->DebugName + ",Slider";
    m_highlight_color = Color(255, 80, 80, 70);
}

Vector2i Slider::preferred_size(NVGcontext*) const {
    // Classic compact default (~80). Theme field min-width is for text inputs,
    // not sliders — using it here inflated form panels and example windows.
    int pref_w = 80;
    if (m_min_size.x() > 0)
        pref_w = m_min_size.x();
    int h = m_theme ? std::max(22, m_theme->m_control_height) : 22;
    if (m_min_size.y() > 0)
        h = std::max(h, m_min_size.y());
    return Vector2i(pref_w, h);
}

bool Slider::mouse_drag_event(const Vector2i& p, const Vector2i& rel, int  button, int  modifiers) {
    if (!m_enabled)
        return false;

    const float kr = (int)(m_size.y() * 0.42f), kshadow = 2;
    const float start_x = kr + kshadow + m_pos.x() - 1;
    const float width_x = m_size.x() - 2 * (kr + kshadow);

    float value = (p.x() - start_x) / width_x, old_value = m_value;
    value = value * (m_range.second - m_range.first) + m_range.first;
    m_value = std::min(std::max(value, m_range.first), m_range.second);
    if (m_callback && m_value != old_value)
        m_callback(m_value);
    return true;
}

bool Slider::mouse_button_event(const Vector2i& p, int /* button */, bool down, int /* modifiers */) {
    if (!m_enabled)
        return false;

    const float kr = (int)(m_size.y() * 0.42f), kshadow = 2;
    const float start_x = kr + kshadow + m_pos.x() - 1;
    const float width_x = m_size.x() - 2 * (kr + kshadow);

    float value = (p.x() - start_x) / width_x, old_value = m_value;
    value = value * (m_range.second - m_range.first) + m_range.first;
    m_value = std::min(std::max(value, m_range.first), m_range.second);
    if (m_callback && m_value != old_value)
        m_callback(m_value);
    if (m_final_callback && !down)
        m_final_callback(m_value);
    return true;
}

void Slider::draw(NVGcontext* ctx) {
    Vector2f center = Vector2f(m_pos) + Vector2f(m_size) * 0.5f;
    float kr = (int)(m_size.y() * 0.42f);
    float kshadow = 2.f;
    float track_h = std::max(3.f, m_size.y() * 0.18f);

    float start_x = kr + kshadow + m_pos.x();
    float width_x = m_size.x() - 2 * (kr + kshadow);
    float t = (m_range.second > m_range.first)
        ? (m_value - m_range.first) / (m_range.second - m_range.first) : 0.f;
    t = std::min(std::max(t, 0.f), 1.f);

    Vector2f knob_pos(start_x + t * width_x, center.y());

    // Track base (recessed glass)
    float ty = center.y() - track_h * 0.5f;
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, start_x, ty, width_x, track_h, track_h * 0.5f);
    Color track = m_theme->m_track_color;
    if (!m_enabled)
        track.a() *= 0.5f;
    nvgFillColor(ctx, track);
    nvgFill(ctx);

    // Filled portion (accent)
    if (t > 0.001f) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, start_x, ty, width_x * t, track_h, track_h * 0.5f);
        Color fill = m_theme->m_track_fill_color;
        if (!m_enabled)
            fill.a() *= 0.45f;
        nvgFillColor(ctx, fill);
        nvgFill(ctx);
    }

    if (m_highlighted_range.second != m_highlighted_range.first) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, start_x + m_highlighted_range.first * width_x,
            ty, width_x * (m_highlighted_range.second - m_highlighted_range.first),
            track_h, track_h * 0.5f);
        nvgFillColor(ctx, m_highlight_color);
        nvgFill(ctx);
    }

    // Soft knob shadow
    NVGpaint knob_shadow = nvgRadialGradient(
        ctx, knob_pos.x(), knob_pos.y() + 1.f, kr * 0.4f, kr + 3.f,
        Color(0, 0, 0, m_enabled ? 70 : 30), m_theme->m_transparent);
    nvgBeginPath(ctx);
    nvgCircle(ctx, knob_pos.x(), knob_pos.y() + 1.f, kr + 2.f);
    nvgFillPaint(ctx, knob_shadow);
    nvgFill(ctx);

    // White glass knob (macOS style)
    Color knob_top = m_enabled ? Color(255, 255, 255, 255) : Color(220, 220, 225, 200);
    Color knob_bot = m_enabled ? Color(235, 238, 242, 255) : Color(200, 200, 205, 180);
    NVGpaint knob = nvgLinearGradient(ctx,
        knob_pos.x(), knob_pos.y() - kr, knob_pos.x(), knob_pos.y() + kr,
        knob_top, knob_bot);

    nvgBeginPath(ctx);
    nvgCircle(ctx, knob_pos.x(), knob_pos.y(), kr);
    nvgFillPaint(ctx, knob);
    nvgFill(ctx);

    nvgBeginPath(ctx);
    nvgCircle(ctx, knob_pos.x(), knob_pos.y(), kr - 0.5f);
    nvgStrokeWidth(ctx, 1.f);
    nvgStrokeColor(ctx, m_theme->m_border_dark);
    nvgStroke(ctx);

    // Tiny specular highlight on knob
    nvgBeginPath(ctx);
    nvgCircle(ctx, knob_pos.x() - kr * 0.2f, knob_pos.y() - kr * 0.25f, kr * 0.35f);
    nvgFillColor(ctx, nvgRGBA(255, 255, 255, m_enabled ? 90 : 40));
    nvgFill(ctx);
}

NAMESPACE_END(nanogui)
