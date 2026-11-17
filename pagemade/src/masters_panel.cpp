/*
 * pagemade/masters_panel.cpp — see masters_panel.h.
 *
 * The previews are drawn from the master's items the way the Layouts tab
 * draws its templates: white paper, gray picture frames (a hill and a sun
 * for an empty one), shapes in their colors, and text as gray lines.
 */
#include "masters_panel.h"

#include "composerview.h"
#include "dialogs.h"
#include "drawlist.h"

#include <nanogui/opengl.h>
#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/dragdrop.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/menu.h>
#include <nanogui/layout.h>
#include <nanogui/messagedialog.h>
#include <nanogui/screen.h>
#include <nanogui/scrollpanel.h>
#include <nanovg.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

using namespace nanogui;
using namespace pagemade;

namespace {

constexpr float kCellWidth = 120.f;
constexpr float kCellGap = 12.f;
constexpr float kLabelHeight = 32.f;     // the name, and how many pages use it
constexpr float kMaxAspect = 1.4f;

const NVGcolor kAmber = nvgRGB(220, 140, 20);

NVGcolor nvg_color(const pagemade::Color &c) { return nvgRGBAf(c.r, c.g, c.b, c.a); }

/* One page of a master, in its own points, at the current transform */
void draw_master_page(NVGcontext *ctx, const PageDoc &doc, const Page &page) {
    for (const Item &it : page.items) {
        if (!doc.item_visible(it))
            continue;
        nvgSave(ctx);
        nvgTransform(ctx, it.xf.a, it.xf.b, it.xf.c, it.xf.d, it.xf.e, it.xf.f);
        if (const PlacedImage *im = it.image()) {
            nvgBeginPath(ctx);
            nvgRect(ctx, 0, 0, it.w, it.h);
            nvgFillColor(ctx, im->asset ? nvgRGB(160, 170, 185) : nvgRGB(214, 214, 214));
            nvgFill(ctx);
            if (!im->asset) {
                const float s = std::min(std::fabs(it.w), std::fabs(it.h)) * 0.22f;
                const float cx = it.w * 0.5f, cy = it.h * 0.5f;
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, cx - s, cy + s * 0.6f);
                nvgLineTo(ctx, cx - s * 0.2f, cy - s * 0.3f);
                nvgLineTo(ctx, cx + s * 0.25f, cy + s * 0.2f);
                nvgLineTo(ctx, cx + s * 0.55f, cy - s * 0.05f);
                nvgLineTo(ctx, cx + s, cy + s * 0.6f);
                nvgClosePath(ctx);
                nvgFillColor(ctx, nvgRGB(190, 190, 190));
                nvgFill(ctx);
            }
        } else if (const Shape *sh = it.shape()) {
            nvgBeginPath(ctx);
            if (sh->kind == Shape::Kind::Line) {
                nvgMoveTo(ctx, 0, 0);
                nvgLineTo(ctx, it.w, it.h);
            } else if (sh->kind == Shape::Kind::Ellipse) {
                nvgEllipse(ctx, it.w * 0.5f, it.h * 0.5f, std::fabs(it.w) * 0.5f, std::fabs(it.h) * 0.5f);
            } else if (sh->kind == Shape::Kind::Path) {
                bool first = true;
                for (const PathCmd &c : shape_path(it)) {
                    if (c.kind == PathCmd::Close)
                        nvgClosePath(ctx);
                    else if (first || c.kind == PathCmd::Move)
                        nvgMoveTo(ctx, c.x, c.y);
                    else if (c.kind == PathCmd::Cubic)
                        nvgBezierTo(ctx, c.cx1, c.cy1, c.cx2, c.cy2, c.x, c.y);
                    else
                        nvgLineTo(ctx, c.x, c.y);
                    first = false;
                }
            } else {
                nvgRoundedRect(ctx, 0, 0, it.w, it.h, sh->corner_radius);
            }
            if (sh->filled()) {
                pagemade::Color c = doc.resolve(sh->fill);
                shape_gradient(doc, sh->gradient, it.w, it.h, &c);   // its middle, small
                nvgFillColor(ctx, nvg_color(c));
                nvgFill(ctx);
            }
            if (!sh->stroke.none()) {
                nvgStrokeColor(ctx, nvg_color(doc.resolve(sh->stroke.paint)));
                nvgStrokeWidth(ctx, std::max(sh->stroke.weight, 2.f));
                nvgStroke(ctx);
            }
        } else if (it.is_text()) {
            /* Gray lines down the block, as layout sheets show text */
            const float lead = std::max(10.f, std::min(18.f, std::fabs(it.h) * 0.5f));
            nvgFillColor(ctx, nvgRGB(165, 165, 165));
            int lines = std::max(1, (int) (std::fabs(it.h) / lead));
            lines = std::min(lines, 40);
            for (int i = 0; i < lines; ++i) {
                const float w = i + 1 == lines && lines > 1 ? it.w * 0.6f : it.w;
                nvgBeginPath(ctx);
                nvgRect(ctx, 0, i * lead + lead * 0.25f, w, lead * 0.5f);
                nvgFill(ctx);
            }
        } else if (it.is_vector()) {
            nvgBeginPath(ctx);
            nvgRect(ctx, 0, 0, it.w, it.h);
            nvgFillColor(ctx, nvgRGB(200, 200, 215));
            nvgFill(ctx);
        }
        nvgRestore(ctx);
    }
}

