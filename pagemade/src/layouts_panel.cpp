/*
 * pagemade/layouts_panel.cpp — see layouts_panel.h.
 *
 * The previews are drawn straight from the templates, the way the sample
 * sheets show layouts: white paper, gray picture frames, the text in its
 * size where it is big enough to read and as gray lines where it isn't,
 * and the SVG ornaments through the page renderer.
 */
#include "layouts_panel.h"

#include "composerview.h"
#include "dialogs.h"
#include "drawlist.h"
#include "render_nvg.h"
#include "vector.h"

#include <GLFW/glfw3.h>
#include <nanogui/button.h>
#include <nanogui/dragdrop.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menu.h>
#include <nanogui/messagedialog.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/scrollpanel.h>
#include <nanovg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>

using namespace nanogui;
using namespace pagemade;

namespace {

constexpr float kCellWidth = 140.f;      // previews aim for this, then share the row
constexpr float kCellGap = 12.f;
constexpr float kLabelHeight = 18.f;
constexpr float kMaxAspect = 1.4f;       // a tall preview is held to this height

const char *kTurns[] = {"0°", "90°", "180°", "270°"};

std::string upper(std::string s) {
    for (char &c : s)
        c = (char) std::toupper((unsigned char) c);
    return s;
}

std::string paragraph_text(const LayoutParagraph &p) {
    std::string s;
    for (const LayoutRun &r : p.runs)
        s += r.text;
    return s;
}

/* What a paragraph looks like: its style's type with the template's
 * overrides (the first run's, for the paragraph as a whole). */
struct ParaLook {
    float size = 12.f, leading = 14.4f, tracking = 0.f;
    float before = 0.f, after = 0.f;
    bool bold = false, caps = false;
    Align align = Align::Left;
};

ParaLook look_of(const PageDoc &doc, const LayoutParagraph &p) {
    ParaLook l;
    const StyleDef *def = doc.find_style(p.style);
    if (!def)
        def = doc.find_style("Normal");
    float leading = 0.f;
    if (def) {
        l.size = def->type.size;
        leading = def->type.leading;
        l.tracking = def->type.tracking;
        l.bold = def->type.bold;
        l.caps = def->type.caps == Caps::All;
        l.align = def->para.align;
        l.before = def->para.space_before;
        l.after = def->para.space_after;
    }
    for (const LayoutRun *r : {&p.type, p.runs.empty() ? &p.type : &p.runs[0]}) {
        if (r->size > 0.f) l.size = r->size;
        if (r->leading >= 0.f) leading = r->leading;
        if (r->has_tracking) l.tracking = r->tracking;
        if (r->has_bold) l.bold = r->bold;
        if (r->has_caps) l.caps = r->caps == Caps::All;
    }
    if (p.has_align) l.align = p.align;
    if (p.space_before >= 0.f) l.before = p.space_before;
    if (p.space_after >= 0.f) l.after = p.space_after;
    l.leading = leading > 0.f ? leading : l.size * 1.2f;
    return l;
}

int nvg_align(Align a) {
    switch (a) {
    case Align::Center: case Align::JustifyCenter: return NVG_ALIGN_CENTER;
    case Align::Right: case Align::JustifyRight: return NVG_ALIGN_RIGHT;
    default: return NVG_ALIGN_LEFT;
    }
}

NVGcolor nvg_color(const pagemade::Color &c, float alpha = 1.f) {
    return nvgRGBAf(c.r, c.g, c.b, c.a * alpha);
}

/* A small picture of the layout for the drag: paper, frames and text areas. */
DragPreview drag_preview(const LayoutTemplate &t) {
    DragPreview p;
    const float k = 96.f / std::max(t.width, t.height);
    p.w = std::max(8, (int) std::lround(t.width * k));
    p.h = std::max(8, (int) std::lround(t.height * k));
    p.rgba.assign((size_t) p.w * p.h * 4, 255);
    auto fill = [&](float x0, float y0, float x1, float y1, uint8_t v) {
        const int ax = std::clamp((int) std::floor(std::min(x0, x1) * k), 0, p.w);
        const int bx = std::clamp((int) std::ceil(std::max(x0, x1) * k), 0, p.w);
        const int ay = std::clamp((int) std::floor(std::min(y0, y1) * k), 0, p.h);
        const int by = std::clamp((int) std::ceil(std::max(y0, y1) * k), 0, p.h);
        for (int y = ay; y < by; ++y)
            for (int x = ax; x < bx; ++x) {
                uint8_t *px = &p.rgba[((size_t) y * p.w + x) * 4];
                px[0] = px[1] = px[2] = v;
            }
    };
    for (const LayoutElement &e : t.elements) {
        const LayoutRect &r = e.rect;
        switch (e.kind) {
        case LayoutElement::Kind::Photo: fill(r.x, r.y, r.x + r.w, r.y + r.h, 200); break;
        case LayoutElement::Kind::Text: fill(r.x, r.y, r.x + r.w, r.y + std::min(r.h, 6.f / k), 150); break;
        case LayoutElement::Kind::Shape:
            if (e.shape == Shape::Kind::Line)
                fill(r.x, r.y, r.x + r.w + 1.f / k, r.y + r.h + 1.f / k, 90);
            break;
        case LayoutElement::Kind::Svg: fill(r.x, r.y, r.x + r.w, r.y + r.h, 170); break;
        }
    }
    /* A gray edge, and the whole thing a little see-through */
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            uint8_t *px = &p.rgba[((size_t) y * p.w + x) * 4];
            if (x == 0 || y == 0 || x == p.w - 1 || y == p.h - 1)
                px[0] = px[1] = px[2] = 120;
            px[3] = 225;
        }
    p.hotspot = Vector2i(p.w / 2, p.h / 2);
    p.label = t.name;
    return p;
}

} // namespace

