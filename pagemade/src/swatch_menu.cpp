/*
 * pagemade/swatch_menu.cpp — see swatch_menu.h.
 */
#include "swatch_menu.h"

#include "dialogs.h"
#include "page.h"

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menu.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>
#include <nanogui/textbox.h>
#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace nanogui;

namespace pagemade {
namespace {

/* A swatch row: color chip, name, and the CMYK definition right-aligned.
 * Built-ins draw a lock in the mark zone; custom rows a pencil. The mark
 * machinery swallows the press so editing never chooses the row. */
class SwatchRow : public MenuItem {
public:
    SwatchRow(Widget *parent, const Swatch &swatch, bool locked)
        : MenuItem(parent, swatch.name), m_color(swatch.rgb), m_locked(locked) {}

    void draw(NVGcontext *ctx) override {
        Widget::draw(ctx);
        if (m_highlighted && m_enabled) {
            float cr = m_theme ? (float) m_theme->m_menu_item_corner_radius : 6.f;
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, m_pos.x() + 4.f, m_pos.y() + 1.f, m_size.x() - 8.f,
                           m_size.y() - 2.f, cr);
            nvgFillColor(ctx, m_theme->m_menu_item_highlight_fill);
            nvgFill(ctx);
        }
        const float cx = (float) m_pos.x(), cy = (float) m_pos.y();
        const float h = (float) m_size.y();
        NVGcolor ink;
        if (!m_enabled)
            ink = m_theme->m_disabled_text_color;
        else if (m_highlighted)
            ink = m_theme->m_menu_item_highlight_text;
        else
            ink = m_theme->m_text_color;

        /* The chip, shifted right when this is the current paint. */
        const float chip = h - 10.f;
        const int font_size = m_font_size == -1 ? m_theme->m_button_font_size : m_font_size;
        float chip_x = cx + 8.f;
        if (m_pushed) {
            nvgFontSize(ctx, (float) font_size * icon_scale());
            nvgFontFace(ctx, "icons");
            nvgFillColor(ctx, ink);
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(ctx, cx + 14.f, cy + h * 0.5f, utf8(FA_CHECK).data(), nullptr);
            chip_x += 20.f;
        }
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, chip_x, cy + 5.f, chip, chip, 3.f);
        nvgFillColor(ctx, nvgRGBAf(m_color.r, m_color.g, m_color.b, 1.f));
        nvgFill(ctx);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, nvgRGBA(0, 0, 0, 90));
        nvgStroke(ctx);

        nvgFontSize(ctx, (float) font_size);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, ink);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        const float tx = chip_x + chip + 8.f;
        /* The mark zone keeps the last 28 px; clip the name short of it. */
        nvgSave(ctx);
        nvgIntersectScissor(ctx, cx, cy, std::max(1.f, (float) m_size.x() - 30.f), h);
        nvgText(ctx, tx, cy + h * 0.5f - 1.f, m_caption.c_str(), nullptr);
        nvgRestore(ctx);

        /* The CMYK tag, right-aligned against the mark zone. */
        nvgFontSize(ctx, (float) font_size - 2.f);
        nvgFillColor(ctx, m_highlighted && m_enabled ? ink : m_theme->m_disabled_text_color);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, cx + m_size.x() - 30.f, cy + h * 0.5f - 1.f, m_tag.c_str(), nullptr);

        /* Lock or pencil in the mark zone. */
        const int icon = m_locked ? FA_LOCK : FA_PENCIL_ALT;
        nvgFontSize(ctx, (float) font_size * icon_scale() * 0.9f);
        nvgFontFace(ctx, "icons");
        nvgFillColor(ctx, m_highlighted ? ink : nvgRGBA(130, 130, 130, 220));
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(ctx, cx + m_size.x() - 14.f, cy + h * 0.5f, utf8(icon).data(), nullptr);
    }

    void set_tag(const std::string &tag) { m_tag = tag; }

    /* Wide enough for chip, name and the CMYK tag: the base measure sees
     * only the caption, which clips the tag. */
    Vector2i preferred_size(NVGcontext *ctx) const override {
        Vector2i size = MenuItem::preferred_size(ctx);
        const int font_size = m_font_size == -1 ? m_theme->m_button_font_size : m_font_size;
        nvgFontSize(ctx, (float) font_size);
        nvgFontFace(ctx, "sans");
        const float name_w = nvgTextBounds(ctx, 0, 0, m_caption.c_str(), nullptr, nullptr);
        nvgFontSize(ctx, (float) font_size - 2.f);
        const float tag_w = nvgTextBounds(ctx, 0, 0, m_tag.c_str(), nullptr, nullptr);
        /* chip + gaps + name + gap + tag + mark zone, with room to spare. */
        const int want = (int) (8 + 18 + 8 + name_w + 16 + tag_w + 8 + 28 + 8);
        if (want > size.x())
            size.x() = want;
        return size;
    }

