/*
 * pagemade — page layout in the spirit of Aldus PageMaker.
 *
 * First slice: the composer view. One page on a pasteboard, a headline
 * and a two-column story, a toolbar with the pointer and text tools, a
 * zoom dropdown, undo/redo (Ctrl+Z / Ctrl+Shift+Z), a switch for
 * alignment, kerning and ligatures, and PageMaker's zoom shortcuts
 * (Ctrl/Cmd + 0 fit, 5 50%, 7 75%, 1 actual size, 2 200%, 4 400%, 8 800%).
 *
 *   pagemade
 *   pagemade --screenshot out.png [--zoom 4 --at 150,400] [--select 1,1]
 *            [--baselines] [--loose] [--no-kerning] [--tool text]
 *            [--drag x0,y0:x1,y1 ...] [--click x,y ...] [--type txt ...]
 *            [--key [mod+]name ...]
 *
 * --at is a page point (points from the page's top-left) to center on.
 * --drag presses at one page point and releases at another, through the
 * real event path, before the screenshot is taken. --click, --type and
 * --key (enter, backspace, left, ..., a-z; mods shift/ctrl/alt) exercise
 * the text tool the same way, in the order given.
 */
#include <nanogui/nanogui.h>
#include <nanogui/menu.h>
#include <nanogui/opengl.h>
#include <nanogui/zoomscrollpanel.h>
#include <GLFW/glfw3.h>

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

using namespace nanogui;
using namespace pagemade;

class PagemadeApp : public Screen {
public:
    PagemadeApp() : Screen(Vector2i(1100, 900), "pagemade") {
        inc_ref();
        set_theme_mode(ThemeMode::Light);
        register_default_fonts(m_fonts);

        auto *root_flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        RootWindow *window = new RootWindow(this, root_flex);

        Widget *toolbar = new Widget(window);
        toolbar->set_min_height(40);
        toolbar->set_height(40);
        toolbar->set_height_flex(SizeMode::Fixed);
        toolbar->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 8, 6));

        m_pointer_btn = new ToolButton(toolbar, FA_MOUSE_POINTER);
        m_text_btn = new ToolButton(toolbar, FA_I_CURSOR);
        m_pointer_btn->set_tooltip("Pointer tool (text blocks)");
        m_text_btn->set_tooltip("Text tool");
        m_pointer_btn->set_pushed(true);
        m_pointer_btn->set_callback([this] { apply_tool(ComposerView::Tool::Pointer); });
        m_text_btn->set_callback([this] { apply_tool(ComposerView::Tool::Text); });

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

        m_scroll = new ZoomScrollPanel(window, ZoomScrollPanel::ScrollTypes::Both);
        m_scroll->set_zoom_range(0.1, 16.0);
        m_scroll->set_zoom_enabled(true);
        m_scroll->set_height_flex(SizeMode::Expanding);
        root_flex->set_flex_item(m_scroll, FlexLayout::FlexItem(1.0f));

        m_view = new ComposerView(m_scroll, &m_fonts);
        m_view->on_recompose = [this] { update_status(); };
        m_view->on_tool_change = [this](ComposerView::Tool t) { sync_tool_buttons(t); };

        m_status = new Label(window, "", "sans", 16);
        m_status->set_min_height(24);
        m_status->set_height(24);
        m_status->set_height_flex(SizeMode::Fixed);

        m_view->set_document(sample_document());
        perform_layout();
    }

    /* The view's tool, mirrored in the toolbar's radio buttons. */
    void apply_tool(ComposerView::Tool t) {
        m_view->set_tool(t);
        sync_tool_buttons(t);
    }

    void sync_tool_buttons(ComposerView::Tool t) {
        m_pointer_btn->set_pushed(t == ComposerView::Tool::Pointer);
        m_text_btn->set_pushed(t == ComposerView::Tool::Text);
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
        for (TextFlow &f : m_view->document().flows)
            for (Paragraph &p : f.story.paragraphs)
                if (p.style.name == "Body text")
                    p.style.align = a;
        m_view->recompose();
    }

    template <typename F> void for_each_run(F fn) {
        m_view->push_undo();
        for (TextFlow &f : m_view->document().flows)
            for (Paragraph &p : f.story.paragraphs)
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
        std::snprintf(buf, sizeof buf, "  %zu stories, %zu lines, composed in %.2f ms%s   |   zoom %.0f%%",
                      comps.size(), lines, ms, overset ? "   |   story overset (red arrow)" : "",
                      m_scroll->zoom() * 100.0);
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

    ComposerView *view() { return m_view; }
    CheckBox *baselines_box() { return m_baselines; }
    CheckBox *loose_box() { return m_loose; }
    CheckBox *kern_box() { return m_kern; }

private:
    FontLibrary     m_fonts;
    ZoomScrollPanel *m_scroll = nullptr;
    ComposerView    *m_view = nullptr;
    ComboBox        *m_align = nullptr;
    Dropdown        *m_zoom_menu = nullptr;
    Button          *m_undo_btn = nullptr, *m_redo_btn = nullptr;
    CheckBox        *m_kern = nullptr, *m_baselines = nullptr, *m_loose = nullptr;
    ToolButton      *m_pointer_btn = nullptr, *m_text_btn = nullptr;
    Label           *m_status = nullptr;
};

static bool parse_pair(const char *s, float &a, float &b) {
    return std::sscanf(s, "%f,%f", &a, &b) == 2;
}

/* A headless input action, replayed through Screen's GLFW callbacks. */
struct Action {
    enum Kind { Drag, Click, Type, Key } kind;
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
    bool baselines = false, loose = false, no_kern = false, text_tool = false;
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
        else if (a == "--tool" && i + 1 < argc) text_tool = std::string(argv[++i]) == "text";
        else if (a == "--zoom" && i + 1 < argc) zoom = std::atof(argv[++i]);
        else if (a == "--at" && i + 1 < argc) parse_pair(argv[++i], at_x, at_y);
        else if (a == "--select" && i + 1 < argc) parse_pair(argv[++i], sel_a, sel_b);
        else if (a == "--baselines") baselines = true;
        else if (a == "--loose") loose = true;
        else if (a == "--no-kerning") no_kern = true;
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
        if (sel_a >= 0) app->view()->select((int) sel_a, (int) sel_b);
        if (zoom > 0) {
            app->zoom_to(zoom);
            if (at_x >= 0) app->center_on(Vector2f(at_x, at_y));
        } else {
            app->fit_page();
        }
        if (text_tool)
            app->apply_tool(ComposerView::Tool::Text);
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