/* ---- The preview grid ----------------------------------------------------- */

class LayoutsPanel::Grid : public Widget {
public:
    Grid(Widget *parent, ComposerView *view) : Widget(parent), m_view(view) { set_live(true); }

    void set_templates(const std::vector<LayoutTemplate> *list) {
        m_list = list;
        m_laid_width = -1.f;
        m_hover = m_press = -1;
        if (m_sel >= count())
            m_sel = -1;
    }
    int selected() const { return m_sel; }
    void set_selected(int i) {
        m_sel = i >= 0 && i < count() ? i : -1;
        if (screen())
            screen()->redraw();
    }
    /* The chosen cell's top and bottom, to scroll it into view */
    bool cell_span(int i, float &top, float &bottom) const {
        ensure_layout();
        if (i < 0 || i >= (int) m_cells.size())
            return false;
        top = m_cells[(size_t) i].y;
        bottom = m_cells[(size_t) i].label_y + kLabelHeight;
        return true;
    }

    std::function<void()> on_select;     // a click chose a cell
    std::function<void()> on_activate;   // a double-click

    Vector2i preferred_size(NVGcontext *) const override {
        ensure_layout();
        return Vector2i((int) content_width(), (int) std::ceil(m_total_h));
    }

    bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (button != GLFW_MOUSE_BUTTON_1)
            return Widget::mouse_button_event(p, button, down, modifiers);
        if (!down) {
            m_press = -1;
            return true;
        }
        const int i = cell_at(p - m_pos);
        m_press = i;
        m_press_pos = p - m_pos;
        const double now = glfwGetTime();
        const bool twice = i >= 0 && i == m_last_click && now - m_last_time < 0.35;
        m_last_click = i;
        m_last_time = now;
        if (i != m_sel) {
            m_sel = i;
            if (on_select)
                on_select();
        }
        if (twice && on_activate) {
            m_last_click = -1;
            on_activate();
        }
        if (screen())
            screen()->redraw();
        return true;
    }

    bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (m_press < 0 || !(button & (1 << GLFW_MOUSE_BUTTON_1)))
            return Widget::mouse_drag_event(p, rel, button, modifiers);
        const Vector2i d = (p - m_pos) - m_press_pos;
        if (d.x() * d.x() + d.y() * d.y() >= 16) {
            const int i = m_press;
            m_press = -1;
            m_last_click = -1;
            begin_drag(i);
        }
        return true;
    }

    bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        const int i = cell_at(p - m_pos);
        if (i != m_hover) {
            m_hover = i;
            if (screen())
                screen()->redraw();
        }
        return Widget::mouse_motion_event(p, rel, button, modifiers);
    }

    bool mouse_enter_event(const Vector2i &p, bool enter) override {
        if (!enter && m_hover >= 0) {
            m_hover = -1;
            if (screen())
                screen()->redraw();
        }
        return Widget::mouse_enter_event(p, enter);
    }

    void draw(NVGcontext *ctx) override {
        ensure_layout();
        if (!m_list || m_list->empty()) {
            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, 14.f);
            nvgFillColor(ctx, m_theme ? m_theme->m_disabled_text_color : nvgRGB(120, 120, 120));
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            nvgText(ctx, m_pos.x() + content_width() * 0.5f, m_pos.y() + 24.f,
                    "No layouts in this category", nullptr);
            return;
        }
        /* Only the rows in the scroll panel's window */
        float vis0 = -1e30f, vis1 = 1e30f;
        if (const Widget *view = parent()) {
            vis0 = (float) -m_pos.y() - 8.f;
            vis1 = vis0 + (float) view->height() + 16.f;
        }
        for (size_t i = 0; i < m_cells.size(); ++i) {
            const Cell &c = m_cells[i];
            if (c.label_y + kLabelHeight < vis0 || c.y > vis1)
                continue;
            draw_cell(ctx, (*m_list)[i], c, (int) i == m_sel, (int) i == m_hover);
        }
    }