private:
    pagemade::Color m_color;
    std::string m_tag;
    bool m_locked = false;
};

bool is_builtin(SwatchId id) {
    return id == kPaper || id == kBlack || id == kRegistration;
}

/* pagemade::Color (floats) to nanogui::Color for the wheel. */
nanogui::Color to_nvg(const pagemade::Color &c) {
    return nanogui::Color(c.r, c.g, c.b, c.a);
}

std::string trim_name(std::string name) {
    size_t a = 0;
    while (a < name.size() && std::isspace((unsigned char) name[a]))
        ++a;
    size_t b = name.size();
    while (b > a && std::isspace((unsigned char) name[b - 1]))
        --b;
    return name.substr(a, b - a);
}

} // namespace

/* A ComboBox whose closed face shows the current color: a chip plus the
 * name. The popup rows come from refill_swatch_menu. */
ColorDropdown::ColorDropdown(Widget *parent) : Dropdown(parent, Dropdown::ComboBox, "") {}

void ColorDropdown::set_swatch(const std::string &name, const pagemade::Color &color) {
    set_caption(name);
    m_chip = color;
    m_has_chip = true;
}

void ColorDropdown::clear_swatch() {
    m_has_chip = false;
    set_caption("");
}

nanogui::Vector2i ColorDropdown::preferred_size(NVGcontext *ctx) const {
    Vector2i size = Dropdown::preferred_size(ctx);
    /* The caption measure misses the chip; keep room for it. */
    if (m_has_chip)
        size.x() += 30;
    return size;
}

