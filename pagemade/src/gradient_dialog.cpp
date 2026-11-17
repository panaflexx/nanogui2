/*
 * pagemade/gradient_dialog.cpp — see gradient_dialog.h.
 */
#include "gradient_dialog.h"

#include "dialogs.h"
#include "spinbox.h"
#include "swatch_menu.h"

#include <nanogui/button.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menu.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanovg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

using namespace nanogui;

namespace pagemade {

namespace {

struct GradientState {
    PageDoc palette;                     // the swatches, for resolve()
    Gradient g;
    std::string name;
    bool auto_name = true;               // the name follows the colors
};

/* The ramp as it will fill: left to right, over a checkerboard */
class RampPreview : public Widget {
public:
    RampPreview(Widget *parent, std::shared_ptr<GradientState> st)
        : Widget(parent), m_st(std::move(st)) {
        set_fixed_size(Vector2i(420, 36));
    }

    void draw(NVGcontext *ctx) override {
        const float x = (float) m_pos.x(), y = (float) m_pos.y();
        const float w = (float) m_size.x(), h = (float) m_size.y();
        nvgSave(ctx);
        nvgScissor(ctx, x, y, w, h);
        const float cell = 6.f;
        for (int r = 0; r * cell < h; ++r)
            for (int c = 0; c * cell < w; ++c) {
                nvgBeginPath(ctx);
                nvgRect(ctx, x + c * cell, y + r * cell, cell, cell);
                nvgFillColor(ctx, (r + c) % 2 ? nvgRGB(205, 205, 205) : nvgRGB(250, 250, 250));
                nvgFill(ctx);
            }
        const Gradient &g = m_st->g;
        std::vector<GradientPoint> pts = g.points;
        std::stable_sort(pts.begin(), pts.end(),
                         [](const GradientPoint &a, const GradientPoint &b) { return a.pos < b.pos; });
        auto color = [&](const GradientPoint &p) {
            Color c = m_st->palette.resolve(p.paint);
            c.a *= std::clamp(p.opacity, 0.f, 100.f) / 100.f;
            return nvgRGBAf(c.r, c.g, c.b, c.a);
        };
        if (g.kind != Gradient::Kind::None && pts.size() >= 2) {
            /* Hold the ends, blend between points */
            auto band = [&](float x0, float x1, NVGcolor a, NVGcolor b) {
                if (x1 <= x0)
                    return;
                nvgBeginPath(ctx);
                nvgRect(ctx, x0, y, x1 - x0 + 0.5f, h);
                nvgFillPaint(ctx, nvgLinearGradient(ctx, x0, y, x1, y, a, b));
                nvgFill(ctx);
            };
            auto at = [&](const GradientPoint &p) {
                return x + w * std::clamp(p.pos, 0.f, 100.f) / 100.f;
            };
            band(x, at(pts.front()), color(pts.front()), color(pts.front()));
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                band(at(pts[i]), at(pts[i + 1]), color(pts[i]), color(pts[i + 1]));
            band(at(pts.back()), x + w, color(pts.back()), color(pts.back()));
            /* Where the points are */
            for (const GradientPoint &p : pts) {
                const float px = at(p);
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, px, y + h);
                nvgLineTo(ctx, px - 5, y + h - 8);
                nvgLineTo(ctx, px + 5, y + h - 8);
                nvgClosePath(ctx);
                nvgFillColor(ctx, nvgRGB(255, 255, 255));
                nvgFill(ctx);
                nvgStrokeWidth(ctx, 1.f);
                nvgStrokeColor(ctx, nvgRGB(0, 0, 0));
                nvgStroke(ctx);
            }
        }
        nvgResetScissor(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, x + 0.5f, y + 0.5f, w - 1, h - 1);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, nvgRGB(90, 90, 90));
        nvgStroke(ctx);
        nvgRestore(ctx);
    }

private:
    std::shared_ptr<GradientState> m_st;
};

/* One color point's controls */
struct PointRow {
    Widget *row = nullptr;
    ColorDropdown *swatch = nullptr;
    IntSpin *tint = nullptr, *pos = nullptr, *opacity = nullptr;
    Button *remove = nullptr;
};

} // namespace

