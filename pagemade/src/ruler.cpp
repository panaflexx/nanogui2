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

/* A tab stop's mark at x along `base`: a tick and a foot that says which
 * way it sets (decimal adds a dot). Adds to the current path. */
void tab_mark(NVGcontext *ctx, float at, float base, pagemade::TabAlign align, float k = 1.f) {
    nvgMoveTo(ctx, at, base);
    nvgLineTo(ctx, at, base - 8.f * k);
    switch (align) {
    case pagemade::TabAlign::Left:  nvgMoveTo(ctx, at, base); nvgLineTo(ctx, at + 5.f * k, base); break;
    case pagemade::TabAlign::Right: nvgMoveTo(ctx, at, base); nvgLineTo(ctx, at - 5.f * k, base); break;
    default:                        nvgMoveTo(ctx, at - 4.f * k, base); nvgLineTo(ctx, at + 4.f * k, base); break;
    }
}

/* Indent and tab markers: blue on the light ruler, cyan on the dark one */
NVGcolor mark_color(const Theme *theme) {
    return theme && theme->mode() == ThemeMode::Dark ? nvgRGB(70, 210, 235) : nvgRGB(40, 90, 200);
}
constexpr float kSnap = 4.5f;
constexpr float kGrow = 0.7f;            // a marker in hand is drawn this much larger
constexpr double kSettle = 0.4;          // seconds it takes to settle back once let go            // markers snap to 1/16 inch
constexpr float kMarkSize = 5.f;         // half an indent triangle's width
constexpr float kMinColumn = 18.f;       // left and right indents keep this apart

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
        /* The page, or in the story editor the text column */
        const float extent = m_view->ruler_extent(m_horizontal) * scale;
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

        /* The story editor: the caret's paragraph's indents (triangles
         * along the foot: first line above, left below, right) and its tab
         * stops (a tick with a foot that says which way it sets) */
        ComposerView::RulerIndents ind;
        if (m_horizontal && m_view->ruler_indents(ind)) {
            const float base = y + h - 1.f, s = 5.f;
            /* `k` enlarges it about its tip */
            auto tri_path = [&](float px, bool down, float k) {
                const float z = s * k;
                nvgBeginPath(ctx);
                if (down) {
                    nvgMoveTo(ctx, px - z, base - s - z);
                    nvgLineTo(ctx, px + z, base - s - z);
                    nvgLineTo(ctx, px, base - s);
                } else {
                    nvgMoveTo(ctx, px - z, base);
                    nvgLineTo(ctx, px + z, base);
                    nvgLineTo(ctx, px, base - z);
                }
                nvgClosePath(ctx);
            };
            auto tri = [&](float px, bool down) {
                tri_path(px, down, 1.f);
                nvgFillColor(ctx, mark_color(theme()));
                nvgFill(ctx);
            };
            tri(zero + ind.first * scale, true);
            tri(zero + ind.left * scale, false);
            tri(zero + ind.right * scale, false);
            nvgBeginPath(ctx);
            for (const pagemade::TabStop &t : ind.tabs) {
                const float at = std::round(zero + (t.from_right ? ind.right - t.pos : t.pos) * scale) + 0.5f;
                tab_mark(ctx, at, base, t.align);
            }
            nvgStrokeColor(ctx, mark_color(theme()));
            nvgStrokeWidth(ctx, 1.5f);
            nvgStroke(ctx);
            for (const pagemade::TabStop &t : ind.tabs)
                if (t.align == pagemade::TabAlign::Decimal) {
                    const float at = std::round(zero + (t.from_right ? ind.right - t.pos : t.pos) * scale) + 0.5f;
                    nvgBeginPath(ctx);
                    nvgCircle(ctx, at + 3.f, base - 4.f, 1.2f);
                    nvgFillColor(ctx, mark_color(theme()));
                    nvgFill(ctx);
                }

            /* The marker in hand, or just let go: larger and outlined,
             * shrinking back over kSettle once dropped */
            float k = 0.f;
            if (m_flash != Mark::None) {
                if (m_mark != Mark::None)
                    k = 1.f;
                else {
                    const double t = (glfwGetTime() - m_flash_t0) / kSettle;
                    k = t >= 1.0 ? 0.f : (float) ((1.0 - t) * (1.0 - t));   // ease out
                    if (k <= 0.f)
                        m_flash = Mark::None;
                    else if (Screen *sc = screen())
                        sc->redraw();    // the next frame of the settle
                }
            }
            const bool tab_ok = m_flash != Mark::Tab ||
                                (m_flash_tab >= 0 && m_flash_tab < (int) ind.tabs.size() && !m_tab_off);
            if (k > 0.f && tab_ok) {
                const bool dark = theme() && theme()->mode() == ThemeMode::Dark;
                const NVGcolor edge = dark ? nvgRGBAf(1, 1, 1, k) : nvgRGBAf(0.05f, 0.08f, 0.2f, k);
                const NVGcolor fill = mark_color(theme());
                const float big = 1.f + kGrow * k;
                if (m_flash == Mark::Tab) {
                    const pagemade::TabStop &t = ind.tabs[(size_t) m_flash_tab];
                    const float at = std::round(zero + (t.from_right ? ind.right - t.pos : t.pos) * scale) + 0.5f;
                    nvgLineCap(ctx, NVG_ROUND);
                    nvgBeginPath(ctx);
                    tab_mark(ctx, at, base, t.align, big);
                    nvgStrokeColor(ctx, edge);
                    nvgStrokeWidth(ctx, 1.5f + 3.f * k);
                    nvgStroke(ctx);
                    nvgStrokeColor(ctx, fill);
                    nvgStrokeWidth(ctx, 1.5f + 0.8f * k);
                    nvgStroke(ctx);
                    nvgLineCap(ctx, NVG_BUTT);
                } else {
                    const float at = m_flash == Mark::First ? ind.first
                                   : m_flash == Mark::Left  ? ind.left : ind.right;
                    tri_path(zero + at * scale, m_flash == Mark::First, big);
                    nvgFillColor(ctx, fill);
                    nvgFill(ctx);
                    nvgStrokeColor(ctx, edge);
                    nvgStrokeWidth(ctx, 1.5f);
                    nvgStroke(ctx);
                }
            }
        }
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
    if (m_view->story_mode()) {
        /* No guides in the story editor; the top ruler sets the indents
         * and tabs */
        m_dragging = false;
        if (!m_horizontal)
            return true;
        if (down)
            story_press(p, modifiers);
        else
            story_release();
        return true;
    }
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
    if (m_view && m_view->story_mode()) {
        if (m_mark != Mark::None)
            story_drag(p, modifiers);
        return true;
    }
    if (!m_dragging || !m_view)
        return Widget::mouse_drag_event(p, rel, button, modifiers);
    m_view->guide_drag_to(p + parent()->absolute_position(), shift_down(modifiers));
    return true;
}