void ColorDropdown::draw(NVGcontext *ctx) {
        if (!m_has_chip) {
            Dropdown::draw(ctx);
            return;
        }
        if (!m_popup->visible())
            set_pushed(false);
        else {
            update_popup_geometry();
            m_popup->perform_layout(ctx);
        }
        if (!m_enabled && m_pushed)
            m_pushed = false;
        Widget::draw(ctx);

        NVGcolor grad_top = m_theme->m_button_gradient_top_unfocused;
        NVGcolor grad_bot = m_theme->m_button_gradient_bot_unfocused;
        if (m_pushed) {
            grad_top = m_theme->m_button_gradient_top_pushed;
            grad_bot = m_theme->m_button_gradient_bot_pushed;
        } else if (m_mouse_focus && m_enabled) {
            grad_top = m_theme->m_button_gradient_top_focused;
            grad_bot = m_theme->m_button_gradient_bot_focused;
        }
        float cr = (float) std::max(0, m_theme->m_button_corner_radius);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + 1, m_pos.y() + 1.0f, m_size.x() - 2, m_size.y() - 2,
                       cr);
        if (m_background_color.w() != 0) {
            nvgFillColor(ctx, nanogui::Color(m_background_color[0], m_background_color[1],
                                            m_background_color[2], 1.f));
            nvgFill(ctx);
        }
        NVGpaint bg = nvgLinearGradient(ctx, m_pos.x(), m_pos.y(), m_pos.x(),
                                        m_pos.y() + m_size.y(), grad_top, grad_bot);
        nvgFillPaint(ctx, bg);
        nvgFill(ctx);
        if (m_theme->m_border_dark.w() > 0) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, m_pos.x() + 0.5f, m_pos.y() + 0.5f, m_size.x() - 1,
                           m_size.y() - 1, cr);
            nvgStrokeWidth(ctx, 1.0f);
            nvgStrokeColor(ctx, m_theme->m_border_dark);
            nvgStroke(ctx);
        }

        const int font_size = m_font_size == -1 ? m_theme->m_button_font_size : m_font_size;
        const Vector2f center = Vector2f(m_pos) + Vector2f(m_size) * 0.5f;
        NVGcolor text_color;
        if (!m_enabled)
            text_color = m_theme->m_disabled_text_color;
        else if (m_text_color.w() != 0)
            text_color = m_text_color;
        else
            text_color = m_theme->m_text_color;

        /* The chip. */
        const float chip = (float) m_size.y() - 10.f;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + 8.f, center.y() - chip * 0.5f, chip, chip, 3.f);
        nvgFillColor(ctx, nvgRGBAf(m_chip.r, m_chip.g, m_chip.b, 1.f));
        nvgFill(ctx);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, nvgRGBA(0, 0, 0, 90));
        nvgStroke(ctx);

        nvgFontSize(ctx, (float) font_size);
        nvgFontFace(ctx, "sans");
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, text_color);
        nvgSave(ctx);
        nvgIntersectScissor(ctx, m_pos.x() + 6.f, m_pos.y(),
                            std::max(0.f, (float) m_size.x() - 28.f - chip), (float) m_size.y());
        nvgText(ctx, m_pos.x() + 10 + chip + 6, center.y() - 1, m_caption.c_str(), nullptr);
        nvgRestore(ctx);

        const std::string icon = utf8(FA_SORT);
        nvgFontSize(ctx, (m_font_size < 0 ? m_theme->m_button_font_size : m_font_size) *
                             icon_scale());
        nvgFontFace(ctx, "icons");
        nvgFillColor(ctx, m_enabled ? text_color : NVGcolor(m_theme->m_disabled_text_color));
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        const float iw = nvgTextBounds(ctx, 0, 0, icon.data(), nullptr, nullptr);
        nvgText(ctx, m_pos.x() + m_size.x() - iw - 8, center.y(), icon.data(), nullptr);
    }

void refill_swatch_menu(Dropdown *menu, const SwatchMenuView &view,
                        const std::function<void(SwatchId)> &pick,
                        const std::function<void(SwatchId)> &edit,
                        const std::function<void()> &add) {
    if (!menu || !menu->popup())
        return;
    menu->clear_items();
    int current_index = -1;
    const Swatch *current = nullptr;
    for (size_t si = 0; si < view.swatches.size(); ++si) {
        const Swatch &sw = view.swatches[si];
        if (sw.id == kNoPaint)
            continue;
        auto *row = new SwatchRow(menu->popup(), sw, is_builtin(sw.id));
        row->set_tag(sw.id <= kRegistration ? (sw.id == kPaper ? "Paper" : "CMYK") :
                                              cmyk_text(sw.cmyk));
        row->set_flags(Button::RadioButton);
        const SwatchId id = sw.id;
        const int index = menu->popup()->child_count() - 1;
        if (sw.id == view.current) {
            current_index = index;
            current = &view.swatches[si];
        }
        row->set_callback([menu, pick, id, index] {
            if (pick)
                pick(id);
            /* The check follows the paint; the face follows on refill. */
            menu->set_selected_index(index);
        });
        /* The mark swallows the press; the deferred callback opens the
         * editor after the click is done with the menu. Built-ins pass no
         * callback, so the lock zone is dead. */
        if (!is_builtin(sw.id) && edit)
            row->set_mark(false, [edit, id] { edit(id); });
        else
            row->set_mark(false, {});
    }
    MenuItem *plus = menu->add_item("Add color...", FA_PLUS);
    if (plus && add)
        plus->set_callback([add] { add(); });
    /* The check follows the current paint; the button face shows it. */
    menu->popup()->set_selected_index(current_index);
    if (auto *colors = dynamic_cast<ColorDropdown *>(menu)) {
        if (current)
            colors->set_swatch(current->name, current->rgb);
        else
            colors->clear_swatch();
    } else if (current) {
        menu->set_caption(current->name);
    }

    if (!menu->popup()->visible())
        return;
    if (Screen *screen = menu->screen()) {
        if (NVGcontext *nvg = screen->nvg_context())
            menu->popup()->perform_layout(nvg);
    }
    const int index = menu->selected_index();
    if (index >= 0)
        menu->popup()->reveal(index);
}

