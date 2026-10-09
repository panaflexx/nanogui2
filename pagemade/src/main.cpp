/*
 * pagemade — page layout in the spirit of Aldus PageMaker.
 *
 * One page on a pasteboard with text blocks and shapes. The toolbar holds
 * PageMaker's toolbox (pointer, rotate, text, line, rectangle, ellipse,
 * polygon), snap to guides, undo/redo (Ctrl+Z / Ctrl+Shift+Z), a zoom
 * dropdown, and switches for alignment, kerning and ligatures. Below it, a
 * control palette shows the selection's position, size and angle and its
 * fill and stroke (with nothing selected, the defaults for new shapes).
 * PageMaker's zoom shortcuts: Ctrl/Cmd + 0 fit, 5 50%, 7 75%, 1 actual
 * size, 2 200%, 4 400%, 8 800%.
 *
 *   pagemade
 *   pagemade --screenshot out.png [--zoom 4 --at 150,400] [--select 1,1]
 *            [--baselines] [--loose] [--no-kerning] [--no-snap]
 *            [--tool pointer|text|rotate|line|rect|ellipse|polygon ...]
 *            [--fill swatch[,tint] ...] [--stroke swatch,weight[,style] ...]
 *            [--drag x0,y0:x1,y1 ...] [--click x,y ...] [--type txt ...]
 *            [--key [mod+]name ...]
 *
 * --at is a page point (points from the page's top-left) to center on.
 * --select picks a text block by story and thread index ("1,0").
 * --drag presses at one page point and releases at another, through the
 * real event path, before the screenshot is taken. --click, --type and
 * --key (enter, backspace, left, ..., a-z; mods shift/ctrl/alt) exercise
 * the text tool the same way. --tool picks a tool, and --fill / --stroke
 * set the selection's paint as the control palette would (style: solid,
 * dashed, dotted, dashdot). All of these run in the order given.
 */
#include <nanogui/nanogui.h>
#include <nanogui/menu.h>
#include <nanogui/opengl.h>
#include <nanogui/zoomscrollpanel.h>
#include <GLFW/glfw3.h>

#include "composer/hyphenator.h"
#include "composerview.h"
#include "default_fonts.h"
#include "page.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../ext/glfw/deps/stb_image_write.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef PAGEMADE_DATA_DIR
#define PAGEMADE_DATA_DIR "pagemade/resources"
#endif

using namespace nanogui;
using namespace pagemade;