/* A small picture for the drag: paper and gray frames */
DragPreview drag_preview(const PageDoc &doc, const MasterPage *m) {
    DragPreview p;
    const float pw = std::max(1.f, doc.setup.width), ph = std::max(1.f, doc.setup.height);
    const size_t n = m ? doc.master_pages(*m).size() : 1;
    const float w = pw * (float) n;
    const float k = 72.f / std::max(w, ph);
    p.w = std::max(8, (int) std::lround(w * k));
    p.h = std::max(8, (int) std::lround(ph * k));
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
    if (m) {
        const std::vector<Page> &pages = doc.master_pages(*m);
        for (size_t s = 0; s < pages.size(); ++s)
            for (const Item &it : pages[s].items) {
                const float x = it.xf.e + pw * (float) s, y = it.xf.f;
                fill(x, y, x + std::max(1.f / k, it.w), y + std::max(1.f / k, it.h),
                     it.is_image() ? 200 : it.is_text() ? 160 : 120);
            }
    }
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            uint8_t *px = &p.rgba[((size_t) y * p.w + x) * 4];
            const bool edge = x == 0 || y == 0 || x == p.w - 1 || y == p.h - 1;
            if (edge) {
                px[0] = 220;
                px[1] = 140;
                px[2] = 20;
            }
            px[3] = 225;
        }
    p.hotspot = Vector2i(p.w / 2, p.h / 2);
    p.label = m ? m->label() : std::string("[None]");
    return p;
}

} // namespace

/* ---- The preview grid ----------------------------------------------------- */

class MastersPanel::Grid : public Widget {
public:
    Grid(Widget *parent, ComposerView *view) : Widget(parent), m_view(view) { set_live(true); }

    /* Cell 0 is [None]; cell i + 1 is masters[i] */
    int count() const { return 1 + (int) m_view->masters().size(); }
    int selected() const { return m_sel; }
    void set_selected(int i) {
        m_sel = i >= 0 && i < count() ? i : -1;
        if (screen())
            screen()->redraw();
    }
    void changed() { m_laid_width = -1.f; }

    std::function<void()> on_select;
    std::function<void()> on_activate;

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
        float vis0 = -1e30f, vis1 = 1e30f;
        if (const Widget *view = parent()) {
            vis0 = (float) -m_pos.y() - 8.f;
            vis1 = vis0 + (float) view->height() + 16.f;
        }
        for (size_t i = 0; i < m_cells.size(); ++i) {
            const Cell &c = m_cells[i];
            if (c.label_y + kLabelHeight < vis0 || c.y > vis1)
                continue;
            draw_cell(ctx, (int) i, c);
        }
    }