/* ---- The editor ------------------------------------------------------------ */

namespace {

/* Three or four labeled sliders with numeric boxes, kept in lockstep with
 * the wheel through set_all(). CMYK is the truth; RGB converts both ways. */
class ColorSliders : public Widget {
public:
    ColorSliders(Widget *parent, bool cmyk) : Widget(parent), m_cmyk(cmyk) {
        set_layout(new GridLayout(Orientation::Horizontal, 3, Alignment::Middle, 0, 4));
        const char **names = cmyk ? kCmykNames : kRgbNames;
        const int n = cmyk ? 4 : 3;
        for (int i = 0; i < n; ++i) {
            new Label(this, names[i], "sans-bold");
            auto *slider = new Slider(this);
            slider->set_range({0.f, cmyk ? 100.f : 255.f});
            slider->set_fixed_size(Vector2i(150, 20));
            auto *box = new IntBox<int>(this, 0);
            box->set_editable(true);
            box->set_fixed_size(Vector2i(56, 24));
            box->set_min_max_values(0, cmyk ? 100 : 255);
            m_sliders.push_back(slider);
            m_boxes.push_back(box);
            const int channel = i;
            slider->set_callback([this, channel](float v) { changed(channel, v); });
            box->set_callback([this, channel](int v) { changed(channel, (float) v); });
        }
    }

    void set_on_change(std::function<void()> on_change) { m_on_change = std::move(on_change); }

    /* Push a color into every control without firing callbacks. Neither
     * Slider::set_value nor TextBox::set_value calls back. */
    void set_all(const Cmyk &cmyk, const pagemade::Color &rgb) {
        m_syncing = true;
        for (size_t i = 0; i < m_sliders.size(); ++i) {
            const float v = m_cmyk ? (i == 0 ? cmyk.c : i == 1 ? cmyk.m : i == 2 ? cmyk.y : cmyk.k) :
                                     (i == 0 ? rgb.r * 255.f : i == 1 ? rgb.g * 255.f : rgb.b * 255.f);
            m_sliders[i]->set_value(v);
            m_boxes[i]->set_value((int) std::lround(v));
        }
        m_syncing = false;
    }

    void set_sliders_enabled(bool enabled) {
        for (Slider *s : m_sliders)
            s->set_enabled(enabled);
        for (IntBox<int> *b : m_boxes)
            b->set_enabled(enabled);
    }

    Cmyk cmyk_value() const {
        Cmyk out;
        if (!m_cmyk || m_sliders.size() < 4)
            return out;
        out.c = m_sliders[0]->value();
        out.m = m_sliders[1]->value();
        out.y = m_sliders[2]->value();
        out.k = m_sliders[3]->value();
        return out;
    }

