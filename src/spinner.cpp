/*
    src/spinner.cpp -- Busy-feedback widgets: overlay spinner and
    indeterminate progress bar

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/spinner.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <algorithm>
#include <cmath>

NAMESPACE_BEGIN(nanogui)

Spinner::Spinner(Widget *parent, const std::string &message)
    : Widget(parent), m_message(message) {
    set_visible(false);
}

void Spinner::start() {
    m_spinning = true;
    m_t0 = glfwGetTime();
    set_visible(true);
    if (screen()) {
        screen()->perform_layout();
        screen()->redraw();
    }
}

void Spinner::stop() {
    m_spinning = false;
    set_visible(false);
    if (screen())
        screen()->perform_layout();
}

Vector2i Spinner::preferred_size(NVGcontext *) const {
    return Vector2i(100, 60);
}

void Spinner::draw(NVGcontext *ctx) {
    float x = (float)m_pos.x(), y = (float)m_pos.y();
    float w = (float)m_size.x(), h = (float)m_size.y();
    bool dark = screen() && screen()->theme_mode() == ThemeMode::Dark;

    /* Dim the covered widget. */
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x, y, w, h, 8.0f);
    nvgFillColor(ctx, dark ? nvgRGBA(15, 16, 20, 170)
                           : nvgRGBA(250, 250, 252, 190));
    nvgFill(ctx);

    /* Rotating arc: the sweep pulses while the whole arc spins. */
    float r = std::min(w, h) * 0.12f;
    r = std::max(10.0f, std::min(r, 28.0f));
    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f - (m_message.empty() ? 0.0f : r * 0.5f);
    double t = glfwGetTime() - m_t0;
    float a0 = (float)(t * 4.2);
    float sweep = NVG_PI * (0.6f + 0.4f *
                  (1.0f + (float)std::sin(t * 3.0)) * 0.5f) * 2.0f;

    nvgBeginPath(ctx);
    nvgArc(ctx, cx, cy, r, a0, a0 + sweep, NVG_CW);
    nvgStrokeColor(ctx, nvgRGBA(0, 122, 255, 255));
    nvgStrokeWidth(ctx, std::max(2.0f, r * 0.16f));
    nvgLineCap(ctx, NVG_ROUND);
    nvgStroke(ctx);

    if (!m_message.empty()) {
        nvgFontSize(ctx, 15.0f);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, dark ? nvgRGBA(226, 227, 233, 255)
                               : nvgRGBA(40, 40, 48, 255));
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(ctx, cx, cy + r + 10.0f, m_message.c_str(), nullptr);
    }

    Widget::draw(ctx);
    if (m_spinning && screen())
        screen()->redraw();
}

IndeterminateBar::IndeterminateBar(Widget *parent) : ProgressBar(parent) {}

void IndeterminateBar::draw(NVGcontext *ctx) {
    float fx = (float)m_pos.x(), fy = (float)m_pos.y();
    float fw = (float)m_size.x(), fh = (float)m_size.y();
    float cr = std::min(fh * 0.5f, 6.0f);

    /* Recessed track (same look as ProgressBar). */
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, fx, fy, fw, fh, cr);
    nvgFillColor(ctx, m_theme ? m_theme->m_track_color : Color(0, 40));
    nvgFill(ctx);

    /* Sliding segment: smoothstep-eased left->right over ~1.4 s. */
    double t = glfwGetTime();
    float phase = (float)(t / 1.4);
    phase -= (float)std::floor(phase);
    phase = phase * phase * (3.0f - 2.0f * phase);
    float seg = fw * 0.3f;
    float sx = fx + 1.0f + (fw - 2.0f - seg) * phase;

    Color fill = m_theme ? m_theme->m_track_fill_color
                         : Color(0, 122, 255, 255);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, sx, fy + 1.0f, seg, fh - 2.0f,
                   std::max(0.0f, cr - 1.0f));
    nvgFillColor(ctx, fill);
    nvgFill(ctx);

    if (m_visible && screen())
        screen()->redraw();
}

NAMESPACE_END(nanogui)