void ask_gradient(Screen *screen, const std::vector<Swatch> &swatches, const Gradient &initial,
                  const std::string &name,
                  std::function<void(const std::string &, const Gradient &)> done,
                  std::function<void()> remove) {
    auto st = std::make_shared<GradientState>();
    st->palette.swatches = swatches;
    st->g = initial;
    if (st->g.kind == Gradient::Kind::None)
        st->g.kind = Gradient::Kind::Linear;
    if (st->g.points.size() < kMinGradientPoints) {
        const Gradient::Kind kind = st->g.kind;
        st->g = default_gradient(Paint{});
        st->g.kind = kind;
    }
    if (st->g.points.size() > kMaxGradientPoints)
        st->g.points.resize(kMaxGradientPoints);

    st->name = name.empty() ? gradient_auto_name(st->g, swatches) : name;
    /* A name that is the colors' (or none) follows the colors */
    st->auto_name = name.empty() || name == gradient_auto_name(initial, swatches);

    auto *d = new ModalDialog(screen, remove ? "Edit Gradient" : "New Gradient");
    Widget *body = d->body();

    Widget *name_row = new Widget(body);
    name_row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    new Label(name_row, "Name:", "sans", 15);
    auto *name_box = new TextBox(name_row, st->name);
    name_box->set_editable(true);
    name_box->set_alignment(TextBox::Alignment::Left);
    name_box->set_fixed_size(Vector2i(360, 28));
    name_box->set_tooltip("Filled in from the colors until you type your own; clear it to "
                          "go back to that");

    Widget *top = new Widget(body);
    top->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    new Label(top, "Type:", "sans", 15);
    auto *kind = new Dropdown(top, {"Linear", "Radial"}, {}, Dropdown::ComboBox, "Linear");
    kind->set_fixed_size(Vector2i(150, 26));
    kind->set_selected_index(st->g.kind == Gradient::Kind::Radial ? 1 : 0);
    new Label(top, "  Angle:", "sans", 15);
    auto *angle = new IntSpin(top, (int) std::lround(st->g.angle), 15, -360, 360);
    angle->set_units("°");
    angle->set_fixed_size(Vector2i(80, 26));
    angle->set_tooltip("The linear ramp's direction: 0° runs left to right, 90° top "
                       "to bottom");

    new RampPreview(body, st);

    Widget *head = new Widget(body);
    head->set_layout(new GridLayout(Orientation::Horizontal, 5, Alignment::Middle, 0, 6));
    auto heading = [&](const char *text, int w) {
        Label *l = new Label(head, text, "sans-bold", 14);
        l->set_fixed_size(Vector2i(w, 0));
    };
    heading("Color", 150);
    heading("Tint", 74);
    heading("Position", 74);
    heading("Opacity", 74);
    heading("", 26);

    auto rows = std::make_shared<std::vector<PointRow>>();
    Button *add = nullptr;
    std::shared_ptr<std::function<void()>> sync = std::make_shared<std::function<void()>>();

    for (size_t i = 0; i < kMaxGradientPoints; ++i) {
        PointRow r;
        r.row = new Widget(body);
        r.row->set_layout(new GridLayout(Orientation::Horizontal, 5, Alignment::Middle, 0, 6));
        r.swatch = new ColorDropdown(r.row);
        r.swatch->set_fixed_size(Vector2i(150, 26));
        r.tint = new IntSpin(r.row, 100, 5, 0, 100);
        r.tint->set_units("%");
        r.tint->set_fixed_size(Vector2i(74, 26));
        r.tint->set_tooltip("The swatch's tint");
        r.pos = new IntSpin(r.row, 0, 5, 0, 100);
        r.pos->set_units("%");
        r.pos->set_fixed_size(Vector2i(74, 26));
        r.pos->set_tooltip("Where the point sits along the ramp");
        r.opacity = new IntSpin(r.row, 100, 5, 0, 100);
        r.opacity->set_units("%");
        r.opacity->set_fixed_size(Vector2i(74, 26));
        r.opacity->set_tooltip("The point's opacity: 0% is clear");
        r.remove = new Button(r.row, "", FA_MINUS);
        r.remove->set_fixed_size(Vector2i(26, 26));
        r.remove->set_tooltip("Remove this point");

        r.tint->set_callback([st, i, sync](int v) {
            if (i < st->g.points.size())
                st->g.points[i].paint.tint = (float) v;
            (*sync)();
        });
        r.pos->set_callback([st, i, sync](int v) {
            if (i < st->g.points.size())
                st->g.points[i].pos = (float) v;
            (*sync)();
        });
        r.opacity->set_callback([st, i, sync](int v) {
            if (i < st->g.points.size())
                st->g.points[i].opacity = (float) v;
            (*sync)();
        });
        r.remove->set_callback([st, i, sync] {
            if (st->g.points.size() > kMinGradientPoints && i < st->g.points.size())
                st->g.points.erase(st->g.points.begin() + (long) i);
            (*sync)();
        });
        rows->push_back(r);
    }

    add = new Button(body, "Add point", FA_PLUS);
    add->set_tooltip("Up to four points");
    add->set_callback([st, sync] {
        std::vector<GradientPoint> &pts = st->g.points;
        if (pts.size() >= kMaxGradientPoints)
            return;
        /* Halfway along the widest gap, colored as its left end */
        std::vector<GradientPoint> sorted = pts;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const GradientPoint &a, const GradientPoint &b) { return a.pos < b.pos; });
        size_t gap = 0;
        float widest = -1.f;
        for (size_t k = 0; k + 1 < sorted.size(); ++k)
            if (sorted[k + 1].pos - sorted[k].pos > widest) {
                widest = sorted[k + 1].pos - sorted[k].pos;
                gap = k;
            }
        GradientPoint p = sorted[gap];
        p.pos = std::round((sorted[gap].pos + sorted[gap + 1].pos) * 0.5f);
        pts.push_back(p);
        (*sync)();
    });

    /* Everything follows the state: rows shown for each point, the angle
     * only for a linear ramp, the window resized to fit */
    *sync = [st, rows, add, angle, d, swatches, name_box,
             sync_weak = std::weak_ptr<std::function<void()>>(sync)] {
        const bool on = st->g.kind != Gradient::Kind::None;
        if (st->auto_name) {
            st->name = gradient_auto_name(st->g, swatches);
            if (!name_box->focused())
                name_box->set_value(st->name);
        }
        angle->set_enabled(st->g.kind == Gradient::Kind::Linear);
        bool resized = false;
        for (size_t i = 0; i < rows->size(); ++i) {
            PointRow &r = (*rows)[i];
            const bool shown = i < st->g.points.size();
            if (r.row->visible() != shown) {
                r.row->set_visible(shown);
                resized = true;
            }
            if (!shown)
                continue;
            const GradientPoint &p = st->g.points[i];
            if (const Swatch *sw = st->palette.find_swatch(p.paint.swatch))
                r.swatch->set_swatch(sw->name, sw->rgb);
            else
                r.swatch->set_none();
            if (r.swatch->popup()->visible())
                continue;                // picking from it: it closes on its own
            SwatchMenuView view;
            view.swatches = swatches;
            view.current = p.paint.swatch;
            refill_swatch_menu(
                r.swatch, view,
                [st, i, sync_weak](SwatchId id) {
                    if (i < st->g.points.size() && id != kNoPaint)
                        st->g.points[i].paint.swatch = id;
                    if (auto s = sync_weak.lock())
                        (*s)();
                },
                {}, {});
            r.tint->set_value((int) std::lround(p.paint.tint));
            r.pos->set_value((int) std::lround(p.pos));
            r.opacity->set_value((int) std::lround(p.opacity));
            r.remove->set_enabled(st->g.points.size() > kMinGradientPoints);
            for (Widget *w : {(Widget *) r.swatch, (Widget *) r.tint, (Widget *) r.pos,
                              (Widget *) r.opacity})
                w->set_enabled(on);
            if (!on)
                r.remove->set_enabled(false);
        }
        add->set_enabled(on && st->g.points.size() < kMaxGradientPoints);
        if (resized) {
            if (Screen *s = d->screen()) {
                NVGcontext *ctx = s->nvg_context();
                d->set_size(d->preferred_size(ctx));
                d->perform_layout(ctx);
            }
        }
    };
    kind->set_selected_callback([st, sync](int i) {
        st->g.kind = i == 1 ? Gradient::Kind::Radial : Gradient::Kind::Linear;
        (*sync)();
    });
    name_box->set_callback([st, swatches, sync](const std::string &v) {
        std::string t = v;
        while (!t.empty() && std::isspace((unsigned char) t.back()))
            t.pop_back();
        while (!t.empty() && std::isspace((unsigned char) t.front()))
            t.erase(t.begin());
        const std::string automatic = gradient_auto_name(st->g, swatches);
        st->auto_name = t.empty() || t == automatic;
        st->name = st->auto_name ? automatic : t;
        (*sync)();
        return true;
    });
    angle->set_callback([st, sync](int v) {
        st->g.angle = (float) v;
        (*sync)();
    });
    (*sync)();

    if (remove)
        d->add_button("Delete", [remove] { remove(); });
    d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
    d->add_button("OK", [st, done] {
        Gradient g = st->g;
        for (GradientPoint &p : g.points) {
            p.pos = std::clamp(p.pos, 0.f, 100.f);
            p.opacity = std::clamp(p.opacity, 0.f, 100.f);
            p.paint.tint = std::clamp(p.paint.tint, 0.f, 100.f);
        }
        if (done)
            done(st->name, g);
    }, ModalDialog::Role::Default);
    /* Laid out with the buttons in: the size the rows set came first */
    d->set_size(Vector2i(0, 0));
    d->open();
}

} // namespace pagemade