class PagemadeApp : public Screen {
public:
    PagemadeApp() : Screen(Vector2i(1280, 940), "pagemade") {
        inc_ref();
        set_theme_mode(ThemeMode::Light);
        register_default_fonts(m_fonts);
        const std::string hyph = std::string(PAGEMADE_DATA_DIR) + "/hyphenation/hyph-en-us";
        if (!m_hyphenator.load(hyph + ".pat.txt", hyph + ".hyp.txt"))
            std::fprintf(stderr, "pagemade: no hyphenation patterns at %s.pat.txt\n", hyph.c_str());

        auto *root_flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        RootWindow *window = new RootWindow(this, root_flex);

        Widget *toolbar = new Widget(window);
        toolbar->set_min_height(40);
        toolbar->set_height(40);
        toolbar->set_height_flex(SizeMode::Fixed);
        toolbar->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 8, 6));

        using Tool = ComposerView::Tool;
        auto tool_btn = [&](Tool t, int icon, const char *tip) {
            ToolButton *b = new ToolButton(toolbar, icon);
            b->set_tooltip(tip);
            b->set_callback([this, t] { apply_tool(t); });
            m_tool_btns.push_back({t, b});
        };
        tool_btn(Tool::Pointer, FA_MOUSE_POINTER,
                 "Pointer tool (Shift+click adds, drag a marquee; Delete removes; "
                 "arrows nudge; Ctrl+F/B front/back)");
        tool_btn(Tool::Rotate, FA_SYNC_ALT, "Rotate tool (Shift snaps to 15\u00B0)");
        tool_btn(Tool::Text, FA_I_CURSOR, "Text tool (drag on empty space for a new block)");
        tool_btn(Tool::Line, FA_SLASH, "Line tool (Shift: 45\u00B0 steps)");
        tool_btn(Tool::Rect, FA_SQUARE, "Rectangle tool (Shift: square)");
        tool_btn(Tool::Ellipse, FA_CIRCLE, "Ellipse tool (Shift: circle)");
        tool_btn(Tool::Polygon, FA_DRAW_POLYGON, "Polygon tool (Shift: regular)");
        m_tool_btns.front().second->set_pushed(true);

        m_snap_btn = new Button(toolbar, "", FA_MAGNET);
        m_snap_btn->set_flags(Button::ToggleButton);
        m_snap_btn->set_pushed(true);
        m_snap_btn->set_tooltip("Snap to guides");
        m_snap_btn->set_change_callback([this](bool on) { m_view->set_snap(on); });

        m_undo_btn = new Button(toolbar, "", FA_UNDO);
        m_redo_btn = new Button(toolbar, "", FA_REDO);
        m_undo_btn->set_tooltip("Undo (Ctrl+Z)");
        m_redo_btn->set_tooltip("Redo (Ctrl+Shift+Z)");
        m_undo_btn->set_callback([this] { m_view->undo(); });
        m_redo_btn->set_callback([this] { m_view->redo(); });

        /* The zoom levels PageMaker keeps in the View menu, as one dropdown.
         * 0 means Fit in Window. The caption follows the actual zoom. */
        m_zoom_menu = new Dropdown(toolbar, {"Fit", "50%", "100%", "200%", "400%", "800%"},
                                   {}, Dropdown::ComboBox, "Fit");
        m_zoom_menu->set_tooltip("Zoom (Ctrl+0 fit, Ctrl+5/7/1/2/4/8)");
        m_zoom_menu->set_selected_callback([this](int i) {
            static const double zv[] = {0, 0.5, 1, 2, 4, 8};
            zv[i] > 0 ? zoom_to(zv[i]) : fit_page();
        });

        new Label(toolbar, "  Body:", "sans-bold");
        m_align = new ComboBox(toolbar, {"Left", "Center", "Right", "Justify", "Force justify"});
        m_align->set_selected_index(3);
        m_align->set_callback([this](int i) { set_body_align((Align) i); });

        m_kern = new CheckBox(toolbar, "Kerning", [this](bool on) {
            for_each_run([on](CharStyle &cs) { cs.kerning = on; });
        });
        m_kern->set_checked(true);
        auto *liga = new CheckBox(toolbar, "Ligatures", [this](bool on) {
            for_each_run([on](CharStyle &cs) { cs.ligatures = on; });
        });
        liga->set_checked(true);
        m_baselines = new CheckBox(toolbar, "Baselines", [this](bool on) {
            m_view->set_show_baselines(on);
        });
        m_loose = new CheckBox(toolbar, "Loose/tight lines", [this](bool on) {
            m_view->set_show_loose_tight(on);
        });

        PageDoc doc = sample_document();
        build_palette(window, doc);

        m_scroll = new ZoomScrollPanel(window, ZoomScrollPanel::ScrollTypes::Both);
        m_scroll->set_zoom_range(0.1, 16.0);
        m_scroll->set_zoom_enabled(true);
        m_scroll->set_height_flex(SizeMode::Expanding);
        root_flex->set_flex_item(m_scroll, FlexLayout::FlexItem(1.0f));

        m_view = new ComposerView(m_scroll, &m_fonts, &m_hyphenator);
        m_view->on_recompose = [this] { update_status(); };
        m_view->on_tool_change = [this](ComposerView::Tool t) { sync_tool_buttons(t); };
        m_view->on_selection_change = [this] { sync_palette(); };

        m_status = new Label(window, "", "sans", 16);
        m_status->set_min_height(24);
        m_status->set_height(24);
        m_status->set_height_flex(SizeMode::Fixed);

        m_view->set_document(std::move(doc));
        sync_palette();
        perform_layout();
    }

    /* ---- Control palette ------------------------------------------- */

    static constexpr float kTints[] = {100, 80, 60, 40, 30, 20, 10};
    static constexpr float kWeights[] = {0.25f, 0.5f, 1, 2, 4, 6, 8, 12};

    void build_palette(Widget *window, const PageDoc &doc) {
        Widget *bar = new Widget(window);
        bar->set_min_height(34);
        bar->set_height(34);
        bar->set_height_flex(SizeMode::Fixed);
        bar->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 8, 5));

        auto number = [&](const char *label, const char *units, int field, float width) {
            new Label(bar, label, "sans-bold");
            auto *box = new FloatBox<float>(bar);
            box->set_editable(true);
            box->number_format("%.2f");
            box->set_units(units);
            box->set_fixed_size(Vector2i((int) width, 26));
            box->set_callback([this, field](float v) { apply_geometry(field, v); });
            m_geom[field] = box;
        };
        number("X", "pt", 0, 70);
        number("Y", "pt", 1, 70);
        number("W", "pt", 2, 70);
        number("H", "pt", 3, 70);
        number("Rot", "\u00B0", 4, 58);

        /* The Colors palette's swatches, "None" first, in both paint menus. */
        std::vector<std::string> swatches{"None"};
        m_swatch_ids = {kNoPaint};
        for (const Swatch &sw : doc.swatches) {
            swatches.push_back(sw.name);
            m_swatch_ids.push_back(sw.id);
        }
        auto menu = [&](const std::vector<std::string> &items, int width) {
            auto *d = new Dropdown(bar, items, {}, Dropdown::ComboBox, items.front());
            d->set_fixed_size(Vector2i(width, 26));
            return d;
        };

        new Label(bar, " Fill", "sans-bold");
        m_fill = menu(swatches, 108);
        m_fill->set_selected_callback([this](int i) {
            const SwatchId id = m_swatch_ids[i];
            m_view->apply_shape_style([id](Shape &sh) { sh.fill.swatch = id; });
        });
        m_fill_tint = menu({"100%", "80%", "60%", "40%", "30%", "20%", "10%"}, 66);
        m_fill_tint->set_selected_callback([this](int i) {
            const float t = kTints[i];
            m_view->apply_shape_style([t](Shape &sh) { sh.fill.tint = t; });
        });

        new Label(bar, " Stroke", "sans-bold");
        m_stroke = menu(swatches, 108);
        m_stroke->set_selected_callback([this](int i) {
            const SwatchId id = m_swatch_ids[i];
            m_view->apply_shape_style([id](Shape &sh) { sh.stroke.paint.swatch = id; });
        });
        m_weight = menu({"Hairline", "0.5 pt", "1 pt", "2 pt", "4 pt", "6 pt", "8 pt", "12 pt"}, 82);
        m_weight->set_selected_callback([this](int i) {
            const float w = kWeights[i];
            m_view->apply_shape_style([w](Shape &sh) { sh.stroke.weight = w; });
        });
        m_style = menu({"Solid", "Dashed", "Dotted", "Dash-dot"}, 88);
        m_style->set_selected_callback([this](int i) {
            m_view->apply_shape_style([i](Shape &sh) { sh.stroke.style = (LineStyle) i; });
        });

        new Label(bar, " Sides", "sans-bold");
        m_sides = new IntBox<int>(bar, 6);
        m_sides->set_editable(true);
        m_sides->set_min_max_values(3, 100);
        m_sides->set_fixed_size(Vector2i(44, 26));
        m_sides->set_callback([this](int v) {
            m_view->apply_shape_style([v](Shape &sh) { sh.sides = std::max(3, v); });
        });
        new Label(bar, "Star", "sans-bold");
        m_star = new IntBox<int>(bar, 0);
        m_star->set_editable(true);
        m_star->set_min_max_values(0, 100);
        m_star->set_units("%");
        m_star->set_fixed_size(Vector2i(54, 26));
        m_star->set_callback([this](int v) {
            m_view->apply_shape_style([v](Shape &sh) { sh.star_inset = (float) v; });
        });
    }

    void apply_geometry(int field, float v) {
        ComposerView::Geometry g;
        if (!m_view->geometry(g))
            return;
        float *f[] = {&g.x, &g.y, &g.w, &g.h, &g.angle};
        *f[field] = v;
        m_view->set_geometry(g);
    }

    void sync_palette() {
        if (!m_fill)
            return;
        ComposerView::Geometry g;
        const bool one = m_view->geometry(g);
        const float vals[] = {g.x, g.y, g.w, g.h, g.angle};
        for (int i = 0; i < 5; ++i) {
            m_geom[i]->set_enabled(one);
            if (m_geom[i]->focused())
                continue;
            if (one)
                m_geom[i]->set_value(vals[i]);
            else
                m_geom[i]->TextBox::set_value("");
        }
        const Shape sh = m_view->shape_style();
        auto swatch_index = [&](SwatchId id) {
            for (size_t i = 0; i < m_swatch_ids.size(); ++i)
                if (m_swatch_ids[i] == id)
                    return (int) i;
            return 0;
        };
        auto nearest = [](const float *v, int n, float x) {
            int best = 0;
            for (int i = 1; i < n; ++i)
                if (std::fabs(v[i] - x) < std::fabs(v[best] - x))
                    best = i;
            return best;
        };
        m_fill->set_selected_index(swatch_index(sh.fill.swatch));
        m_fill_tint->set_selected_index(nearest(kTints, 7, sh.fill.tint));
        m_stroke->set_selected_index(swatch_index(sh.stroke.paint.swatch));
        m_weight->set_selected_index(nearest(kWeights, 8, sh.stroke.weight));
        m_style->set_selected_index((int) sh.stroke.style);
        if (!m_sides->focused())
            m_sides->set_value(sh.sides);
        if (!m_star->focused())
            m_star->set_value((int) sh.star_inset);
    }

    /* The view's tool, mirrored in the toolbar's radio buttons. */
    void apply_tool(ComposerView::Tool t) {
        m_view->set_tool(t);
        sync_tool_buttons(t);
    }

    void sync_tool_buttons(ComposerView::Tool t) {
        for (auto &tb : m_tool_btns)
            tb.second->set_pushed(tb.first == t);
    }

    void zoom_to(double z) {
        m_scroll->set_zoom_about(z, m_scroll->size() / 2);
        update_status();
    }

    /* The dropdown mirrors the actual zoom: a preset when it matches one,
     * the plain percentage otherwise. */
    void sync_zoom_menu() {
        if (!m_zoom_menu)
            return;
        const double z = m_scroll->zoom();
        static const double zv[] = {0.5, 1, 2, 4, 8};
        for (int i = 0; i < 5; ++i)
            if (std::fabs(z - zv[i]) < 0.005) {
                if (m_zoom_menu->selected_index() != i + 1)
                    m_zoom_menu->set_selected_index(i + 1);
                return;
            }
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f%%", z * 100.0);
        if (m_zoom_menu->caption() != buf)
            m_zoom_menu->set_caption(buf);
    }

    void fit_page() {
        const PageSetup &s = m_view->document().setup;
        Vector2i panel = m_scroll->size();
        double z = std::min((panel.x() - 24) / (s.width + 24.0),
                            (panel.y() - 24) / (s.height + 24.0));
        m_scroll->set_zoom(z);
        center_on(Vector2f(s.width * 0.5f, s.height * 0.5f));
        update_status();
    }

    /* Put a page point at the middle of the panel. */
    void center_on(const Vector2f &page_pt) {
        perform_layout();
        double z = m_scroll->zoom();
        Vector2f v = m_view->page_origin() + page_pt;
        m_scroll->set_pan_offset(ZoomScrollPanel::Vector2d(m_scroll->size().x() * 0.5 - v.x() * z,
                                                           m_scroll->size().y() * 0.5 - v.y() * z));
    }

    void set_body_align(Align a) {
        m_view->push_undo();
        for (StoryEntry &se : m_view->document().stories)
            for (Paragraph &p : se.story.paragraphs)
                if (p.style.name == "Body text")
                    p.style.align = a;
        m_view->recompose();
    }

    template <typename F> void for_each_run(F fn) {
        m_view->push_undo();
        for (StoryEntry &se : m_view->document().stories)
            for (Paragraph &p : se.story.paragraphs)
                for (Run &r : p.runs)
                    fn(r.style);
        m_view->recompose();
    }

    void update_status() {
        if (!m_status)
            return;
        const auto &comps = m_view->compositions();
        size_t lines = 0;
        double ms = 0;
        bool overset = false;
        for (const Composition &c : comps) {
            lines += c.lines.size();
            ms += c.compose_ms;
            overset |= c.overset;
        }
        char buf[200];
        const PageDoc &doc = m_view->document();
        std::snprintf(buf, sizeof buf,
                      "  %zu items, %zu stories, %zu lines, composed in %.2f ms%s   |   zoom %.0f%%",
                      doc.pages[0].items.size(), comps.size(), lines, ms,
                      overset ? "   |   story overset (red arrow)" : "", m_scroll->zoom() * 100.0);
        m_status->set_caption(buf);
        sync_zoom_menu();
        m_undo_btn->set_enabled(m_view->can_undo());
        m_redo_btn->set_enabled(m_view->can_redo());
    }

    bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        if (action != GLFW_PRESS || !(modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)))
            return false;
        switch (key) {
        case GLFW_KEY_Z:
            if (modifiers & GLFW_MOD_SHIFT) m_view->redo(); else m_view->undo();
            return true;
        case GLFW_KEY_Y:
            m_view->redo();
            return true;
        case GLFW_KEY_0: fit_page(); return true;
        case GLFW_KEY_5: zoom_to(0.5); return true;
        case GLFW_KEY_7: zoom_to(0.75); return true;
        case GLFW_KEY_1: zoom_to(1); return true;
        case GLFW_KEY_2: zoom_to(2); return true;
        case GLFW_KEY_4: zoom_to(4); return true;
        case GLFW_KEY_8: zoom_to(8); return true;
        }
        return false;
    }

    bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        bool r = Screen::scroll_event(p, rel);
        update_status();
        return r;
    }

    bool save_png(const std::string &path) {
        int w = 0, h = 0;
        glfwGetFramebufferSize(glfw_window(), &w, &h);
        std::vector<unsigned char> buf((size_t) w * h * 4);
        draw_setup();
        draw_contents();
        draw_widgets();
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        stbi_flip_vertically_on_write(1);
        return stbi_write_png(path.c_str(), w, h, 4, buf.data(), w * 4) != 0;
    }

    /* A page point as the screen coordinates GLFW callbacks expect. */
    Vector2f screen_point(const Vector2f &pt) {
        Vector2f v = m_view->page_origin() + pt;
        auto pan = m_scroll->pan_offset();
        double z = m_scroll->zoom();
        Vector2i a = m_scroll->absolute_position();
        /* cursor_pos_callback_event() shifts by (-1, -2) and, on X11 and
         * Windows, divides by the pixel ratio. */
        Vector2f s((float) (a.x() + pan.x() + v.x() * z) + 1,
                   (float) (a.y() + pan.y() + v.y() * z) + 2);
#if !defined(NANOGUI_WAYLAND) && !defined(__APPLE__)
        s = s * pixel_ratio();
#endif
        return s;
    }

    /* A left-button drag between two page points, fed through Screen's
     * GLFW callbacks so it takes the real path (Screen -> ZoomScrollPanel
     * -> ComposerView). Used by --drag to test the pointer tool headlessly. */
    void synthetic_drag(const Vector2f &from, const Vector2f &to) {
        Vector2f a = screen_point(from), b = screen_point(to);
        cursor_pos_callback_event(a.x(), a.y());
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_PRESS, 0);
        for (int i = 1; i <= 8; ++i) {
            Vector2f p = a + (b - a) * (i / 8.f);
            cursor_pos_callback_event(p.x(), p.y());
        }
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_RELEASE, 0);
    }

    /* A plain left click at a page point. */
    void synthetic_click(const Vector2f &pt) {
        Vector2f s = screen_point(pt);
        cursor_pos_callback_event(s.x(), s.y());
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_PRESS, 0);
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_RELEASE, 0);
    }

    /* UTF-8 text as character events, as if typed. */
    void synthetic_type(const std::string &utf8) {
        for (size_t i = 0; i < utf8.size();) {
            unsigned char c = (unsigned char) utf8[i];
            int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            unsigned int cp = c & (0x7F >> n);
            for (int k = 1; k <= n && i + k < utf8.size(); ++k)
                cp = (cp << 6) | ((unsigned char) utf8[i + k] & 0x3F);
            char_callback_event(cp);
            i += (size_t) n + 1;
        }
    }

    /* A key press and release with modifiers. */
    void synthetic_key(int key, int mods) {
        key_callback_event(key, 0, GLFW_PRESS, mods);
        key_callback_event(key, 0, GLFW_RELEASE, mods);
    }

    /* --tool, --fill and --stroke: what the toolbox and palette would do. */
    void scripted(const std::string &what, const std::string &arg) {
        std::vector<std::string> f;
        for (size_t i = 0; i <= arg.size();) {
            size_t c = arg.find(',', i);
            f.push_back(arg.substr(i, c == std::string::npos ? std::string::npos : c - i));
            if (c == std::string::npos) break;
            i = c + 1;
        }
        if (what == "tool") {
            static const std::pair<const char *, ComposerView::Tool> tools[] = {
                {"pointer", ComposerView::Tool::Pointer}, {"text", ComposerView::Tool::Text},
                {"rotate", ComposerView::Tool::Rotate},   {"line", ComposerView::Tool::Line},
                {"rect", ComposerView::Tool::Rect},       {"ellipse", ComposerView::Tool::Ellipse},
                {"polygon", ComposerView::Tool::Polygon},
            };
            for (const auto &t : tools)
                if (f[0] == t.first)
                    apply_tool(t.second);
            return;
        }
        SwatchId id = kNoPaint;
        for (const Swatch &sw : m_view->document().swatches)
            if (sw.name == f[0])
                id = sw.id;
        if (what == "fill") {
            const float tint = f.size() > 1 ? (float) std::atof(f[1].c_str()) : 100.f;
            m_view->apply_shape_style([&](Shape &sh) { sh.fill = {id, tint}; });
        } else {
            const float w = f.size() > 1 ? (float) std::atof(f[1].c_str()) : 1.f;
            const std::string st = f.size() > 2 ? f[2] : "solid";
            const LineStyle ls = st == "dashed" ? LineStyle::Dashed : st == "dotted" ? LineStyle::Dotted
                               : st == "dashdot" ? LineStyle::DashDot : LineStyle::Solid;
            m_view->apply_shape_style([&](Shape &sh) { sh.stroke = {{id, 100.f}, w, ls}; });
        }
    }

    ComposerView *view() { return m_view; }
    void set_snap(bool on) {
        m_view->set_snap(on);
        m_snap_btn->set_pushed(on);
    }
    CheckBox *baselines_box() { return m_baselines; }
    CheckBox *loose_box() { return m_loose; }
    CheckBox *kern_box() { return m_kern; }