private:
    struct Cell {
        float x = 0, y = 0, w = 0, h = 0;   // the paper
        float slot_x = 0, slot_w = 0;       // the column it sits in
        float label_y = 0;
    };

    int count() const { return m_list ? (int) m_list->size() : 0; }

    float content_width() const {
        return (float) (width() > 0 ? width() : (parent() ? parent()->width() - 14 : 300));
    }

    /* Columns as wide as fit; each row as tall as its tallest preview */
    void ensure_layout() const {
        const float w = content_width();
        if (w == m_laid_width && m_cells.size() == (size_t) count())
            return;
        m_laid_width = w;
        m_cells.assign((size_t) count(), Cell());
        const int cols = std::max(1, (int) ((w - kCellGap) / (kCellWidth + kCellGap)));
        const float slot = std::max(40.f, (w - kCellGap * (float) (cols + 1)) / (float) cols);
        float y = kCellGap;
        for (int row0 = 0; row0 < count(); row0 += cols) {
            float row_h = 0.f;
            for (int i = row0; i < std::min(count(), row0 + cols); ++i) {
                const LayoutTemplate &t = (*m_list)[(size_t) i];
                const float aspect = t.height / std::max(1.f, t.width);
                float cw = slot, ch = slot * aspect;
                if (ch > slot * kMaxAspect) {
                    ch = slot * kMaxAspect;
                    cw = ch / aspect;
                }
                Cell &c = m_cells[(size_t) i];
                c.slot_x = kCellGap + (float) (i - row0) * (slot + kCellGap);
                c.slot_w = slot;
                c.x = c.slot_x + (slot - cw) * 0.5f;
                c.w = cw;
                c.h = ch;
                row_h = std::max(row_h, ch);
            }
            for (int i = row0; i < std::min(count(), row0 + cols); ++i) {
                Cell &c = m_cells[(size_t) i];
                c.y = y + (row_h - c.h) * 0.5f;    // centered in the row
                c.label_y = y + row_h + 3.f;
            }
            y += row_h + 3.f + kLabelHeight + kCellGap;
        }
        m_total_h = y;
    }

    int cell_at(const Vector2i &local) const {
        ensure_layout();
        const float x = (float) local.x(), y = (float) local.y();
        for (size_t i = 0; i < m_cells.size(); ++i) {
            const Cell &c = m_cells[i];
            if (x >= c.slot_x && x <= c.slot_x + c.slot_w && y >= c.y - 4.f &&
                y <= c.label_y + kLabelHeight)
                return (int) i;
        }
        return -1;
    }

    void draw_cell(NVGcontext *ctx, const LayoutTemplate &t, const Cell &c, bool sel, bool hover) {
        const float x = m_pos.x() + c.x, y = m_pos.y() + c.y;
        if (sel || hover) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, m_pos.x() + c.slot_x - 5.f, y - 5.f, c.slot_w + 10.f,
                           c.label_y - c.y + kLabelHeight + 8.f, 5.f);
            nvgFillColor(ctx, sel ? nvgRGBA(70, 130, 255, 60) : nvgRGBA(255, 255, 255, 28));
            nvgFill(ctx);
        }
        /* Paper, with a little shadow */
        nvgBeginPath(ctx);
        nvgRect(ctx, x + 1.5f, y + 2.f, c.w, c.h);
        nvgFillColor(ctx, nvgRGBA(0, 0, 0, 50));
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, c.w, c.h);
        nvgFillColor(ctx, nvgRGB(255, 255, 255));
        nvgFill(ctx);
        draw_elements(ctx, t, x, y, c.w / t.width);
        if (t.spread) {                  // the spine
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, x + c.w * 0.5f, y);
            nvgLineTo(ctx, x + c.w * 0.5f, y + c.h);
            nvgStrokeColor(ctx, nvgRGBA(0, 0, 0, 40));
            nvgStrokeWidth(ctx, 1.f);
            nvgStroke(ctx);
        }
        nvgBeginPath(ctx);
        nvgRect(ctx, x + 0.5f, y + 0.5f, c.w - 1.f, c.h - 1.f);
        nvgStrokeColor(ctx, sel ? nvgRGB(70, 130, 255) : nvgRGBA(0, 0, 0, 60));
        nvgStrokeWidth(ctx, sel ? 2.f : 1.f);
        nvgStroke(ctx);

        nvgSave(ctx);
        nvgIntersectScissor(ctx, m_pos.x() + c.slot_x, m_pos.y() + c.label_y, c.slot_w, kLabelHeight);
        nvgFontFace(ctx, sel ? "sans-bold" : "sans");
        nvgFontSize(ctx, 13.f);
        nvgFillColor(ctx, m_theme ? m_theme->m_text_color : nvgRGB(220, 220, 220));
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(ctx, m_pos.x() + c.slot_x + c.slot_w * 0.5f, m_pos.y() + c.label_y + 1.f,
                t.name.c_str(), nullptr);
        nvgRestore(ctx);
    }

    void draw_elements(NVGcontext *ctx, const LayoutTemplate &t, float x, float y, float k) {
        const PageDoc *doc = m_view ? &m_view->document() : nullptr;
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x, y, t.width * k, t.height * k);
        for (const LayoutElement &e : t.elements) {
            const float rx = x + e.rect.x * k, ry = y + e.rect.y * k;
            const float rw = e.rect.w * k, rh = e.rect.h * k;
            switch (e.kind) {
            case LayoutElement::Kind::Photo:
                nvgBeginPath(ctx);
                nvgRect(ctx, rx, ry, rw, rh);
                nvgFillColor(ctx, nvgRGB(214, 214, 214));
                nvgFill(ctx);
                if (rw > 22.f && rh > 22.f) {
                    /* A landscape mark: a hill and a sun */
                    const float s = std::min(rw, rh) * 0.22f, cx = rx + rw * 0.5f, cy = ry + rh * 0.5f;
                    nvgBeginPath(ctx);
                    nvgMoveTo(ctx, cx - s, cy + s * 0.6f);
                    nvgLineTo(ctx, cx - s * 0.2f, cy - s * 0.3f);
                    nvgLineTo(ctx, cx + s * 0.25f, cy + s * 0.2f);
                    nvgLineTo(ctx, cx + s * 0.55f, cy - s * 0.05f);
                    nvgLineTo(ctx, cx + s, cy + s * 0.6f);
                    nvgClosePath(ctx);
                    nvgFillColor(ctx, nvgRGB(190, 190, 190));
                    nvgFill(ctx);
                    nvgBeginPath(ctx);
                    nvgCircle(ctx, cx + s * 0.55f, cy - s * 0.55f, s * 0.18f);
                    nvgFill(ctx);
                }
                if (!e.stroke_swatch.empty()) {
                    nvgBeginPath(ctx);
                    nvgRect(ctx, rx, ry, rw, rh);
                    nvgStrokeColor(ctx, nvgRGB(80, 80, 80));
                    nvgStrokeWidth(ctx, std::max(0.75f, e.stroke_weight * k));
                    nvgStroke(ctx);
                }
                break;
            case LayoutElement::Kind::Shape: {
                nvgBeginPath(ctx);
                if (e.shape == Shape::Kind::Line) {
                    nvgMoveTo(ctx, rx, ry);
                    nvgLineTo(ctx, rx + rw, ry + rh);
                } else if (e.shape == Shape::Kind::Ellipse) {
                    nvgEllipse(ctx, rx + rw * 0.5f, ry + rh * 0.5f, rw * 0.5f, rh * 0.5f);
                } else {
                    nvgRoundedRect(ctx, rx, ry, rw, rh, e.corner_radius * k);
                }
                auto paint = [&](const std::string &name, float tint) {
                    pagemade::Color c{0.3f, 0.3f, 0.3f, 1.f};
                    if (doc)
                        for (const Swatch &s : doc->swatches)
                            if (s.name == name)
                                c = doc->resolve(Paint{s.id, tint});
                    return nvg_color(c);
                };
                if (!e.fill_swatch.empty() && e.shape != Shape::Kind::Line) {
                    nvgFillColor(ctx, paint(e.fill_swatch, e.fill_tint));
                    nvgFill(ctx);
                }
                if (!e.stroke_swatch.empty()) {
                    nvgStrokeColor(ctx, paint(e.stroke_swatch, e.stroke_tint));
                    nvgStrokeWidth(ctx, std::max(0.75f, e.stroke_weight * k));
                    nvgStroke(ctx);
                }
                break;
            }
            case LayoutElement::Kind::Text:
                if (doc)
                    draw_text(ctx, *doc, e, rx, ry, rw, rh, k);
                break;
            case LayoutElement::Kind::Svg:
                draw_svg(ctx, e, rx, ry, rw, rh, k);
                break;
            }
        }
        nvgRestore(ctx);
    }

    /* Text in its size where that reads (a few pixels and up), else a gray
     * line per line of text, as layout sheets show it */
    void draw_text(NVGcontext *ctx, const PageDoc &doc, const LayoutElement &e, float x, float y,
                   float w, float h, float k) {
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x, y, w, h);
        float at = y;
        bool first = true;
        for (const LayoutParagraph &p : e.paragraphs) {
            const ParaLook l = look_of(doc, p);
            if (!first)
                at += l.before * k;
            first = false;
            std::string text = paragraph_text(p);
            if (l.caps)
                text = upper(text);
            const float px = l.size * k, lead = l.leading * k;
            const int align = nvg_align(l.align);
            if (px >= 4.5f) {
                nvgFontFace(ctx, l.bold ? "sans-bold" : "sans");
                nvgFontSize(ctx, px);
                nvgTextLetterSpacing(ctx, l.tracking / 1000.f * px);
                nvgTextLineHeight(ctx, lead / px);
                nvgTextAlign(ctx, align | NVG_ALIGN_TOP);
                nvgFillColor(ctx, nvgRGB(70, 70, 70));
                float b[4] = {0, 0, 0, 0};
                nvgTextBoxBounds(ctx, x, at, w, text.c_str(), nullptr, b);
                nvgTextBox(ctx, x, at, w, text.c_str(), nullptr);
                at = std::max(at + lead, b[3]);
            } else {
                /* About half an em per character */
                const float line_w = std::max(1.f, w);
                const float chars = std::max(1.f, (float) text.size());
                int lines = std::max(1, (int) std::ceil(chars * px * 0.5f / line_w));
                lines = std::min(lines, std::max(1, (int) ((y + h - at) / std::max(1.f, lead))));
                const float bar = std::max(0.8f, px * 0.55f);
                nvgFillColor(ctx, l.bold ? nvgRGB(120, 120, 120) : nvgRGB(170, 170, 170));
                for (int i = 0; i < lines; ++i) {
                    float bw = i + 1 == lines && lines > 1 ? line_w * 0.6f
                             : std::min(line_w, chars * px * 0.5f);
                    float bx = x;
                    if (align & NVG_ALIGN_CENTER)
                        bx = x + (line_w - bw) * 0.5f;
                    else if (align & NVG_ALIGN_RIGHT)
                        bx = x + line_w - bw;
                    nvgBeginPath(ctx);
                    nvgRect(ctx, bx, at + (lead - bar) * 0.5f, bw, bar);
                    nvgFill(ctx);
                    at += lead;
                }
            }
            at += l.after * k;
            if (at > y + h)
                break;
        }
        nvgTextLetterSpacing(ctx, 0.f);
        nvgTextLineHeight(ctx, 1.f);
        nvgRestore(ctx);
    }

    void draw_svg(NVGcontext *ctx, const LayoutElement &e, float x, float y, float w, float h,
                  float k) {
        auto it = m_scenes.find(e.svg);
        if (it == m_scenes.end()) {
            VectorScene scene;
            std::string err;
            if (!vector_loader().decode((const uint8_t *) e.svg.data(), e.svg.size(), &scene, &err))
                scene = VectorScene();
            it = m_scenes.emplace(e.svg, std::move(scene)).first;
        }
        const VectorScene &scene = it->second;
        if (!m_view || !(scene.width > 0.f) || !(scene.height > 0.f) || !(w > 0.f) || !(h > 0.f))
            return;
        /* Its own proportions, centered, as placing it does */
        const float g = std::min(w / scene.width, h / scene.height);
        Item item;
        item.w = scene.width * g / k;
        item.h = scene.height * g / k;
        PlacedVector pv;
        pv.w = item.w;
        pv.h = item.h;
        item.content = pv;
        DrawList list;
        append_placed_vector(list, m_view->document(), item, scene);
        nvgSave(ctx);
        nvgTranslate(ctx, x + (w - scene.width * g) * 0.5f, y + (h - scene.height * g) * 0.5f);
        nvgScale(ctx, k, k);
        draw_list(ctx, list, 1.f / k);
        nvgRestore(ctx);
    }

    void begin_drag(int i) {
        if (!screen() || i < 0 || i >= count())
            return;
        const LayoutTemplate &t = (*m_list)[(size_t) i];
        DragData data;
        data.set(kLayoutTemplateDragType, t.name);
        data.object = std::shared_ptr<const LayoutTemplate>(std::make_shared<LayoutTemplate>(t));
        screen()->start_drag(this, std::move(data), drag_preview(t), DropCopy);
    }

    ComposerView *m_view = nullptr;
    const std::vector<LayoutTemplate> *m_list = nullptr;
    mutable std::vector<Cell> m_cells;
    mutable float m_laid_width = -1.f, m_total_h = 0.f;
    int m_sel = -1, m_hover = -1, m_press = -1;
    Vector2i m_press_pos;
    int m_last_click = -1;
    double m_last_time = 0;
    std::map<std::string, VectorScene> m_scenes;   // decoded once per markup
};