private:
    struct Cell {
        float x = 0, y = 0, w = 0, h = 0;   // the paper
        float slot_x = 0, slot_w = 0;
        float label_y = 0;
    };

    const MasterPage *master_at(int i) const {
        const std::vector<MasterPage> &ms = m_view->masters();
        return i >= 1 && (size_t) i <= ms.size() ? &ms[(size_t) i - 1] : nullptr;
    }
    size_t sides(int i) const {
        const MasterPage *m = master_at(i);
        return m ? std::max<size_t>(1, m_view->document().master_pages(*m).size()) : 1;
    }

    float content_width() const {
        return (float) (width() > 0 ? width() : (parent() ? parent()->width() - 14 : 300));
    }

    void ensure_layout() const {
        const float w = content_width();
        if (w == m_laid_width && m_cells.size() == (size_t) count())
            return;
        m_laid_width = w;
        m_cells.assign((size_t) count(), Cell());
        const PageSetup &s = m_view->document().setup;
        const float pw = std::max(1.f, s.width), ph = std::max(1.f, s.height);
        const int cols = std::max(1, (int) ((w - kCellGap) / (kCellWidth + kCellGap)));
        const float slot = std::max(40.f, (w - kCellGap * (float) (cols + 1)) / (float) cols);
        float y = kCellGap;
        for (int row0 = 0; row0 < count(); row0 += cols) {
            float row_h = 0.f;
            for (int i = row0; i < std::min(count(), row0 + cols); ++i) {
                const float aspect = ph / (pw * (float) sides(i));
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
                c.y = y + (row_h - c.h) * 0.5f;
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

    void draw_cell(NVGcontext *ctx, int i, const Cell &c) {
        const PageDoc &doc = m_view->document();
        const MasterPage *m = master_at(i);
        const bool sel = i == m_sel, hover = i == m_hover;
        const bool editing = m && doc.editing_master == m->id;
        const float x = m_pos.x() + c.x, y = m_pos.y() + c.y;
        if (sel || hover || editing) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, m_pos.x() + c.slot_x - 5.f, y - 5.f, c.slot_w + 10.f,
                           c.label_y - c.y + kLabelHeight + 8.f, 5.f);
            nvgFillColor(ctx, editing ? nvgRGBA(220, 140, 20, 70)
                              : sel   ? nvgRGBA(70, 130, 255, 60)
                                      : nvgRGBA(255, 255, 255, 28));
            nvgFill(ctx);
        }
        nvgBeginPath(ctx);
        nvgRect(ctx, x + 1.5f, y + 2.f, c.w, c.h);
        nvgFillColor(ctx, nvgRGBA(0, 0, 0, 50));
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, c.w, c.h);
        nvgFillColor(ctx, nvgRGB(255, 255, 255));
        nvgFill(ctx);

        const size_t n = sides(i);
        const float pw = std::max(1.f, doc.setup.width);
        const float k = c.w / (pw * (float) n);
        if (m) {
            const std::vector<Page> &pages = doc.master_pages(*m);
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x, y, c.w, c.h);
            for (size_t side = 0; side < pages.size(); ++side) {
                nvgSave(ctx);
                nvgTranslate(ctx, x + (float) side * pw * k, y);
                nvgScale(ctx, k, k);
                draw_master_page(ctx, doc, pages[side]);
                nvgRestore(ctx);
            }
            nvgRestore(ctx);
            if (n == 2) {                // the spine
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, x + c.w * 0.5f, y);
                nvgLineTo(ctx, x + c.w * 0.5f, y + c.h);
                nvgStrokeColor(ctx, nvgRGBA(0, 0, 0, 40));
                nvgStrokeWidth(ctx, 1.f);
                nvgStroke(ctx);
            }
        } else {
            /* [None]: a plain page with a slash */
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, x + 6.f, y + c.h - 6.f);
            nvgLineTo(ctx, x + c.w - 6.f, y + 6.f);
            nvgStrokeColor(ctx, nvgRGBA(0, 0, 0, 50));
            nvgStrokeWidth(ctx, 1.f);
            nvgStroke(ctx);
        }
        nvgBeginPath(ctx);
        nvgRect(ctx, x + 0.5f, y + 0.5f, c.w - 1.f, c.h - 1.f);
        nvgStrokeColor(ctx, editing ? kAmber : sel ? nvgRGB(70, 130, 255) : nvgRGBA(0, 0, 0, 60));
        nvgStrokeWidth(ctx, sel || editing ? 2.f : 1.f);
        nvgStroke(ctx);

        /* The prefix badge, and a star on the default */
        if (m) {
            nvgFontFace(ctx, "sans-bold");
            nvgFontSize(ctx, 12.f);
            float b[4];
            nvgTextBounds(ctx, 0, 0, m->prefix.c_str(), nullptr, b);
            const float bw = std::max(16.f, b[2] - b[0] + 8.f);
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 3.f, y + 3.f, bw, 16.f, 3.f);
            nvgFillColor(ctx, kAmber);
            nvgFill(ctx);
            nvgFillColor(ctx, nvgRGB(255, 255, 255));
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(ctx, x + 3.f + bw * 0.5f, y + 11.5f, m->prefix.c_str(), nullptr);
        }
        if ((m && doc.default_master == m->id) || (!m && doc.default_master == 0)) {
            nvgFontFace(ctx, "icons");
            nvgFontSize(ctx, 13.f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgFillColor(ctx, kAmber);
            nvgText(ctx, x + c.w - 4.f, y + 4.f, utf8(FA_STAR).data(), nullptr);
        }

        nvgSave(ctx);
        nvgIntersectScissor(ctx, m_pos.x() + c.slot_x, m_pos.y() + c.label_y, c.slot_w, kLabelHeight);
        nvgFontFace(ctx, sel ? "sans-bold" : "sans");
        nvgFontSize(ctx, 13.f);
        nvgFillColor(ctx, m_theme ? m_theme->m_text_color : nvgRGB(220, 220, 220));
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        const std::string name = m ? m->label() : std::string("[None]");
        const float cx = m_pos.x() + c.slot_x + c.slot_w * 0.5f;
        nvgText(ctx, cx, m_pos.y() + c.label_y + 1.f, name.c_str(), nullptr);
        const size_t uses = m ? m_view->master_use_count(m->id) : m_view->master_use_count(0);
        std::string sub = std::to_string(uses) + (uses == 1 ? " page" : " pages");
        if (editing)
            sub = "editing";
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 11.f);
        nvgFillColor(ctx, editing ? kAmber : m_theme ? m_theme->m_disabled_text_color
                                                     : nvgRGB(150, 150, 150));
        nvgText(ctx, cx, m_pos.y() + c.label_y + 16.f, sub.c_str(), nullptr);
        nvgRestore(ctx);
    }

    void begin_drag(int i) {
        if (!screen() || i < 0 || i >= count())
            return;
        const MasterPage *m = master_at(i);
        DragData data;
        data.set(kMasterPageDragType, std::to_string(m ? m->id : 0u));
        screen()->start_drag(this, std::move(data), drag_preview(m_view->document(), m), DropCopy);
    }

    ComposerView *m_view = nullptr;
    mutable std::vector<Cell> m_cells;
    mutable float m_laid_width = -1.f, m_total_h = 0.f;
    int m_sel = -1, m_hover = -1, m_press = -1;
    Vector2i m_press_pos;
    int m_last_click = -1;
    double m_last_time = 0;
};