private:
    FontLibrary     m_fonts;
    Hyphenator      m_hyphenator;
    ZoomScrollPanel *m_scroll = nullptr;
    ComposerView    *m_view = nullptr;
    ComboBox        *m_align = nullptr;
    Dropdown        *m_zoom_menu = nullptr;
    Button          *m_undo_btn = nullptr, *m_redo_btn = nullptr;
    CheckBox        *m_kern = nullptr, *m_baselines = nullptr, *m_loose = nullptr;
    std::vector<std::pair<ComposerView::Tool, ToolButton *>> m_tool_btns;
    Button          *m_snap_btn = nullptr;
    FloatBox<float> *m_geom[5] = {};
    Dropdown        *m_fill = nullptr, *m_fill_tint = nullptr, *m_stroke = nullptr;
    Dropdown        *m_weight = nullptr, *m_style = nullptr;
    IntBox<int>     *m_sides = nullptr, *m_star = nullptr;
    std::vector<SwatchId> m_swatch_ids;
    Label           *m_status = nullptr;
};

static bool parse_pair(const char *s, float &a, float &b) {
    return std::sscanf(s, "%f,%f", &a, &b) == 2;
}

/* A headless input action, replayed through Screen's GLFW callbacks. */
struct Action {
    enum Kind { Drag, Click, Type, Key, Tool, Fill, Stroke } kind;
    std::array<float, 4> d{};
    std::string text;
    int key = 0, mods = 0;
};