/* ---- The panel ------------------------------------------------------------- */

LayoutsPanel::LayoutsPanel(Widget *parent, ComposerView *view) : ToolPanel(parent), m_view(view) {
    auto *flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                AlignItems::Stretch, 8, 6);
    set_layout(flex);

    auto menu_row = [this](const char *caption) {
        Widget *row = new Widget(this);
        auto *rl = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                  AlignItems::Center, 0, 6);
        row->set_layout(rl);
        auto *label = new Label(row, caption, "sans", 14);
        label->set_fixed_size(Vector2i(62, 0));
        return row;
    };
    Widget *r1 = menu_row("Category");
    m_category = new Dropdown(r1, std::vector<std::string>{"All"}, {}, Dropdown::ComboBox, "All");
    m_category->set_fixed_size(Vector2i(0, 26));
    m_category->set_tooltip("Show the layouts of one kind");
    static_cast<FlexLayout *>(r1->layout())->set_flex_item(m_category, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_category->set_selected_callback([this](int) { apply_filter(); });

    Widget *r2 = menu_row("Rotate");
    m_rotation = new Dropdown(r2, std::vector<std::string>(std::begin(kTurns), std::end(kTurns)), {},
                              Dropdown::ComboBox, kTurns[0]);
    m_rotation->set_fixed_size(Vector2i(0, 26));
    m_rotation->set_tooltip("Turn the layouts clockwise; what's in each frame stays upright");
    static_cast<FlexLayout *>(r2->layout())->set_flex_item(m_rotation, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_rotation->set_selected_callback([this](int i) {
        m_turns = std::clamp(i, 0, 3);
        apply_filter();
    });

    m_scroll = new ScrollPanel(this, ScrollPanel::ScrollTypes::Vertical);
    flex->set_flex_item(m_scroll, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_grid = new Grid(m_scroll, view);
    m_grid->on_select = [this] { selection_changed(); };
    m_grid->on_activate = [this] { place_chosen(); };

    m_info = new Label(this, "Drag a layout onto a page, or double-click it.", "sans", 13);
    m_info->set_live(true);

    auto grid_row = [this]() {
        Widget *row = new Widget(this);
        row->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                       AlignItems::Stretch, 0, 6));
        return row;
    };
    auto cell = [](Widget *row, const char *caption, int icon, const char *tip) {
        auto *b = new Button(row, caption, icon);
        b->set_tooltip(tip);
        static_cast<FlexLayout *>(row->layout())->set_flex_item(b, FlexLayout::FlexItem(1.f, 1.f, 0));
        return b;
    };
    Widget *b1 = grid_row();
    m_place = cell(b1, "Place", FA_TH_LARGE,
                   "Put the chosen layout on the page on screen, inside its margins");
    m_place->set_callback([this] { place_chosen(); });
    auto *save = cell(b1, "Save Page...", FA_SAVE,
                      "Keep the selected items (or the whole page) as a new layout");
    save->set_callback([this] { save_page(); });
    Widget *b2 = grid_row();
    m_remove = cell(b2, "Remove", FA_MINUS, "Take the chosen layout out of your layouts");
    m_remove->set_callback([this] { remove_chosen(); });
    auto *reload = cell(b2, "Reload", FA_SYNC_ALT,
                        "Read layouts.json again (delete that file to get the built-in set back)");
    reload->set_callback([this] {
        m_loaded = false;
        load();
    });
    selection_changed();
}