    pagemade::Color rgb_value() const {
        if (m_cmyk || m_sliders.size() < 3)
            return {0, 0, 0, 1};
        return {m_sliders[0]->value() / 255.f, m_sliders[1]->value() / 255.f,
                m_sliders[2]->value() / 255.f, 1};
    }

private:
    void changed(int channel, float value) {
        if (m_syncing)
            return;
        /* The box follows the slider exactly (and vice versa). */
        m_syncing = true;
        m_sliders[(size_t) channel]->set_value(value);
        m_boxes[(size_t) channel]->set_value((int) std::lround(value));
        m_syncing = false;
        if (m_on_change)
            m_on_change();
    }

    static const char *kRgbNames[3];
    static const char *kCmykNames[4];
    bool m_cmyk = false;
    bool m_syncing = false;
    std::vector<Slider *> m_sliders;
    std::vector<IntBox<int> *> m_boxes;
    std::function<void()> m_on_change;
};

const char *ColorSliders::kRgbNames[3] = {"R", "G", "B"};
const char *ColorSliders::kCmykNames[4] = {"C", "M", "Y", "K"};

/* A solid preview chip. */
class Preview : public Widget {
public:
    explicit Preview(Widget *parent) : Widget(parent) { set_fixed_size(Vector2i(64, 28)); }
    void set_color(const pagemade::Color &color) {
        m_color = color;
        if (screen())
            screen()->redraw();
    }
    void draw(NVGcontext *ctx) override {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, (float) m_pos.x(), (float) m_pos.y(), (float) m_size.x(),
                       (float) m_size.y(), 4.f);
        nvgFillColor(ctx, nvgRGBAf(m_color.r, m_color.g, m_color.b, 1.f));
        nvgFill(ctx);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, nvgRGBA(0, 0, 0, 90));
        nvgStroke(ctx);
    }

private:
    pagemade::Color m_color{1, 1, 1, 1};
};

std::string hex_text(const pagemade::Color &rgb) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "# %02X%02X%02X", (int) std::lround(rgb.r * 255.f),
                  (int) std::lround(rgb.g * 255.f), (int) std::lround(rgb.b * 255.f));
    return buf;
}

} // namespace