/* ---- The panel ------------------------------------------------------------- */

MastersPanel::MastersPanel(Widget *parent, ComposerView *view) : ToolPanel(parent), m_view(view) {
    auto *flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                AlignItems::Stretch, 8, 6);
    set_layout(flex);

    /* Editing a master (either way): which, how far it reaches, Revert */
    m_banner_box = new Widget(this);
    m_banner_box->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Minimum, 0, 2));
    m_banner = new Label(m_banner_box, "", "sans-bold", 14);
    m_banner->set_color(nanogui::Color(230, 150, 30, 255));
    m_banner_where = new Label(m_banner_box, "", "sans", 13);
    m_banner_where->set_color(nanogui::Color(230, 150, 30, 255));
    m_banner_info = new Label(m_banner_box, "", "sans", 13);
    m_banner_hint = new Label(m_banner_box, "", "sans", 12);
    m_banner_hint->set_color(nanogui::Color(150, 150, 150, 255));
    m_revert = new Button(m_banner_box, "Revert", FA_UNDO);
    m_revert->set_tooltip("Put back everything since you began editing this master, "
                          "and stop editing it");
    m_revert->set_callback([this] {
        m_view->revert_master_mode();
        refresh();
    });
    m_banner_box->set_visible(false);

    m_scroll = new ScrollPanel(this, ScrollPanel::ScrollTypes::Vertical);
    flex->set_flex_item(m_scroll, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_grid = new Grid(m_scroll, view);
    m_grid->on_select = [this] { selection_changed(); };
    m_grid->on_activate = [this] {
        if (chosen())
            edit_or_done();
    };

    m_info = new Label(this, "", "sans", 13);
    m_info->set_live(true);

    /* The buttons sit in one group at its natural width, centered: a wide
     * panel doesn't stretch them, a narrow one still squeezes them. */
    Widget *buttons = new Widget(this);
    buttons->set_layout(new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                       AlignItems::Stretch, 0, 6));
    buttons->set_min_size(Vector2i(1, 0));   // min:auto would stop it shrinking
    FlexLayout::FlexItem centered(0.f, 1.f);
    centered.align_self = AlignItems::Center;
    flex->set_flex_item(buttons, centered);

    auto grid_row = [buttons]() {
        Widget *row = new Widget(buttons);
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
    Widget *r1 = grid_row();
    m_new = cell(r1, "New...", FA_PLUS, "A blank master page (or spread)");
    m_new->set_callback([this] { new_master(); });
    m_from_page = cell(r1, "From Page", FA_FILE_IMPORT,
                       "The page on screen (its spread, with facing pages) becomes a master; "
                       "its items move onto the master, and the page uses it");
    m_from_page->set_callback([this] { master_from_page(); });
    Widget *r2 = grid_row();
    m_edit = cell(r2, "Edit", FA_PEN, "Put the master's pages on the pasteboard to edit them");
    m_edit->set_callback([this] { edit_or_done(); });
    m_apply = cell(r2, "Apply...", FA_SIGN_IN_ALT, "Apply the chosen master to a range of pages");
    m_apply->set_callback([this] {
        bool none = false;
        const MasterId id = chosen(&none);
        if (id || none)
            apply_to_pages(id);
    });
    Widget *r3 = grid_row();
    m_default = cell(r3, "Default", FA_STAR, "New pages get the chosen master");
    m_default->set_callback([this] {
        bool none = false;
        const MasterId id = chosen(&none);
        if (id || none)
            m_view->set_default_master(id);
    });
    m_options = cell(r3, "Options...", FA_COG,
                     "Name, prefix, based on, and the master's own margins and columns");
    m_options->set_callback([this] {
        if (const MasterId id = chosen())
            master_options(id);
    });
    Widget *r4 = grid_row();
    m_numbering = cell(r4, "Numbering...", FA_LIST_OL,
                       "Numbering & Section Options for the page on screen");
    m_numbering->set_callback([this] { numbering_options(); });
    m_delete = cell(r4, "Delete", FA_TRASH_ALT, "Take the chosen master out of the publication");
    m_delete->set_callback([this] { delete_chosen(); });
    selection_changed();
}