/* "shift+ctrl+a", "enter", "b" -> GLFW key and modifiers. */
static bool parse_key(const std::string &spec, int &key, int &mods) {
    static const std::pair<const char *, int> named[] = {
        {"enter", GLFW_KEY_ENTER},       {"backspace", GLFW_KEY_BACKSPACE},
        {"delete", GLFW_KEY_DELETE},     {"left", GLFW_KEY_LEFT},
        {"right", GLFW_KEY_RIGHT},       {"up", GLFW_KEY_UP},
        {"down", GLFW_KEY_DOWN},         {"home", GLFW_KEY_HOME},
        {"end", GLFW_KEY_END},           {"escape", GLFW_KEY_ESCAPE},
        {"space", GLFW_KEY_SPACE},       {"tab", GLFW_KEY_TAB},
    };
    mods = 0;
    size_t i = 0;
    for (;;) {
        size_t plus = spec.find('+', i);
        std::string tok = spec.substr(i, plus == std::string::npos ? plus : plus - i);
        for (char &c : tok) c = (char) std::tolower((unsigned char) c);
        if (plus != std::string::npos) {
            if (tok == "shift") mods |= GLFW_MOD_SHIFT;
            else if (tok == "ctrl" || tok == "cmd" || tok == "super") mods |= SYSTEM_COMMAND_MOD;
            else if (tok == "alt") mods |= GLFW_MOD_ALT;
            else return false;
            i = plus + 1;
            continue;
        }
        if (tok.size() == 1 && tok[0] >= 'a' && tok[0] <= 'z') {
            key = GLFW_KEY_A + (tok[0] - 'a');
            return true;
        }
        for (const auto &n : named)
            if (tok == n.first) {
                key = n.second;
                return true;
            }
        return false;
    }
}