void ask_swatch(Screen *screen, const SwatchEdit &initial, bool locked,
                std::function<void(const std::string &, const Cmyk &)> save,
                std::function<void()> remove,
                std::function<void(const std::string &, const Cmyk &)> to_library) {
    auto *d = new ModalDialog(screen, initial.name.empty() ? "New Color" : "Edit Color");
    struct State {
        std::string name;
        Cmyk cmyk;
        bool cmyk_mode = true;
        bool syncing = false;
        bool dialog_locked = false;
    };
    auto st = std::make_shared<State>();
    st->name = initial.name;
    st->cmyk = initial.cmyk;
    st->dialog_locked = locked;

    /* Name row with the lock top-right. */
    Widget *top = new Widget(d->body());
    top->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 10));
    new Label(top, "Name:", "sans-bold");
    TextBox *name_box = new TextBox(top, initial.name);
    name_box->set_editable(!locked);
    name_box->set_alignment(TextBox::Alignment::Left);
    name_box->set_fixed_size(Vector2i(220, 28));
    name_box->set_callback([st](const std::string &v) {
        st->name = v;
        return true;
    });
    Button *lock = new Button(top, "", locked ? FA_LOCK : FA_UNLOCK);
    lock->set_fixed_size(Vector2i(28, 28));
    lock->set_tooltip(locked ? "Locked: built-in colors cannot change"
                             : "Lock against editing");
    lock->set_enabled(!locked);

    /* Wheel beside the sliders. */
    Widget *mid = new Widget(d->body());
    mid->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 16));
    ColorWheel *wheel = new ColorWheel(mid, to_nvg(cmyk_to_rgb(initial.cmyk)));
    wheel->set_fixed_size(Vector2i(180, 180));
    wheel->set_enabled(!locked);
    Widget *right = new Widget(mid);
    right->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Minimum, 0, 8));
    Dropdown *mode = new Dropdown(right, {"CMYK", "RGB"}, {}, Dropdown::ComboBox, "CMYK");
    mode->set_fixed_size(Vector2i(120, 26));
    mode->set_tooltip("Sliders show CMYK or RGB; both stay in step");
    mode->set_enabled(!locked);
    ColorSliders *cmyk_sliders = new ColorSliders(right, true);
    ColorSliders *rgb_sliders = new ColorSliders(right, false);
    rgb_sliders->set_visible(false);
    if (locked) {
        cmyk_sliders->set_sliders_enabled(false);
        rgb_sliders->set_sliders_enabled(false);
    }

    /* Hex readout, preview, and the globe. */
    Widget *foot = new Widget(right);
    foot->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 10));
    Label *hex = new Label(foot, "", "sans", 15);
    Preview *preview = new Preview(foot);
    Button *globe = new Button(foot, "", FA_GLOBE);
    globe->set_fixed_size(Vector2i(28, 28));
    globe->set_tooltip(initial.in_library ? "Already in the global colors"
                                          : "Save to the global colors");
    globe->set_enabled(!initial.in_library && !locked);

    /* Push the truth into every control. set_all and set_color are quiet;
     * only user drags call back. */
    std::function<void()> refresh = [=] {
        st->syncing = true;
        const pagemade::Color rgb = cmyk_to_rgb(st->cmyk);
        cmyk_sliders->set_all(st->cmyk, rgb);
        rgb_sliders->set_all(st->cmyk, rgb);
        wheel->set_color(to_nvg(rgb));
        hex->set_caption(hex_text(rgb));
        preview->set_color(rgb);
        st->syncing = false;
    };
    cmyk_sliders->set_on_change([=] {
        if (st->syncing)
            return;
        st->cmyk = cmyk_sliders->cmyk_value();
        refresh();
    });
    rgb_sliders->set_on_change([=] {
        if (st->syncing)
            return;
        st->cmyk = rgb_to_cmyk(rgb_sliders->rgb_value());
        refresh();
    });
    wheel->set_callback([=](const nanogui::Color &wheel_rgb) {
        const pagemade::Color rgb{wheel_rgb.r(), wheel_rgb.g(), wheel_rgb.b(), 1};
        if (st->syncing)
            return;
        st->cmyk = rgb_to_cmyk(rgb);
        refresh();
    });
    mode->set_selected_callback([=](int i) {
        st->cmyk_mode = i == 0;
        cmyk_sliders->set_visible(st->cmyk_mode);
        rgb_sliders->set_visible(!st->cmyk_mode);
        if (Screen *s = screen)
            s->perform_layout();
    });
    if (to_library)
        globe->set_callback([st, to_library, globe, name_box] {
            /* The Save button commits text boxes; the globe is not one, so
             * commit a focused name box before reading it. */
            if (name_box->focused())
                name_box->focus_event(false);
            const std::string name = trim_name(st->name);
            if (name.empty())
                return;
            to_library(name, st->cmyk);
            globe->set_tooltip("Already in the global colors");
            globe->set_enabled(false);
        });
    lock->set_callback([=]() mutable {
        st->dialog_locked = !st->dialog_locked;
        const bool off = st->dialog_locked;
        lock->set_icon(off ? FA_LOCK : FA_UNLOCK);
        name_box->set_editable(!off);
        wheel->set_enabled(!off);
        mode->set_enabled(!off);
        cmyk_sliders->set_sliders_enabled(!off);
        rgb_sliders->set_sliders_enabled(!off);
        globe->set_enabled(!off);
    });
    refresh();

    d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
    if (remove)
        d->add_button("Delete", [remove] { remove(); });
    d->add_button("Save", [st, save] {
        const std::string name = trim_name(st->name);
        if (!name.empty() && save)
            save(name, st->cmyk);
    }, ModalDialog::Role::Default);
    d->open();
}

} // namespace pagemade