void MastersPanel::refresh() {
    std::string sig = std::to_string(m_view->editing_master()) + "|" +
                      std::to_string(m_view->linked_editing() ? m_view->linked_page() + 1 : 0) +
                      "|" + std::to_string(m_view->default_master()) + "|";
    for (const MasterPage &m : m_view->masters())
        sig += std::to_string(m.id) + ":" + m.label() + ":" +
               std::to_string(m_view->document().master_pages(m).size()) + ";";
    if (sig != m_sig) {
        m_sig = sig;
        m_grid->changed();
        if (m_grid->selected() >= 1 + (int) m_view->masters().size())
            m_grid->set_selected(-1);
        if (Screen *s = screen())
            if (NVGcontext *ctx = s->nvg_context())
                perform_layout(ctx);
    }
    selection_changed();
    if (screen())
        screen()->redraw();
}

MasterId MastersPanel::chosen(bool *none) const {
    const int i = m_grid ? m_grid->selected() : -1;
    if (none)
        *none = i == 0;
    const std::vector<MasterPage> &ms = m_view->masters();
    return i >= 1 && (size_t) i <= ms.size() ? ms[(size_t) i - 1].id : 0;
}

void MastersPanel::selection_changed() {
    bool none = false;
    const MasterId id = chosen(&none);
    const MasterId editing = m_view->mode_master();   // either way of editing one
    const PageDoc &doc = m_view->document();
    m_edit->set_enabled(id != 0 || editing != 0);
    m_edit->set_caption(editing && (id == editing || !id) ? "Done" : "Edit");
    m_edit->set_icon(editing && (id == editing || !id) ? FA_CHECK : FA_PEN);
    m_apply->set_enabled((id != 0 || none) && !editing);
    m_default->set_enabled((id != 0 || none) && doc.default_master != id);
    m_options->set_enabled(id != 0);
    /* [None] isn't a master, but "deleting" it moves its pages to one */
    m_delete->set_enabled(id != 0 || (none && !editing && !m_view->masters().empty()));
    m_delete->set_tooltip(none ? "Pages without a master use a master you choose instead "
                                 "([None] stays, to take a page's master away)"
                               : "Take the chosen master out of the publication");
    m_from_page->set_enabled(!editing);
    m_numbering->set_enabled(!editing);
    if (editing) {
        const MasterPage *m = doc.find_master(editing);
        const size_t uses = m_view->master_use_count(editing);
        const std::string pages = std::to_string(uses) + (uses == 1 ? " page" : " pages");
        m_banner->set_caption("Editing master " + (m ? m->label() : std::string()));
        std::string info;
        if (m_view->linked_editing()) {
            m_banner_where->set_caption("Here, on page " + m_view->page_label(m_view->linked_page()));
            info = "Changes reach " + pages + ".";
            m_banner_hint->set_caption("The page's own items are locked; new ones join the master.");
        } else {
            m_banner_where->set_caption(m && m->spread() ? "On the pasteboard (a spread)"
                                                         : "On the pasteboard");
            info = "Used by " + pages;
            if (const MasterPage *p = m ? doc.find_master(m->based_on) : nullptr)
                info += ", based on " + p->label();
            info += ".";
            m_banner_hint->set_caption("Drag a margin guide to change it. Done (or Esc) ends.");
        }
        m_banner_info->set_caption(info);
        m_revert->set_enabled(m_view->can_revert_master_mode());
        m_banner_box->set_visible(true);
    } else {
        m_banner_box->set_visible(false);
    }
    if (const MasterPage *m = doc.find_master(id)) {
        std::string info = m->spread() ? "Spread" : "Single page";
        const size_t uses = m_view->master_use_count(id);
        info += ", " + std::to_string(uses) + (uses == 1 ? " page" : " pages");
        if (const MasterPage *p = doc.find_master(m->based_on))
            info += ", based on " + p->label();
        if (m->own_margins)
            info += ", own margins";
        m_info->set_caption(info);
    } else if (none) {
        m_info->set_caption("[None]: pages without a master");
    } else {
        m_info->set_caption(m_view->masters().empty()
                                ? "New... or From Page makes a master."
                                : "Drag a master onto a page (Shift: its spread).");
    }
}