bool Ruler::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (m_view && m_view->story_mode() && m_horizontal) {
        int tab = -1;
        set_cursor(m_mark != Mark::None || mark_at(p, tab) != Mark::None ? Cursor::HResize
                                                                         : Cursor::Arrow);
    } else {
        set_cursor(Cursor::Arrow);
    }
    return Widget::mouse_motion_event(p, rel, button, modifiers);
}

/* ---- Story mode: indents and tabs ---------------------------------------- */

void Ruler::cycle_new_tab() {
    m_new_tab = (pagemade::TabAlign) (((int) m_new_tab + 1) % 4);
    if (Screen *s = screen())
        s->redraw();
}

bool Ruler::story_map(float &zero, float &scale) const {
    Vector2f origin;
    if (!m_view || !parent() || !m_view->ruler_mapping(origin, scale) || scale <= 0.f)
        return false;
    zero = origin.x() - (float) parent()->absolute_position().x();
    return true;
}

/* The nearest marker within reach. The indent triangles split the foot:
 * first line above, left (and right) below. */
Ruler::Mark Ruler::mark_at(const Vector2i &p, int &tab) const {
    tab = -1;
    float zero, scale;
    ComposerView::RulerIndents ind;
    if (!story_map(zero, scale) || !m_view->ruler_indents(ind))
        return Mark::None;
    const float px = (float) p.x(), py = (float) p.y();
    const float base = (float) m_pos.y() + (float) m_size.y() - 1.f;
    const float reach = kMarkSize + 1.f;
    Mark best = Mark::None;
    float dist = reach + 1.f;
    auto consider = [&](Mark m, float at, int i) {
        const float d = std::fabs(px - (zero + at * scale));
        if (d <= reach && d < dist) {
            best = m;
            dist = d;
            tab = i;
        }
    };
    if (py < base - kMarkSize)
        consider(Mark::First, ind.first, -1);
    else {
        consider(Mark::Left, ind.left, -1);
        consider(Mark::Right, ind.right, -1);
    }
    if (py >= base - 10.f)
        for (int i = 0; i < (int) ind.tabs.size(); ++i) {
            const pagemade::TabStop &t = ind.tabs[(size_t) i];
            consider(Mark::Tab, t.from_right ? ind.right - t.pos : t.pos, i);
        }
    if (best != Mark::Tab)
        tab = -1;
    return best;
}

