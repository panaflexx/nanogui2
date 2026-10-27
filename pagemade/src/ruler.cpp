/*
 * pagemade/ruler.cpp — see ruler.h.
 */
#include "ruler.h"

#include "composerview.h"

#include <GLFW/glfw3.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace nanogui;

namespace {

/* The rulers are window chrome: they follow View > Dark Mode (the pages
 * and pasteboard don't). `page` is the band over the page's extent. */
struct RulerColors {
    NVGcolor fill, page, edge, tick, text;
};

const RulerColors &ruler_colors(const Theme *theme) {
    static const RulerColors light{nvgRGB(236, 236, 236), nvgRGB(250, 250, 250),
                                   nvgRGB(150, 150, 150), nvgRGB(90, 90, 90), nvgRGB(50, 50, 50)};
    static const RulerColors dark{nvgRGB(44, 44, 48), nvgRGB(62, 62, 68),
                                  nvgRGB(24, 24, 26), nvgRGB(165, 165, 170), nvgRGB(215, 215, 220)};
    return theme && theme->mode() == ThemeMode::Dark ? dark : light;
}

/* Inches: the smallest step from this list that is at least `min_px` wide */
float ruler_step(float px_per_inch, float min_px) {
    static const float steps[] = {1.f / 16, 1.f / 8, 1.f / 4, 1.f / 2, 1.f, 2.f, 4.f,
                                  8.f, 16.f, 32.f, 64.f};
    for (float s : steps)
        if (s * px_per_inch >= min_px)
            return s;
    return steps[sizeof steps / sizeof steps[0] - 1];
}

bool multiple_of(double v, double step) {
    const double q = v / step;
    return std::fabs(q - std::round(q)) < 1e-4;
}

} // namespace

/* ---- Ruler --------------------------------------------------------------- */

Ruler::Ruler(Widget *parent, bool horizontal) : Widget(parent), m_horizontal(horizontal) {
    set_live(true);                      // follows every scroll and zoom
}

void Ruler::draw(NVGcontext *ctx) {
    const float x = (float) m_pos.x(), y = (float) m_pos.y();
    const float w = (float) m_size.x(), h = (float) m_size.y();
    const RulerColors &col = ruler_colors(theme());
    nvgSave(ctx);
    nvgIntersectScissor(ctx, x, y, w, h);
    nvgBeginPath(ctx);
    nvgRect(ctx, x, y, w, h);
    nvgFillColor(ctx, col.fill);
    nvgFill(ctx);

    Vector2f origin;
    float scale = 1.f;
    if (m_view && parent() && m_view->ruler_mapping(origin, scale) && scale > 0.f) {
        /* Page zero along this ruler, in drawing coordinates (the parent's) */
        const Vector2f pabs(parent()->absolute_position());
        const float zero = m_horizontal ? origin.x() - pabs.x() : origin.y() - pabs.y();
        const float ppi = 72.f * scale;  // pixels per inch
        const auto &setup = m_view->document().setup;
        const float extent = (m_horizontal ? setup.width : setup.height) * scale;
        const float lo = m_horizontal ? x : y, hi = lo + (m_horizontal ? w : h);
        const float depth = m_horizontal ? h : w;

        nvgBeginPath(ctx);
        if (m_horizontal)
            nvgRect(ctx, zero, y, extent, h);
        else
            nvgRect(ctx, x, zero, w, extent);
        nvgFillColor(ctx, col.page);
        nvgFill(ctx);

        const float label = ruler_step(ppi, 44.f);
        const float minor = std::min(label, ruler_step(ppi, 6.f));
        const long k0 = (long) std::floor((lo - zero) / (minor * ppi));
        const long k1 = (long) std::ceil((hi - zero) / (minor * ppi));

        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 10.f);
        nvgFillColor(ctx, col.text);
        nvgBeginPath(ctx);
        for (long k = k0; k <= k1; ++k) {
            const double inches = (double) k * minor;
            const float at = std::round(zero + (float) inches * ppi) + 0.5f;
            float len = depth * 0.25f;
            const bool labeled = multiple_of(inches, label);
            if (labeled)
                len = depth * 0.7f;
            else if (multiple_of(inches, label * 0.5))
                len = depth * 0.45f;
            if (m_horizontal) {
                nvgMoveTo(ctx, at, y + h);
                nvgLineTo(ctx, at, y + h - len);
            } else {
                nvgMoveTo(ctx, x + w, at);
                nvgLineTo(ctx, x + w - len, at);
            }
            if (!labeled)
                continue;
            char text[32];
            if (multiple_of(inches, 1.0))
                std::snprintf(text, sizeof text, "%ld", (long) std::lround(inches));
            else
                std::snprintf(text, sizeof text, "%g", inches);
            if (m_horizontal) {
                nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgText(ctx, at + 2.f, y + 1.f, text, nullptr);
            } else {
                /* Read bottom to top, just below the tick */
                nvgSave(ctx);
                nvgTranslate(ctx, x + 1.f, at + 2.f);
                nvgRotate(ctx, -NVG_PI * 0.5f);
                nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
                nvgText(ctx, 0, 0, text, nullptr);
                nvgRestore(ctx);
            }
        }
        nvgStrokeColor(ctx, col.tick);
        nvgStrokeWidth(ctx, 1.f);
        nvgStroke(ctx);
    }

    /* The edge against the pages */
    nvgBeginPath(ctx);
    if (m_horizontal) {
        nvgMoveTo(ctx, x, y + h - 0.5f);
        nvgLineTo(ctx, x + w, y + h - 0.5f);
    } else {
        nvgMoveTo(ctx, x + w - 0.5f, y);
        nvgLineTo(ctx, x + w - 0.5f, y + h);
    }
    nvgStrokeColor(ctx, col.edge);
    nvgStrokeWidth(ctx, 1.f);
    nvgStroke(ctx);
    nvgRestore(ctx);
}