void MastersPanel::edit_or_done() {
    const MasterId id = chosen();
    const MasterId editing = m_view->mode_master();
    if (editing && (id == editing || !id))
        m_view->finish_master_edit();    // either way of editing it
    else if (id)
        m_view->edit_master(id);
    refresh();
}

std::vector<std::pair<MasterId, std::string>> MastersPanel::parents_for(MasterId id) const {
    /* Not itself, nor a master already based on it (that would loop) */
    const PageDoc &doc = m_view->document();
    std::vector<std::pair<MasterId, std::string>> out;
    for (const MasterPage &m : doc.masters) {
        bool loops = m.id == id;
        for (MasterId up = m.based_on, steps = 0; up && !loops && steps < 64; ++steps) {
            loops = up == id;
            const MasterPage *u = doc.find_master(up);
            up = u ? u->based_on : 0;
        }
        if (!loops)
            out.push_back({m.id, m.label()});
    }
    return out;
}

void MastersPanel::new_master() {
    if (!screen())
        return;
    const PageDoc &doc = m_view->document();
    MasterPage init;
    init.prefix = doc.next_master_prefix();
    init.name = "Master";
    init.margin_top = doc.setup.margin_top;
    init.margin_bottom = doc.setup.margin_bottom;
    init.margin_inside = doc.setup.margin_inside;
    init.margin_outside = doc.setup.margin_outside;
    init.columns = doc.setup.columns;
    init.gutter = doc.setup.gutter;
    ask_master_options(screen(), true, init, doc.setup.facing, doc.setup.facing, parents_for(0),
                       [this](const MasterPage &m, bool spread) {
        const MasterId id = m_view->new_master(m.name, m.prefix, spread, m.based_on);
        if (!id) {
            if (screen())
                show_alert(screen(), "No room for the Master layer.",
                           "A publication has at most 8 layers; delete one first.");
            return;
        }
        if (m.own_margins)
            m_view->set_master_options(id, m);
        m_view->edit_master(id);         // a new master opens for editing
        refresh();
        const std::vector<MasterPage> &ms = m_view->masters();
        for (size_t i = 0; i < ms.size(); ++i)
            if (ms[i].id == id)
                m_grid->set_selected((int) i + 1);
        selection_changed();
    });
}