void Ruler::apply_tabs(const std::vector<pagemade::TabStop> &tabs) {
    m_view->set_undo_merge(this);
    m_view->apply_para([tabs](pagemade::ParaStyle &ps) { ps.tabs = tabs; });
    m_view->set_undo_merge(nullptr);
}

bool Ruler::story_press(const Vector2i &p, int) {
    float zero, scale;
    ComposerView::RulerIndents ind;
    if (!story_map(zero, scale) || !m_view->ruler_indents(ind))
        return false;
    const float x = ((float) p.x() - zero) / scale;
    const float w = m_view->ruler_extent(true);
    int tab = -1;
    m_mark = mark_at(p, tab);
    m_tabs = ind.tabs;
    m_tab = tab;
    m_tab_off = false;
    m_view->break_undo_run();            // each drag is its own undo step
    const double now = glfwGetTime();
    const bool again = m_mark == Mark::Tab && tab == m_last_tab && now - m_last_click < 0.4;
    m_last_click = now;
    m_last_tab = m_mark == Mark::Tab ? tab : -1;
    m_flash = m_mark;
    m_flash_tab = tab;
    m_flash_t0 = now;
    switch (m_mark) {
    case Mark::First: m_grab = x - ind.first; break;
    case Mark::Left:  m_grab = x - ind.left; break;
    case Mark::Right: m_grab = x - ind.right; break;
    case Mark::Tab: {
        pagemade::TabStop &t = m_tabs[(size_t) tab];
        if (again) {
            /* Double-click: the next kind, Left > Center > Right > Decimal */
            t.align = (pagemade::TabAlign) (((int) t.align + 1) % 4);
            apply_tabs(m_tabs);
            m_mark = Mark::None;         // the new kind pulses
            m_last_tab = -1;
            return true;
        }
        m_grab = x - (t.from_right ? ind.right - t.pos : t.pos);
        break;
    }
    case Mark::None: {
        /* A click on the ruler over the column sets a new stop there */
        if (x < 0.f || x > w)
            return false;
        pagemade::TabStop t;
        t.pos = std::clamp(std::round(x / kSnap) * kSnap, 0.f, w);
        t.align = m_new_tab;
        m_tabs.push_back(t);
        m_tab = (int) m_tabs.size() - 1;
        m_mark = Mark::Tab;
        m_flash = Mark::Tab;
        m_flash_tab = m_tab;
        m_grab = x - t.pos;
        m_last_tab = -1;                 // a new stop is not a double-click
        apply_tabs(m_tabs);
        break;
    }
    }
    return true;
}

/* Shift drags the left indent alone; the first line stays put. A tab
 * dragged well off the ruler is removed (and back on, restored). */