void LayoutsPanel::shown() {
    if (!m_loaded)
        load();
    refresh();
}

void LayoutsPanel::refresh() {
    /* Previews resolve paragraph styles and swatches in the open
     * publication: redraw them */
    if (screen())
        screen()->redraw();
}

void LayoutsPanel::load() {
    m_path = layout_templates_config_path();
    std::string err;
    m_all = layout_templates_load_or_seed(m_path, &err);
    m_loaded = true;
    rebuild_categories();
    apply_filter();
    if (!err.empty() && screen())
        show_alert(screen(), "Your layouts couldn't be read.",
                   err + "\n\nThe built-in layouts are shown instead; the file is left as it is.");
}

void LayoutsPanel::rebuild_categories() {
    const int sel = m_category->selected_index();
    const std::string keep = sel >= 0 && sel < (int) m_categories.size() ? m_categories[(size_t) sel]
                                                                         : std::string("All");
    m_categories = {"All"};
    for (const LayoutTemplate &t : m_all)
        if (!t.category.empty() &&
            std::find(m_categories.begin(), m_categories.end(), t.category) == m_categories.end())
            m_categories.push_back(t.category);
    m_category->set_items(m_categories);
    int at = 0;
    for (size_t i = 0; i < m_categories.size(); ++i)
        if (m_categories[i] == keep)
            at = (int) i;
    m_category->set_selected_index(at);
}