void MastersPanel::master_from_page() {
    if (!screen() || m_view->editing_master())
        return;
    const PageDoc &doc = m_view->document();
    const std::string prefix = doc.next_master_prefix();
    ask_text(screen(), "Master from Page", "Name:", "Master",
             doc.setup.facing ? "The page's spread becomes master " + prefix +
                                    "; its items move onto the master, and the pages use it."
                              : "The page becomes master " + prefix +
                                    "; its items move onto the master, and the page uses it.",
             [this, prefix](const std::string &name) {
        const MasterId id = m_view->master_from_page(name, prefix, true);
        if (!id && screen()) {
            show_alert(screen(), "No room for the Master layer.",
                       "A publication has at most 8 layers; delete one first.");
            return;
        }
        refresh();
        const std::vector<MasterPage> &ms = m_view->masters();
        for (size_t i = 0; i < ms.size(); ++i)
            if (ms[i].id == id)
                m_grid->set_selected((int) i + 1);
        selection_changed();
    });
}

void MastersPanel::master_options(MasterId id) {
    const PageDoc &doc = m_view->document();
    const MasterPage *m = doc.find_master(id);
    if (!m || !screen())
        return;
    MasterPage init = *m;
    init.pages.clear();
    if (!init.own_margins) {
        init.margin_top = doc.setup.margin_top;
        init.margin_bottom = doc.setup.margin_bottom;
        init.margin_inside = doc.setup.margin_inside;
        init.margin_outside = doc.setup.margin_outside;
        init.columns = doc.setup.columns;
        init.gutter = doc.setup.gutter;
    }
    ask_master_options(screen(), false, init, m->spread(), doc.setup.facing, parents_for(id),
                       [this, id](const MasterPage &opts, bool) {
        m_view->set_master_options(id, opts);
        refresh();
    });
}

void MastersPanel::numbering_options() {
    if (!screen() || m_view->editing_master())
        return;
    const PageDoc &doc = m_view->document();
    const size_t i = m_view->page_index();
    if (i >= doc.pages.size())
        return;
    const Page &p = doc.pages[i];
    Section init = p.section;
    if (!p.section_start) {
        /* Start from what the page shows now */
        size_t first = 0;
        const Section &now = doc.section_at(i, &first);
        init = now;
        init.start = now.start + (int) (i - first);
    }
    ask_section_options(screen(), m_view->page_label(i), p.section_start || i == 0, init,
                        [this, i](bool start, const Section &s) {
        m_view->set_section(i, start, s);
    });
}

void MastersPanel::apply_to_pages(MasterId id) {
    if (!screen() || m_view->editing_master())
        return;
    const MasterPage *m = m_view->document().find_master(id);
    const std::string what = m ? m->label() : std::string("[None]");
    ask_text(screen(), "Apply Master", "Pages:", std::to_string(m_view->page_index() + 1),
             "Apply " + what + " to these pages: numbers or page labels, ranges and "
             "lists (“1-5, 8”), or “even”, “odd”, “all”.",
             [this, id](const std::string &range) {
        const std::vector<size_t> pages = m_view->parse_page_range(range);
        if (pages.empty()) {
            if (screen())
                show_alert(screen(), "No such pages.", "“" + range + "” names no page.");
            return;
        }
        m_view->apply_master(id, pages);
        refresh();
    });
}

void MastersPanel::delete_none() {
    const std::vector<MasterPage> &ms = m_view->masters();
    if (!screen() || ms.empty() || m_view->editing_master())
        return;
    const size_t uses = m_view->master_use_count(0);
    const size_t with_content = m_view->master_pages_with_content(0);
    auto targets = std::make_shared<std::vector<MasterId>>();
    std::vector<std::string> captions;
    for (auto it = ms.rbegin(); it != ms.rend(); ++it) {   // the newest first
        targets->push_back(it->id);
        captions.push_back(it->label());
    }

    auto *d = new ModalDialog(screen(), "Replace [None]");
    Widget *body = d->body();
    const std::string pages = std::to_string(uses) + (uses == 1 ? " page" : " pages");
    Label *what = new Label(body,
                            "[None] isn't a master; it's pages without one. " +
                                (uses ? "Its " + pages + " will use the master you choose."
                                      : std::string("No page is without a master now.")) +
                                " [None] stays in the list, to take a page's master away.",
                            "sans", 15);
    what->set_fixed_size(Vector2i(400, 0));
    if (with_content) {
        Label *warn = new Label(body,
                                "Warning: " + std::to_string(with_content) + " of " + pages +
                                    (with_content == 1 ? " has" : " have") +
                                    " content. It stays, with the master's items added "
                                    "underneath (on the Master layer).",
                                "sans-bold", 14);
        warn->set_fixed_size(Vector2i(400, 0));
        warn->set_color(nanogui::Color(230, 150, 30, 255));
    }
    Widget *row = new Widget(body);
    row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    new Label(row, "Use master:", "sans", 15);
    auto *how = new Dropdown(row, captions, {}, Dropdown::ComboBox, captions.front());
    how->set_fixed_size(Vector2i(260, 28));
    auto pick = std::make_shared<int>(0);
    how->set_selected_callback([pick](int i) { *pick = i; });
    auto *dflt = new CheckBox(body, "New pages use it too (make it the default)");
    auto make_default = std::make_shared<bool>(m_view->default_master() == 0);
    dflt->set_checked(*make_default);
    dflt->set_callback([make_default](bool on) { *make_default = on; });

    d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
    d->add_button("Replace", [this, targets, pick, make_default] {
        const MasterId to =
            (*targets)[(size_t) std::clamp(*pick, 0, (int) targets->size() - 1)];
        m_view->replace_no_master(to, *make_default);
        refresh();
    }, ModalDialog::Role::Default);
    d->open();
}

