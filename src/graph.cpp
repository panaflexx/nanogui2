/*
    src/graph.cpp -- Simple graph widget for showing a function plot

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/graph.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <algorithm>

NAMESPACE_BEGIN(nanogui)

Graph::Graph(Widget *parent, const std::string &caption)
    : WidgetCRTP<Graph>(parent), m_caption(caption) {
    DebugName = m_parent->DebugName + ",Grph";
    // Soft glass well; fill/stroke pick up accent when a theme is present
    m_background_color = Color(0, 0, 0, 40);
    m_fill_color = Color(10, 132, 255, 90);
    m_stroke_color = Color(10, 132, 255, 200);
    m_text_color = Color(255, 200);
}

Vector2i Graph::preferred_size(NVGcontext *) const {
    return Vector2i(180, 45);
}

void Graph::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    float cr = m_theme ? (float)m_theme->m_button_corner_radius : 6.f;
    float fx = (float)m_pos.x(), fy = (float)m_pos.y();
    float fw = (float)m_size.x(), fh = (float)m_size.y();

    Color bg = m_background_color;
    if (m_theme && m_background_color.w() < 0.01f)
        bg = m_theme->m_track_color;
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
    nvgFillColor(ctx, bg);
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, fx + 0.5f, fy + 0.5f, fw - 1.f, fh - 1.f, std::max(0.f, cr - 0.5f));
    nvgStrokeWidth(ctx, 1.f);
    nvgStrokeColor(ctx, m_theme ? m_theme->m_glass_border : Color(255, 30));
    nvgStroke(ctx);

    if (m_values.size() < 2)
        return;

    Color stroke = m_stroke_color;
    Color fill   = m_fill_color;
    if (m_theme) {
        if (stroke.w() > 0.9f && stroke.r() < 0.5f)
            stroke = m_theme->m_accent_color;
        if (fill.a() < 0.5f)
            fill = Color(m_theme->m_accent_color.r(), m_theme->m_accent_color.g(),
                         m_theme->m_accent_color.b(), 0.35f);
    }

    nvgSave(ctx);
    nvgIntersectScissor(ctx, fx, fy, fw, fh);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, fx, fy + fh);
    for (size_t i = 0; i < (size_t) m_values.size(); i++) {
        float value = m_values[i];
        float vx = fx + i * fw / (float) (m_values.size() - 1);
        float vy = fy + (1 - value) * fh;
        nvgLineTo(ctx, vx, vy);
    }
    nvgLineTo(ctx, fx + fw, fy + fh);
    nvgStrokeColor(ctx, stroke);
    nvgStrokeWidth(ctx, 1.5f);
    nvgStroke(ctx);
    if (fill.w() > 0) {
        nvgFillColor(ctx, fill);
        nvgFill(ctx);
    }
    nvgRestore(ctx);

    nvgFontFace(ctx, "sans");
    Color tc = m_theme ? m_theme->m_text_secondary : m_text_color;

    if (!m_caption.empty()) {
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, tc);
        nvgText(ctx, fx + 6, fy + 3, m_caption.c_str(), NULL);
    }

    if (!m_header.empty()) {
        nvgFontSize(ctx, 16.0f);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, m_theme ? m_theme->m_text_color : m_text_color);
        nvgText(ctx, fx + fw - 6, fy + 3, m_header.c_str(), NULL);
    }

    if (!m_footer.empty()) {
        nvgFontSize(ctx, 13.0f);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        nvgFillColor(ctx, tc);
        nvgText(ctx, fx + fw - 6, fy + fh - 3, m_footer.c_str(), NULL);
    }

    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
    nvgStrokeColor(ctx, Color(100, 255));
    nvgStroke(ctx);
}

NAMESPACE_END(nanogui)