void LayoutsPanel::apply_filter() {
    const LayoutTemplate *was = chosen();
    const std::string keep = was ? was->name : std::string();
    const int sel = m_category->selected_index();
    const std::string cat = sel > 0 && sel < (int) m_categories.size() ? m_categories[(size_t) sel]
                                                                       : std::string();
    m_shown.clear();
    m_turned.clear();
    for (size_t i = 0; i < m_all.size(); ++i)
        if (cat.empty() || m_all[i].category == cat) {
            m_shown.push_back(i);
            m_turned.push_back(rotate_layout(m_all[i], m_turns));
        }
    m_grid->set_templates(&m_turned);
    int again = -1;
    for (size_t i = 0; i < m_turned.size(); ++i)
        if (!keep.empty() && m_turned[i].name == keep)
            again = (int) i;
    m_grid->set_selected(again);
    selection_changed();
    relayout();
}

void LayoutsPanel::relayout() {
    if (Screen *s = screen())
        if (s->nvg_context())
            perform_layout(s->nvg_context());
    if (screen())
        screen()->redraw();
}

const LayoutTemplate *LayoutsPanel::chosen() const {
    const int i = m_grid ? m_grid->selected() : -1;
    return i >= 0 && i < (int) m_turned.size() ? &m_turned[(size_t) i] : nullptr;
}