void MastersPanel::delete_chosen() {
    bool none = false;
    const MasterId id = chosen(&none);
    if (none) {
        delete_none();
        return;
    }
    const MasterPage *m = m_view->document().find_master(id);
    if (!m || !screen())
        return;
    const size_t uses = m_view->master_use_count(id);
    const size_t with_content = m_view->master_pages_with_content(id);
    const std::string name = m->label();
    std::string children;                // masters based on this one
    for (const MasterPage &o : m_view->masters())
        if (o.based_on == id)
            children += (children.empty() ? "" : ", ") + o.label();
    const std::string child_note =
        children.empty() ? std::string()
                         : " Masters based on it (" + children + ") lose what they take from it.";
    if (uses == 0) {
        auto *dlg = new MessageDialog(screen(), MessageDialog::Type::Warning, "Delete Master",
                                      "Delete master “" + name + "”?" + child_note, "Delete",
                                      "Cancel", true);
        dlg->set_callback([this, id](int result) {
            if (result == 0)
                m_view->delete_master(id, false);
            refresh();
        });
        return;
    }

    /* What its pages do instead: another master (the newest first, the
     * usual reason to delete one), keep its items, or none */
    struct Choice { MasterId replacement; bool keep; std::string caption; };
    auto choices = std::make_shared<std::vector<Choice>>();
    const std::vector<MasterPage> &ms = m_view->masters();
    for (auto it = ms.rbegin(); it != ms.rend(); ++it)
        if (it->id != id)
            choices->push_back({it->id, false, "Use “" + it->label() + "” instead"});
    choices->push_back({0, true, "Keep its items as the pages' own"});
    choices->push_back({0, false, "Remove its items (no master)"});

    auto *d = new ModalDialog(screen(), "Delete Master");
    Widget *body = d->body();
    const std::string pages = std::to_string(uses) + (uses == 1 ? " page" : " pages");
    Label *what = new Label(body, "“" + name + "” is used by " + pages + "." + child_note,
                            "sans", 15);
    what->set_fixed_size(Vector2i(400, 0));
    if (with_content) {
        Label *warn = new Label(body,
                                "Warning: " + std::to_string(with_content) + " of " + pages +
                                    (with_content == 1 ? " has" : " have") +
                                    " content of its own. That content stays, but the page "
                                    "loses the master's look unless another master takes over.",
                                "sans-bold", 14);
        warn->set_fixed_size(Vector2i(400, 0));
        warn->set_color(nanogui::Color(230, 150, 30, 255));
    }
    Widget *row = new Widget(body);
    row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    new Label(row, "Its pages:", "sans", 15);
    std::vector<std::string> captions;
    for (const Choice &c : *choices)
        captions.push_back(c.caption);
    auto *how = new Dropdown(row, captions, {}, Dropdown::ComboBox, captions.front());
    how->set_fixed_size(Vector2i(300, 28));
    how->set_tooltip("Another master: the pages switch to it; items edited on a page stay.\n"
                     "Keep: the master's items stay on the pages as ordinary items.\n"
                     "Remove: the master's items go; items edited on a page stay.");
    auto pick = std::make_shared<int>(0);
    how->set_selected_callback([pick](int i) { *pick = i; });

    d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
    d->add_button("Delete", [this, id, choices, pick] {
        const Choice &c = (*choices)[(size_t) std::clamp(*pick, 0, (int) choices->size() - 1)];
        m_view->delete_master(id, c.keep, c.replacement);
        refresh();
    }, ModalDialog::Role::Default);
    d->open();
}
