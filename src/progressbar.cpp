/*
    src/progressbar.cpp -- Standard widget for visualizing progress

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/progressbar.h>
#include <nanogui/opengl.h>
#include <nanogui/theme.h>
#include <algorithm>

NAMESPACE_BEGIN(nanogui)

ProgressBar::ProgressBar(Widget *parent)
    : WidgetCRTP<ProgressBar>(parent), m_value(0.0f) {
    DebugName = m_parent->DebugName + ",PrgBar";
}

Vector2i ProgressBar::preferred_size(NVGcontext *) const {
    return Vector2i(90, 8);
}

void ProgressBar::draw(NVGcontext* ctx) {
    Widget::draw(ctx);

    float cr = std::min(m_size.y() * 0.5f, 6.f);
    float fx = (float)m_pos.x(), fy = (float)m_pos.y();
    float fw = (float)m_size.x(), fh = (float)m_size.y();

    // Recessed track
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
    nvgFillColor(ctx, m_theme ? m_theme->m_track_color : Color(0, 40));
    nvgFill(ctx);

    float value = std::min(std::max(0.0f, m_value), 1.0f);
    float bar_w = std::max(0.f, (fw - 2.f) * value);

    if (bar_w > 0.5f) {
        Color fill = m_theme ? m_theme->m_track_fill_color : Color(0, 122, 255, 255);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, fx + 1.f, fy + 1.f, bar_w, fh - 2.f, std::max(0.f, cr - 1.f));
        nvgFillColor(ctx, fill);
        nvgFill(ctx);

        // Soft specular on the filled portion
        NVGpaint wash = nvgLinearGradient(ctx, fx, fy, fx, fy + fh,
            nvgRGBA(255, 255, 255, 50), nvgRGBA(255, 255, 255, 0));
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, fx + 1.f, fy + 1.f, bar_w, fh * 0.5f, std::max(0.f, cr - 1.f));
        nvgFillPaint(ctx, wash);
        nvgFill(ctx);
    }
}

NAMESPACE_END(nanogui)