int main(int argc, char **argv) {
    std::string shot;
    double zoom = 0;
    float at_x = -1, at_y = -1, sel_a = -1, sel_b = -1;
    bool baselines = false, loose = false, no_kern = false, no_snap = false;
    std::vector<Action> actions;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        Action act;
        if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--drag" && i + 1 < argc &&
                 std::sscanf(argv[i + 1], "%f,%f:%f,%f", &act.d[0], &act.d[1], &act.d[2], &act.d[3]) == 4) {
            act.kind = Action::Drag;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--click" && i + 1 < argc && parse_pair(argv[i + 1], act.d[0], act.d[1])) {
            act.kind = Action::Click;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--type" && i + 1 < argc) {
            act.kind = Action::Type;
            act.text = argv[++i];
            actions.push_back(act);
        }
        else if (a == "--key" && i + 1 < argc && parse_key(argv[i + 1], act.key, act.mods)) {
            act.kind = Action::Key;
            actions.push_back(act);
            ++i;
        }
        else if ((a == "--tool" || a == "--fill" || a == "--stroke") && i + 1 < argc) {
            act.kind = a == "--tool" ? Action::Tool : a == "--fill" ? Action::Fill : Action::Stroke;
            act.text = argv[++i];
            actions.push_back(act);
        }
        else if (a == "--zoom" && i + 1 < argc) zoom = std::atof(argv[++i]);
        else if (a == "--at" && i + 1 < argc) parse_pair(argv[++i], at_x, at_y);
        else if (a == "--select" && i + 1 < argc) parse_pair(argv[++i], sel_a, sel_b);
        else if (a == "--baselines") baselines = true;
        else if (a == "--loose") loose = true;
        else if (a == "--no-kerning") no_kern = true;
        else if (a == "--no-snap") no_snap = true;
    }

    nanogui::init();
    {
        ref<PagemadeApp> app = new PagemadeApp();
        app->dec_ref();
        app->set_visible(true);
        for (int k = 0; k < 3; ++k) { app->perform_layout(); app->draw_all(); }

        if (baselines) { app->baselines_box()->set_checked(true); app->view()->set_show_baselines(true); }
        if (loose)     { app->loose_box()->set_checked(true); app->view()->set_show_loose_tight(true); }
        if (no_kern)   { app->kern_box()->set_checked(false);
                         app->for_each_run([](CharStyle &cs) { cs.kerning = false; }); }
        if (sel_a >= 0) app->view()->select_frame((size_t) sel_a, (size_t) sel_b);
        if (zoom > 0) {
            app->zoom_to(zoom);
            if (at_x >= 0) app->center_on(Vector2f(at_x, at_y));
        } else {
            app->fit_page();
        }
        if (no_snap)
            app->set_snap(false);
        for (const Action &act : actions) {
            app->perform_layout();
            app->draw_all();
            switch (act.kind) {
            case Action::Drag:
                app->synthetic_drag(Vector2f(act.d[0], act.d[1]), Vector2f(act.d[2], act.d[3]));
                break;
            case Action::Click:
                app->synthetic_click(Vector2f(act.d[0], act.d[1]));
                break;
            case Action::Type:
                app->synthetic_type(act.text);
                break;
            case Action::Key:
                app->synthetic_key(act.key, act.mods);
                break;
            case Action::Tool:
            case Action::Fill:
            case Action::Stroke:
                app->scripted(act.kind == Action::Tool ? "tool" : act.kind == Action::Fill
                              ? "fill" : "stroke", act.text);
                break;
            }
        }

        if (!shot.empty()) {
            for (int k = 0; k < 2; ++k) { app->perform_layout(); app->draw_all(); }
            bool ok = app->save_png(shot);
            std::printf("screenshot %s %s\n", ok ? "saved" : "FAILED", shot.c_str());
        } else {
            nanogui::mainloop(-1);
        }
    }
    nanogui::shutdown();
    return 0;
}