bool Ruler::shift_down(int modifiers) const {
    if (modifiers & GLFW_MOD_SHIFT)
        return true;
    const Screen *s = screen();
    GLFWwindow *win = s ? s->glfw_window() : nullptr;
    return win && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                   glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
}

/* Points arrive relative to the parent, during drags too: the parent's
 * position gives the screen point the view measures with. */
bool Ruler::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    if (button != GLFW_MOUSE_BUTTON_1 || !m_view)
        return Widget::mouse_button_event(p, button, down, modifiers);
    if (down) {
        m_dragging = true;
        m_view->begin_guide_drag(!m_horizontal);   // the top ruler gives horizontal guides
        return true;
    }
    if (m_dragging) {
        m_dragging = false;
        m_view->end_guide_drag(p + parent()->absolute_position(), shift_down(modifiers));
    }
    return true;
}

bool Ruler::mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (!m_dragging || !m_view)
        return Widget::mouse_drag_event(p, rel, button, modifiers);
    m_view->guide_drag_to(p + parent()->absolute_position(), shift_down(modifiers));
    return true;
}

/* ---- RulerFrame ---------------------------------------------------------- */

RulerFrame::RulerFrame(Widget *parent) : Widget(parent) {
    set_live(true);
    m_top = new Ruler(this, true);
    m_left = new Ruler(this, false);
}

void RulerFrame::set_content(Widget *content, ComposerView *view) {
    m_content = content;
    m_top->set_view(view);
    m_left->set_view(view);
}

void RulerFrame::set_rulers_shown(bool shown) {
    if (m_shown == shown)
        return;
    m_shown = shown;
    if (Screen *s = screen()) {
        if (s->nvg_context())
            perform_layout(s->nvg_context());
        s->redraw();
    }
}

void RulerFrame::set_side_insets(int left, int right) {
    left = std::max(0, left);
    right = std::max(0, right);
    if (left == m_inset_left && right == m_inset_right)
        return;
    m_inset_left = left;
    m_inset_right = right;
    if (Screen *s = screen()) {
        if (s->nvg_context())
            perform_layout(s->nvg_context());
        s->redraw();
    }
}

Vector2i RulerFrame::preferred_size(NVGcontext *ctx) const {
    const int t = m_shown ? kThickness : 0;
    return (m_content ? m_content->preferred_size(ctx) : Vector2i(0, 0)) + Vector2i(t, t);
}

void RulerFrame::perform_layout(NVGcontext *ctx) {
    const int t = m_shown ? kThickness : 0;
    const int x0 = m_inset_left;
    const int w = std::max(0, size().x() - m_inset_left - m_inset_right);
    const int h = size().y();
    m_top->set_visible(m_shown);
    m_left->set_visible(m_shown);
    m_top->set_position(Vector2i(x0 + t, 0));
    m_top->set_size(Vector2i(std::max(0, w - t), t));
    m_left->set_position(Vector2i(x0, t));
    m_left->set_size(Vector2i(t, std::max(0, h - t)));
    if (m_content) {
        m_content->set_position(Vector2i(x0 + t, t));
        m_content->set_size(Vector2i(std::max(0, w - t), std::max(0, h - t)));
        m_content->perform_layout(ctx);
    }
    if (on_layout)
        on_layout();
}

void RulerFrame::draw(NVGcontext *ctx) {
    Widget::draw(ctx);
    if (!m_shown)
        return;
    /* The corner square where the rulers meet */
    const RulerColors &col = ruler_colors(theme());
    const float t = (float) kThickness;
    const float x = (float) (m_pos.x() + m_inset_left), y = (float) m_pos.y();
    nvgBeginPath(ctx);
    nvgRect(ctx, x, y, t, t);
    nvgFillColor(ctx, col.fill);
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, x + t - 0.5f, y);
    nvgLineTo(ctx, x + t - 0.5f, y + t - 0.5f);
    nvgLineTo(ctx, x, y + t - 0.5f);
    nvgStrokeColor(ctx, col.edge);
    nvgStrokeWidth(ctx, 1.f);
    nvgStroke(ctx);
}