void Ruler::story_drag(const Vector2i &p, int modifiers) {
    float zero, scale;
    ComposerView::RulerIndents ind;
    if (!story_map(zero, scale) || !m_view->ruler_indents(ind))
        return;
    const float w = m_view->ruler_extent(true);
    float x = ((float) p.x() - zero) / scale - m_grab;
    x = std::clamp(std::round(x / kSnap) * kSnap, 0.f, w);
    m_view->set_undo_merge(this);
    switch (m_mark) {
    case Mark::First:
        if (std::fabs(x - ind.first) > 0.01f)
            m_view->apply_para([x](pagemade::ParaStyle &ps) { ps.first_indent = x - ps.left_indent; });
        break;
    case Mark::Left: {
        x = std::min(x, ind.right - kMinColumn);
        if (std::fabs(x - ind.left) <= 0.01f)
            break;
        const bool alone = shift_down(modifiers);
        m_view->apply_para([x, alone](pagemade::ParaStyle &ps) {
            if (alone)
                ps.first_indent += ps.left_indent - x;
            ps.left_indent = std::max(0.f, x);
            ps.first_indent = std::max(ps.first_indent, -ps.left_indent);
        });
        break;
    }
    case Mark::Right:
        x = std::max(x, ind.left + kMinColumn);
        if (std::fabs(x - ind.right) > 0.01f)
            m_view->apply_para([r = std::max(0.f, w - x)](pagemade::ParaStyle &ps) { ps.right_indent = r; });
        break;
    case Mark::Tab: {
        if (m_tab < 0 || m_tab >= (int) m_tabs.size())
            break;
        const float mid = (float) m_pos.y() + (float) m_size.y() * 0.5f;
        const bool off = std::fabs((float) p.y() - mid) > 24.f;
        std::vector<pagemade::TabStop> tabs = m_tabs;
        if (off) {
            tabs.erase(tabs.begin() + m_tab);
        } else {
            pagemade::TabStop &t = tabs[(size_t) m_tab];
            const float pos = t.from_right ? ind.right - x : x;
            t.pos = std::max(0.f, pos);
            m_tabs[(size_t) m_tab].pos = t.pos;
        }
        auto same = [](const std::vector<pagemade::TabStop> &a,
                       const std::vector<pagemade::TabStop> &b) {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].pos != b[i].pos || a[i].align != b[i].align ||
                    a[i].leader != b[i].leader || a[i].from_right != b[i].from_right)
                    return false;
            return true;
        };
        if (!same(tabs, ind.tabs))
            apply_tabs(tabs);
        m_tab_off = off;
        break;
    }
    case Mark::None:
        break;
    }
    m_view->set_undo_merge(nullptr);
}

void Ruler::story_release() {
    if (m_mark == Mark::Tab && m_tab_off)
        m_flash = Mark::None;            // removed: nothing to settle
    m_flash_t0 = glfwGetTime();          // dropped: settle back from here
    if (Screen *s = screen())
        s->redraw();
    m_mark = Mark::None;
    m_tab = -1;
    m_tab_off = false;
}

/* ---- RulerFrame ---------------------------------------------------------- */

RulerFrame::RulerFrame(Widget *parent) : Widget(parent) {
    set_live(true);
    m_top = new Ruler(this, true);
    m_left = new Ruler(this, false);
}

void RulerFrame::set_content(Widget *content, ComposerView *view) {
    m_content = content;
    m_view = view;
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
    /* The story editor: the kind of tab a click on the ruler sets */
    if (m_view && m_view->story_mode()) {
        const pagemade::TabAlign a = m_top->new_tab();
        const float at = std::round(x + t * 0.5f) + 0.5f, base = y + t - 5.f;
        nvgBeginPath(ctx);
        tab_mark(ctx, at, base, a);
        nvgStrokeColor(ctx, mark_color(theme()));
        nvgStrokeWidth(ctx, 1.5f);
        nvgStroke(ctx);
        if (a == pagemade::TabAlign::Decimal) {
            nvgBeginPath(ctx);
            nvgCircle(ctx, at + 3.f, base - 4.f, 1.2f);
            nvgFillColor(ctx, mark_color(theme()));
            nvgFill(ctx);
        }
    }
}

bool RulerFrame::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    const Vector2i local = p - m_pos;
    const int t = kThickness;
    if (m_shown && m_view && m_view->story_mode() && button == GLFW_MOUSE_BUTTON_1 &&
        local.x() >= m_inset_left && local.x() < m_inset_left + t && local.y() >= 0 &&
        local.y() < t) {
        if (down)
            m_top->cycle_new_tab();
        return true;
    }
    return Widget::mouse_button_event(p, button, down, modifiers);
}