void LayoutsPanel::selection_changed() {
    const LayoutTemplate *t = chosen();
    m_place->set_enabled(t != nullptr);
    m_remove->set_enabled(t != nullptr);
    if (!t) {
        m_info->set_caption("Drag a layout onto a page, or double-click it.");
        return;
    }
    size_t texts = 0;
    for (const LayoutElement &e : t->elements)
        texts += e.kind == LayoutElement::Kind::Text;
    const size_t photos = t->photo_count();
    std::string info = std::to_string(photos) + (photos == 1 ? " photo" : " photos");
    if (texts)
        info += ", " + std::to_string(texts) + (texts == 1 ? " text block" : " text blocks");
    if (t->spread)
        info += ", two pages";
    m_info->set_caption(info);
}

void LayoutsPanel::place_chosen() {
    const LayoutTemplate *t = chosen();
    if (!t || !m_view)
        return;
    m_view->place_layout(*t, m_view->page_index());
}

bool LayoutsPanel::save_file() {
    std::string err;
    if (layout_templates_save(m_all, m_path.empty() ? layout_templates_config_path() : m_path, &err))
        return true;
    if (screen())
        show_alert(screen(), "Your layouts couldn't be saved.", err);
    return false;
}

void LayoutsPanel::save_page() {
    if (!m_view || !screen())
        return;
    if (!m_loaded)
        load();
    const size_t page = m_view->page_index();
    const PageDoc &doc = m_view->document();
    if (page >= doc.pages.size())
        return;
    /* The selection on this page with its groups, else everything on it */
    std::vector<ItemId> ids;
    for (ItemId id : m_view->selection()) {
        size_t pg = 0;
        if (!doc.find_item(id, &pg) || pg != page)
            continue;
        for (ItemId m : doc.group_members(id))
            if (std::find(ids.begin(), ids.end(), m) == ids.end())
                ids.push_back(m);
    }
    if (ids.empty())
        for (const Item &it : doc.pages[page].items)
            ids.push_back(it.id);
    if (ids.empty()) {
        show_alert(screen(), "There's nothing to save.",
                   "Put frames, text or graphics on the page (or select some) first.");
        return;
    }
    const std::string what = m_view->selection().empty() ? "the page" : "the selection";
    ask_text(screen(), "Save Layout", "Name:", "My Layout",
             "Keeps " + what + " as a layout in the \"My Layouts\" category.",
             [this, ids, page](const std::string &name) {
        if (!m_view)
            return;
        const PageDoc &doc = m_view->document();
        if (page >= doc.pages.size())
            return;
        LayoutTemplate t = layout_from_items(doc, m_view->vector_store(), page, ids, name,
                                             "My Layouts");
        if (t.elements.empty())
            return;
        m_all.push_back(std::move(t));
        if (!save_file())
            return;
        rebuild_categories();
        apply_filter();
        /* Choose the new one where it shows */
        for (size_t i = m_turned.size(); i-- > 0;)
            if (m_shown[i] == m_all.size() - 1) {
                m_grid->set_selected((int) i);
                float top = 0, bottom = 0;
                if (m_grid->cell_span((int) i, top, bottom) && m_scroll) {
                    const float span = std::max(1.f, (float) (m_grid->height() - m_scroll->height()));
                    m_scroll->set_scroll(std::clamp(top / span, 0.f, 1.f));
                }
                break;
            }
        selection_changed();
    });
}

void LayoutsPanel::remove_chosen() {
    const int i = m_grid->selected();
    if (i < 0 || i >= (int) m_shown.size() || !screen())
        return;
    const size_t index = m_shown[(size_t) i];
    const std::string name = m_all[index].name;
    auto *dlg = new MessageDialog(screen(), MessageDialog::Type::Warning, "Remove Layout",
                                  "Remove \"" + name + "\" from your layouts?", "Remove", "Cancel", true);
    dlg->set_callback([this, index, name](int result) {
        if (result != 0 || index >= m_all.size() || m_all[index].name != name)
            return;
        m_all.erase(m_all.begin() + (long) index);
        save_file();
        rebuild_categories();
        apply_filter();
    });
}
