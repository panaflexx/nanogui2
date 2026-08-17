/*
    src/example1.cpp -- C++ version of an example application that shows
    how to use the various widget classes. For a Python implementation, see
    '../python/example1.py'.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/window.h>
#include <nanogui/layout.h>
#include <nanogui/label.h>
#include <nanogui/checkbox.h>
#include <nanogui/button.h>
#include <nanogui/menu.h>
#include <nanogui/toolbutton.h>
#include <nanogui/popupbutton.h>
#include <nanogui/combobox.h>
#include <nanogui/progressbar.h>
#include <nanogui/icons.h>
#include <nanogui/messagedialog.h>
#include <nanogui/textbox.h>
#include <nanogui/slider.h>
#include <nanogui/imagepanel.h>
#include <nanogui/imageview.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/split.h>
#include <nanogui/colorwheel.h>
#include <nanogui/colorpicker.h>
#include <nanogui/graph.h>
#include <nanogui/tabwidget.h>
#include <nanogui/treeview.h>
#include <nanogui/texture.h>
#include <nanogui/shader.h>
#include <nanogui/renderpass.h>
#include <nanogui/textarea.h>
#include <nanogui/texteditor.h>
#include <nanogui/folderdialog.h>
#include <nanogui/fluent.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#  pragma warning (disable: 4505) // don't warn about dead code in stb_image.h
#elif defined(__GNUC__)
#   pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>

using namespace nanogui;
using std::vector;
using std::function;

class ExampleApplication : public Screen {
public:
    std::vector<MenuItem *> m_menu_items;   /// All menu items
    std::vector<std::vector<std::string>> m_menu_aliases; /// Alternate names for menu item commands
    std::vector<MenuItem *> m_edit_items;   /// Menu items that are enabled iff we have an editable image
    MenuBar *m_menubar = NULL;

	void ask_to_quit() {
        Make<MessageDialog>(this, MessageDialog::Type::Information, "Warning!", "🛑 Quit?", "Yes", "No", true)
            .tap([this](MessageDialog* dlg) {
                dlg->set_callback([this](int result) { set_visible(result != 0); });
                dlg->request_focus();
            });
    }

    void CreateMovingMenuBar() {
        auto MenuBar1 = this->add<Window>("Try to move this around")
            .pos({15, 600})
            .layout(new BoxLayout(Orientation::Horizontal));

        auto FileBtn = MenuBar1->add<PopupButton>("File")
            .tap([](PopupButton* p) {
                p->popup()->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill));
                p->set_side(Popup::Left);
            });

        FileBtn->popup()->add<Button>("Folder Dialog")
            .callback([] {
                printf("**BEEP**\n");
            });
        FileBtn->popup()->add<Button>("Do File Stuff 2");
        FileBtn->popup()->add<Button>("Do File Stuff 3");
        FileBtn->popup()->add<Label>("-------------");
        FileBtn->popup()->add<Button>("short Stuff 4");
        FileBtn->popup()->add<Button>("short Stuff 5");
        FileBtn->popup()->add<Button>("Do File Stuff 6");

        auto EditBtn = MenuBar1->add<PopupButton>("Edit")
            .tap([](PopupButton* p) {
                p->popup()->set_layout(new BoxLayout(Orientation::Vertical));
                p->set_side(Popup::Bottom);
            });
        EditBtn->popup()->add<Button>("This is avery long button that will hit the borders when the popup is opened");
        EditBtn->popup()->add<Button>("Do File Stuff 2");

        auto HelpBtn = MenuBar1->add<PopupButton>("Help")
            .tap([](PopupButton* p) {
                p->popup()->set_layout(new BoxLayout(Orientation::Vertical));
                p->set_side(Popup::Right);
            });
        HelpBtn->popup()->add<Button>("Help");
        HelpBtn->popup()->add<Button>("About");
        HelpBtn->popup()->add<Button>("About2");
        HelpBtn->popup()->add<Button>("About3");
        HelpBtn->popup()->add<Button>("About4");
    }

    void CreateMenuBar()
    {
		m_menubar = Make<MenuBar>(this, "");
        auto menu = m_menubar->add_menu("File");

        auto add_item = [this, &menu](const vector<std::string> &aliases, int icon, const function<void(void)> &cb,
                                  const vector<Shortcut> &s = {{}}, bool edit = true, bool visible = true)
        {
            auto i = menu->add_item(aliases.front(), icon, s);
            i->set_callback(cb);
            i->set_visible(visible);
            m_menu_items.push_back(i);
            // add the menu name as a prefix to the aliases
            auto aliases_copy = aliases;
            aliases_copy[0]   = menu->caption() + ": " + aliases_copy[0];
            m_menu_aliases.push_back(aliases_copy);
            if (edit)
                m_edit_items.push_back(i);
        };

        add_item(
        {"New...", "New image"}, FA_FILE, [] { printf("New...\n"); }, {{SYSTEM_COMMAND_MOD, 'N'}}, false);
        add_item(
        {"Exit...", "Exit..."}, FA_FILE, [this] { ask_to_quit(); }, {{SYSTEM_COMMAND_MOD, 'Q'}}, false);

        menu = m_menubar->add_menu("Edit");

        add_item({"Undo", "Step back in history"}, FA_REPLY, [] { printf("undo()\n"); },
                 {{SYSTEM_COMMAND_MOD, 'Z'}});
        add_item({"Redo", "Step forward in history"}, FA_SHARE, [] { printf("redo()"); },
                 {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, 'Z'}});
        Make<Separator>(menu->popup());
        add_item({"Cut"}, FA_CUT, [] { printf("cut()"); }, {{SYSTEM_COMMAND_MOD, 'X'}});
        add_item({"Copy"}, FA_COPY, [] { printf("copy()"); }, {{SYSTEM_COMMAND_MOD, 'C'}});
        add_item({"Paste"}, FA_PASTE, [] { printf("paste()"); }, {{SYSTEM_COMMAND_MOD, 'V'}});

        menu = m_menubar->add_menu("View");
        add_item({"Light Theme", "Use light appearance"}, FA_SUN,
                 [this] { apply_theme_mode(ThemeMode::Light); },
                 {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, 'L'}}, false);
        add_item({"Dark Theme", "Use dark appearance"}, FA_MOON,
                 [this] { apply_theme_mode(ThemeMode::Dark); },
                 {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, 'D'}}, false);

		// Move menubar to front; width tracks the screen via MenuBar itself.
        move_window_to_front(m_menubar);
    }

    void apply_theme_mode(ThemeMode mode) {
        set_theme_mode(mode);
        // Keep any custom disabled-text tint used by this demo.
        if (m_theme) {
            if (mode == ThemeMode::Dark)
                m_theme->m_disabled_text_color = Color(0, 153, 230, 230);
            // Light palette already sets a sensible disabled color.
        }
        if (m_render_pass)
            m_render_pass->set_clear_color(0, m_background);
        perform_layout();
        redraw();
    }

    virtual bool resize_event(const Vector2i &size) override {
        // Keep the top menubar spanning the full client width.
        if (m_menubar) {
            m_menubar->set_width(size.x());
            m_menubar->perform_layout(m_nvg_context);
        }
        return Screen::resize_event(size);
    }

    void CreateMainWindow()
    {
        auto ButtonDemoWindow = this->add<Window>("", true)
            .pos({15, 40})
            .layout(new GroupLayout());

        ButtonDemoWindow->add<Label>("Push buttons", "sans-bold");

        ButtonDemoWindow->add<Button>("Plain button")
            .tooltip("short tooltip")
            .callback([] { std::cout << "plain pushed!" << std::endl; });

        ButtonDemoWindow->add<Button>("Styled 🎨", FA_ROCKET)
            .tooltip("This button has a fairly long tooltip...")
            .icon(FA_ROCKET)
			.background(Color(0, 0, 255, 25))
			.callback([] { std::cout << "styled pushed!" << std::endl; });

        ButtonDemoWindow->add<Label>("EMOJI! 🐺💺💆🐡🐛", "sans");

        ButtonDemoWindow->add<Label>("Toggle buttons", "sans-bold");
        ButtonDemoWindow->add<Button>("Toggle me")
            .flags(Button::ToggleButton)
            .change_callback([](bool state) {
                std::cout << "Toggle button state: " << state << std::endl;
            });

        ButtonDemoWindow->add<Label>("Radio buttons", "sans-bold");
        ButtonDemoWindow->add<Button>("Radio button 1").flags(Button::RadioButton);
        ButtonDemoWindow->add<Button>("Radio button 2").flags(Button::RadioButton);

        ButtonDemoWindow->add<Label>("A tool palette", "sans-bold");
        GridLayout* layout = new GridLayout(Orientation::Horizontal, 4, Alignment::Maximum, 0, 0);
        layout->set_col_alignment(Alignment::Fill);
        auto tools = ButtonDemoWindow->add<Widget>().layout(layout);

        tools->add<ToolButton>(FA_CLOUD);
        tools->add<ToolButton>(FA_FAST_FORWARD);
        tools->add<ToolButton>(FA_COMPASS);
        tools->add<ToolButton>(FA_UTENSILS);

        ButtonDemoWindow->add<Label>("Popup buttons", "sans-bold");
        auto popup_btn = ButtonDemoWindow->add<PopupButton>("Popup", FA_FLASK);
        auto popup = Make(popup_btn->popup()).layout(new GroupLayout());
        popup->add<Label>("Arbitrary widgets can be placed here");
        popup->add<CheckBox>("A check box");
        // popup right
        auto popup_btn_right = popup->add<PopupButton>("Recursive popup", FA_CHART_PIE);
        Make(popup_btn_right->popup()).layout(new GroupLayout())
            ->add<CheckBox>("Another check box");
        // popup left
        auto popup_btn_left = popup->add<PopupButton>("Recursive popup", FA_DNA)
            .tap([](PopupButton* p) { p->set_side(Popup::Side::Left); });
        Make(popup_btn_left->popup()).layout(new GroupLayout())
            ->add<CheckBox>("Another check box");
    }

    void CreateBasicWindow()
    {
        //int TempWidth = 100;
        //int TempHeight = 200;
        Window* BasicWidgetsWindow = Make<Window>(this, "Basic widgets", true)
            .pos({200, 15})
            .layout(new BoxLayout(Orientation::Vertical, Alignment::Middle, 0, 6));
        // BasicWidgetsWindow->set_fixed_size({ TempWidth/2, TempHeight/2 });

        // // attach a vertical scroll panel
        ScrollPanel* vscroll = Make<ScrollPanel>(BasicWidgetsWindow)
            .tap([](ScrollPanel* s) { s->set_scroll_type(ScrollPanel::ScrollTypes::Both); });

        // vscroll should only have *ONE* child. this is what `wrapper` is for
        auto wrapper = Make<Widget>(vscroll).layout(new GroupLayout()); // defaults: 2 columns
        //wrapper->set_fixed_size({ TempWidth, TempHeight });

        Make<Label>(wrapper, "Message dialog", "sans-bold");
        Widget* tools = Make<Widget>(wrapper)
            .layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 6));

        Make<Button>(tools, "Info").tap([this](Button* b) {
            b->set_callback([this] {
                Make<MessageDialog>(this, MessageDialog::Type::Information, "Title", "This is an information message")
                    .tap([](MessageDialog* d) {
                        d->set_callback([](int result) { std::cout << "Dialog result: " << result << std::endl; });
                    });
            });
        });
        Make<Button>(tools, "Warn").tap([this](Button* b) {
            b->set_callback([this] {
                Make<MessageDialog>(this, MessageDialog::Type::Warning, "Title", "This is a warning message")
                    .tap([](MessageDialog* d) {
                        d->set_callback([](int result) { std::cout << "Dialog result: " << result << std::endl; });
                    });
            });
        });
        Make<Button>(tools, "Ask").tap([this](Button* b) {
            b->set_callback([this] {
                Make<MessageDialog>(this, MessageDialog::Type::Warning, "Title", "This is a question message", "Yes", "No", true)
                    .tap([](MessageDialog* d) {
                        d->set_callback([](int result) { std::cout << "Dialog result: " << result << std::endl; });
                    });
            });
        });

#if defined(_WIN32)
        /// Executable is in the Debug/Release/.. subdirectory
        std::string resources_folder_path("./icons");
#else
        std::string resources_folder_path("./icons");
#endif
        std::vector<std::pair<int, std::string>> icons;

#if !defined(EMSCRIPTEN)
        try {
            icons = load_image_directory(m_nvg_context, resources_folder_path);
        }
        catch (const std::exception& e) {
            std::cerr << "Warning: " << e.what() << std::endl;
        }
#endif

        Make<Label>(wrapper, "Image panel & scroll panel", "sans-bold");
        PopupButton* image_panel_btn = Make<PopupButton>(wrapper, "Image Panel")
            .tap([](PopupButton* p) {
                p->set_icon(FA_IMAGES);
                p->set_side(Popup::Side::Left);
            });

        Popup* popup = Make(image_panel_btn->popup())
            .fixed_size({245, 150})
            .get();

        vscroll = Make<ScrollPanel>(popup);
        ImagePanel* img_panel = Make<ImagePanel>(vscroll)
            .tap([&icons](ImagePanel* p) { p->set_images(icons); });

        auto image_window = Make<Window>(this, "Selected image", true)
            .pos({710, 15})
            //.layout(new GroupLayout(3))
            .layout(new GridLayout(Orientation::Horizontal, 1, Alignment::Fill, 2, 2));

        // Create a Texture instance for each object
        for (auto& icon : icons) {
            Vector2i size;
            int n = 0;
            ImageHolder texture_data(
                stbi_load((icon.second + ".png").c_str(), &size.x(), &size.y(), &n, 0),
                stbi_image_free);
            assert(n == 4);

            Texture* tex = new Texture(
                Texture::PixelFormat::RGBA,
                Texture::ComponentFormat::UInt8,
                size,
                Texture::InterpolationMode::Trilinear,
                Texture::InterpolationMode::Nearest);

            tex->upload(texture_data.get());

            m_images.emplace_back(tex, std::move(texture_data));
        }

        ImageView* image_view = Make<ImageView>(image_window)
            .tap([this](ImageView* iv) {
                if (!m_images.empty())
                    iv->set_image(m_images[0].first);
                iv->center();
            });
        m_current_image = 0;

        img_panel->set_callback([this, image_view](int i) {
            std::cout << "Selected item " << i << std::endl;
            image_view->set_image(m_images[i].first);
            m_current_image = i;
            });

        image_view->set_pixel_callback(
            [this](const Vector2i& index, char** out, size_t size) {
                const Texture* texture = m_images[m_current_image].first.get();
                uint8_t* data = m_images[m_current_image].second.get();
                for (int ch = 0; ch < 4; ++ch) {
                    uint8_t value = data[(index.x() + index.y() * texture->size().x()) * 4 + ch];
                    snprintf(out[ch], size, "%i", (int)value);
                }
            }
        );

        Make<Label>(wrapper, "File dialog", "sans-bold");
        tools = Make<Widget>(wrapper)
            .layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 6));
        Make<Button>(tools, "Open").tap([](Button* b) {
            b->set_callback([&] {
                file_dialog(
                    { {"png", "Portable Network Graphics"}, {"txt", "Text file"} }, false, false, "c:");
            });
        });
        Make<Button>(tools, "Save").tap([](Button* b) {
            b->set_callback([&] {
                file_dialog(
                    { {"png", "Portable Network Graphics"}, {"txt", "Text file"} }, true, false, "c:");
            });
        });

        Make<Label>(wrapper, "Combo box", "sans-bold");
        Make<ComboBox>(wrapper, std::vector<std::string>{ "Combo box item 1", "Combo box item 2", "Combo box item 3", "Combo box item 3", "Combo box item 3", "Combo box item 3", "Combo box item 3", "Combo box item 3", "Combo box item 3" });
        Make<Label>(wrapper, "Check box", "sans-bold");
        Make<CheckBox>(wrapper, "Flag 1",
            std::function<void(bool)>([](bool state) { std::cout << "Check box 1 state: " << state << std::endl; }))
            .tap([](CheckBox* c) { c->set_checked(true); });
        Make<CheckBox>(wrapper, "Flag 2",
            std::function<void(bool)>([](bool state) { std::cout << "Check box 2 state: " << state << std::endl; }));
        Make<Label>(wrapper, "Progress bar", "sans-bold");
        m_progress = Make<ProgressBar>(wrapper);

        Make<Label>(wrapper, "Slider and text box", "sans-bold");

        Widget* panel = Make<Widget>(wrapper)
            .layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 20));

        Slider* slider = Make<Slider>(panel)
            .min_size({80, 0})
            .tap([](Slider* s) { s->set_value(0.5f); });

        TextBox* text_box = Make<TextBox>(panel)
            .fixed_size({60, 25})
            .font_size(20)
            .tap([](TextBox* t) {
                t->set_value("50");
                t->set_units("%");
                t->set_editable(false);
                t->set_alignment(TextBox::Alignment::Right);
            });

        slider->set_callback([text_box](float value) {
            text_box->set_value(std::to_string((int)(value * 100)));
        });
        slider->set_final_callback([&](float value) {
            std::cout << "Final slider value: " << (int)(value * 100) << std::endl;
        });
    }
    void CreateMiscWindow()
    {
        Window* MiscWidgetsWindow = Make<Window>(this, "Misc. widgets", true)
            .pos({425, 15})
            .layout(new GroupLayout());

        TabWidget* tab_widget = Make<TabWidget>(MiscWidgetsWindow);

        Widget* layer = Make<Widget>(tab_widget).layout(new GroupLayout());
        tab_widget->append_tab("Color Wheel", layer);

        // Fill the tab with widgets via Make<>().
        Make<Label>(layer, "Color wheel widget", "sans-bold");
        Make<ColorWheel>(layer);

        layer = Make<Widget>(tab_widget).layout(new GroupLayout());
        tab_widget->append_tab("Function Graph", layer);

        Make<Label>(layer, "Function graph widget", "sans-bold");

        Graph* graph = Make<Graph>(layer, "Some Function")
                .tap([](Graph* g) {
                    g->set_header("E = 2.35e-3");
                    g->set_footer("Iteration 89");
                });

        std::vector<float>& func = graph->values();
        func.resize(100);
        for (int i = 0; i < 100; ++i)
            func[i] = 0.5f * (0.5f * std::sin(i / 10.f) +
                0.5f * std::cos(i / 23.f) + 1);

        // Dummy tab used to represent the last tab button.
        Widget* PlusTab = Make<Widget>(tab_widget);

        int plus_id = tab_widget->append_tab("+", PlusTab);
        // A simple counter.
        int counter = 1;
        tab_widget->set_callback([tab_widget, this, counter, plus_id](int id) mutable {
            if (id == plus_id) {
                // When the "+" tab has been clicked, simply add a new tab.
                std::string tab_name = "Dynamic " + std::to_string(counter);
                Widget* layer_dyn = Make<Widget>(tab_widget).layout(new GroupLayout());
                int new_id = tab_widget->insert_tab(tab_widget->tab_count() - 1,
                    tab_name, layer_dyn);
                Make<Label>(layer_dyn, "Function graph widget", "sans-bold");
                Graph* graph_dyn = Make<Graph>(layer_dyn, "Dynamic function")
                    .tap([new_id, counter](Graph* g) {
                        g->set_header("E = 2.35e-3");
                        g->set_footer("Iteration " + std::to_string(new_id * counter));
                    });
                std::vector<float>& func_dyn = graph_dyn->values();
                func_dyn.resize(100);
                for (int i = 0; i < 100; ++i)
                    func_dyn[i] = 0.5f *
                    std::abs((0.5f * std::sin(i / 10.f + counter) +
                        0.5f * std::cos(i / 23.f + 1 + counter)));
                ++counter;
                tab_widget->set_selected_id(new_id);

                // We must invoke the layout manager after adding tabs dynamically
                perform_layout();
            }
            });

        // A button to go back to the first tab and scroll the window.
        Widget* panel = Make<Widget>(MiscWidgetsWindow)
            .layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 6));
        Make<Label>(panel, "Jump to tab: ");

        auto ib = Make<IntBox<int>>(panel)
            .enabled(true)
            .min_size({0, 22})
            .get();

        Make<Button>(panel, "", FA_FORWARD)
            .fixed_size({22, 22})
            .tap([tab_widget, ib](Button* b) {
                b->set_callback([tab_widget, ib] {
                    int value = ib->value();
                    if (value >= 0 && value < tab_widget->tab_count())
                        tab_widget->set_selected_index(value);
                });
            });
    }
    void CreateSmallWindow()
    {
        GridLayout* layout = new GridLayout(Orientation::Horizontal, 2,
            Alignment::Middle, 15, 5);
        layout->set_col_alignment({ Alignment::Maximum, Alignment::Fill });
        layout->set_spacing(Orientation::Horizontal, 10);

        Window* GridWindow = Make<Window>(this, "Grid of small widgets", true)
            .pos({200, 520})
            .layout(layout);

        /* FP widget */ {
            Make<Label>(GridWindow, "Floating point :", "sans-bold");
            Make<TextBox>(GridWindow)
                .enabled(true)
                .fixed_size({100, 20})
                .font_size(16)
                .tap([](TextBox* t) {
                    t->set_value("50");
                    t->set_units("GiB");
                    t->set_default_value("0.0");
                    t->set_format("[-]?[0-9]*\\.?[0-9]+");
                });
        }

        /* Positive integer widget */ {
            Make<Label>(GridWindow, "Positive integer :", "sans-bold");
            Make<IntBox<int>>(GridWindow)
                .enabled(true)
                .fixed_size({100, 20})
                .font_size(16)
                .tap([](IntBox<int>* ib) {
                    ib->set_value(50);
                    ib->set_units("Mhz");
                    ib->set_default_value("0");
                    ib->set_format("[1-9][0-9]*");
                    ib->set_spinnable(true);
                    ib->set_min_value(1);
                    ib->set_value_increment(2);
                });
        }

        /* Checkbox widget */ {
            Make<Label>(GridWindow, "Checkbox :", "sans-bold");
            Make<CheckBox>(GridWindow, "Check me")
                .font_size(16)
                .tap([](CheckBox* c) { c->set_checked(true); });
        }

        Make<Label>(GridWindow, "Combo box :", "sans-bold");
        Make<ComboBox>(GridWindow, std::vector<std::string>{ "Item 1", "Item 2", "Item 3" })
            .font_size(16)
            .fixed_size({100, 20});

        Make<Label>(GridWindow, "Color picker :", "sans-bold");
        auto cp = Make<ColorPicker>(GridWindow, Color{ 255, 120, 0, 255 })
            .fixed_size({100, 20})
            .tap([](ColorPicker* p) {
                p->set_final_callback([](const Color& c) {
                    std::cout << "ColorPicker final callback: ["
                        << c.r() << ", " << c.g() << ", "
                        << c.b() << ", " << c.w() << "]" << std::endl;
                });
            })
            .get();

        // setup a fast callback for the color picker widget on a new window
        // for demonstrative purposes
        GridLayout* layout2 = new GridLayout(Orientation::Horizontal, 2,
            Alignment::Middle, 15, 5);
        layout2->set_col_alignment({ Alignment::Maximum, Alignment::Fill });
        layout2->set_spacing(Orientation::Horizontal, 10);

        Window* ColorPickerWindow = Make<Window>(this, "Color Picker Fast Callback", true)
            .pos({425, 500})
            .layout(layout2);

        Make<Label>(ColorPickerWindow, "Combined: ");
        Button* b = Make<Button>(ColorPickerWindow, "ColorWheel", FA_INFINITY);
        Make<Label>(ColorPickerWindow, "Red: ");
        auto red_int_box   = Make<IntBox<int>>(ColorPickerWindow).enabled(false).get();
        Make<Label>(ColorPickerWindow, "Green: ");
        auto green_int_box = Make<IntBox<int>>(ColorPickerWindow).enabled(false).get();
        Make<Label>(ColorPickerWindow, "Blue: ");
        auto blue_int_box  = Make<IntBox<int>>(ColorPickerWindow).enabled(false).get();
        Make<Label>(ColorPickerWindow, "Alpha: ");
        auto alpha_int_box = Make<IntBox<int>>(ColorPickerWindow).get();

        cp->set_callback([b, red_int_box, blue_int_box, green_int_box, alpha_int_box](const Color& c) {
            b->set_background_color(c);
            b->set_text_color(c.contrasting_color());
            int red = (int)(c.r() * 255.0f);
            red_int_box->set_value(red);
            int green = (int)(c.g() * 255.0f);
            green_int_box->set_value(green);
            int blue = (int)(c.b() * 255.0f);
            blue_int_box->set_value(blue);
            int alpha = (int)(c.w() * 255.0f);
            alpha_int_box->set_value(alpha);
            });

    }
    void CreateTreeViewWindow()
    {
        Window* TreeViewWindow = Make<Window>(this, "TreeView Example", true)
            .pos({700, 350})
            .layout(new BoxLayout(Orientation::Vertical, Alignment::Middle, 15));

        ScrollPanel* scroll = Make<ScrollPanel>(TreeViewWindow)
            .tap([](ScrollPanel* s) { s->set_scroll_type(ScrollPanel::ScrollTypes::Both); });

        Widget* Container = Make<Widget>(scroll)
            .layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle));

        TreeView* TreeViewWidget = Make<TreeView>(Container)
            .fixed_size({100, 200});

        NanoTree* MyTree = new NanoTree();
        MyTree->set_root("1");
        MyTree->add_node("1", "1,1");
        MyTree->add_node("1", "1,2");
        MyTree->add_node("1,1", "1,1,1");
        MyTree->add_node("1,2", "1,1,2");
        MyTree->add_node("1,2", "1,1,3");
        MyTree->add_node("1,2", "1,1,4");
        MyTree->add_node("1,1,2", "1,1,2,1");
        MyTree->add_node("1,1,2", "1,1,2,2");
        MyTree->add_node("1,1,2", "1,1,2,3");
        MyTree->add_node("1,1,2", "1,1,2,4");
        MyTree->add_node("1,1,2", "1,1,2,5");
        MyTree->add_node("1,1,2", "1,1,2,6");
        MyTree->Objects["1"]->Name = MyTree->Objects["1"]->KeyString;
        MyTree->Objects["1,1"]->Name = MyTree->Objects["1,1"]->KeyString;
        MyTree->Objects["1,2"]->Name = MyTree->Objects["1,2"]->KeyString;
        MyTree->Objects["1,1,1"]->Name = MyTree->Objects["1,1,1"]->KeyString;
        MyTree->Objects["1,1,2"]->Name = MyTree->Objects["1,1,2"]->KeyString;
        MyTree->Objects["1,1,3"]->Name = MyTree->Objects["1,1,3"]->KeyString;
        MyTree->Objects["1,1,4"]->Name = MyTree->Objects["1,1,4"]->KeyString;
        MyTree->Objects["1,1,2,1"]->Name = MyTree->Objects["1,1,2,1"]->KeyString;
        MyTree->Objects["1,1,2,2"]->Name = MyTree->Objects["1,1,2,2"]->KeyString;
        MyTree->Objects["1,1,2,3"]->Name = MyTree->Objects["1,1,2,3"]->KeyString;
        MyTree->Objects["1,1,2,4"]->Name = MyTree->Objects["1,1,2,4"]->KeyString;
        MyTree->Objects["1,1,2,5"]->Name = MyTree->Objects["1,1,2,5"]->KeyString;
        MyTree->Objects["1,1,2,6"]->Name = MyTree->Objects["1,1,2,6"]->KeyString;

        MyTree->Objects["1"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,2"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,2"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,1"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,1"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,2"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,2"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,3"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,3"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,4"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,4"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,2,1"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,2,1"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,2,2"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,2,2"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,2,3"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,2,3"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,2,4"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,2,4"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,2,5"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,2,5"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };
        MyTree->Objects["1,1,2,6"]->CallBack = [=] {TreeButtonSelected->set_caption(MyTree->Objects["1,1,2,6"]->Name); TreeViewWindow->perform_layout(screen()->nvg_context()); };

        TreeViewWidget->set_items(MyTree);

        Make<Label>(Container, "ButtonHit:");
        TreeButtonSelected = Make<Label>(Container, "");

        AdvancedGridLayout* layout = new AdvancedGridLayout();
        Widget* AdvWid = Make<Widget>(Container).layout(layout);

        layout->append_row(5, 1);
        layout->append_row(100, 0);
        layout->append_row(30, 1);
        layout->append_col(20, 1);
        layout->append_col(50, 1);
        layout->append_col(70, 1);
        layout->append_col(100, 1);

        layout->set_anchor(Make<Button>(AdvWid, "b0"), AdvancedGridLayout::Anchor(0, 0));
        layout->set_anchor(Make<Button>(AdvWid, "b1"), AdvancedGridLayout::Anchor(0, 1));
        layout->set_anchor(Make<Button>(AdvWid, "c0"), AdvancedGridLayout::Anchor(1, 0));
        layout->set_anchor(Make<Button>(AdvWid, "c0"), AdvancedGridLayout::Anchor(0, 2));
        layout->set_anchor(Make<Button>(AdvWid, "c0"), AdvancedGridLayout::Anchor(1, 2));
        layout->set_anchor(Make<Button>(AdvWid, "c0"), AdvancedGridLayout::Anchor(1, 1, 2, 2));
    }
    void CreateControlsDefault()
    {
        // ----- Editor window ------------------------------------------------
        // Replaces the old read-only TextArea "console" with the new
        // TextEditor widget in Code mode. The TextEditor scrolls its own
        // content (no ScrollPanel wrapper needed) and supports caret,
        // selection, keyboard editing and per-run syntax colors.
        Window* Editor_TopWindow = Make<Window>(this, "Editor", true)
            .pos({700, 400})
            .size({560, 320})
            .visible(true)
            .layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

        // TextEditor lives in a retained container (set_layout enables cache).
        // Events / caret / text callbacks mark the container's display list dirty.
        auto* editorCache = Make<Widget>(Editor_TopWindow)
            .layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0))
            .get();
        editorCache->set_grow_parent(true);
        editorCache->set_size(Editor_TopWindow->size());
        editorCache->set_cached(true);

        TextEditor* editor = Make<TextEditor>(editorCache,
                                              TextEditor::Mode::Code)
            .tap([](TextEditor* e) {
                e->set_padding(8);
                e->set_show_line_numbers(true);
                e->set_tab_width(4);
                e->set_expand_tab(true);
                e->set_background_color(Color(30, 30, 34, 255));
                e->set_caret_color(Color(220, 220, 220, 255));
                e->set_selection_color(Color(70, 110, 180, 130));
                e->set_current_line_color(Color(255, 255, 255, 14));
                e->set_gutter_color(Color(38, 38, 44, 255));
                e->set_line_number_color(Color(120, 120, 130, 255));
            });

        // Programmatic invalidation (set_plain_text below, run-rebuild
        // for syntax highlighting, etc.) needs to bump the cache too.
        editor->change_callback = [editorCache] { editorCache->cache_dirty(); };
        editor->caret_callback  = [editorCache](TextEditor::Position) {
            editorCache->cache_dirty();
        };
        editor->set_min_height(editorCache->size().y());
        editor->set_grow_parent(true);

        // Sample C source to demo monospace rendering + line numbers.
        const std::string sample =
            "// example.c -- TextEditor demo source\n"
            "#include <stdio.h>\n"
            "\n"
            "int factorial(int n) {\n"
            "\tif (n <= 1)\n"
            "\t\treturn 1;\n"
            "\treturn n * factorial(n - 1);\n"
            "}\n"
            "\n"
            "int main(int argc, char** argv) {\n"
            "\tfor (int i = 0; i < 5; ++i) {\n"
            "\t\tprintf(\"%d! = %d\\n\", i, factorial(i));\n"
            "\t}\n"
            "\treturn 0;\n"
            "}\n";
        editor->set_plain_text(sample);

        // -------------------------------------------------------------------
        // Demonstrate the per-run color model by recoloring a handful of
        // tokens on the first line.  This is the same hook a real
        // syntax highlighter (tree-sitter) will use later: rebuild a
        // paragraph's `runs` vector with each token in its own Style.
        // -------------------------------------------------------------------
        if (auto doc = editor->document(); doc && !doc->paragraphs.empty()) {
            Style comment;  comment.monospace = true;
            comment.fontSize  = editor->code_style().fontSize;
            comment.fgColor   = nvgRGBA(110, 160, 110, 255);  // green

            Style preproc;  preproc.monospace = true;
            preproc.fontSize  = editor->code_style().fontSize;
            preproc.fgColor   = nvgRGBA(200, 130, 200, 255);  // magenta

            Style keyword;  keyword.monospace = true;
            keyword.fontSize  = editor->code_style().fontSize;
            keyword.fgColor   = nvgRGBA(230, 200,  90, 255);  // amber

            Style ident;    ident.monospace = true;
            ident.fontSize  = editor->code_style().fontSize;
            ident.fgColor   = nvgRGBA(110, 170, 220, 255);  // blue

            Style normal;   normal.monospace = true;
            normal.fontSize  = editor->code_style().fontSize;
            normal.fgColor   = editor->code_style().fgColor;

            // Line 0: the // comment
            doc->paragraphs[0]->runs.clear();
            doc->paragraphs[0]->addText(
                "// example.c -- TextEditor demo source", comment);

            // Line 1: #include <stdio.h>
            if (doc->paragraphs.size() > 1) {
                doc->paragraphs[1]->runs.clear();
                doc->paragraphs[1]->addText("#include ", preproc);
                doc->paragraphs[1]->addText("<stdio.h>", normal);
            }

            // Line 3: int factorial(int n) {
            if (doc->paragraphs.size() > 3) {
                doc->paragraphs[3]->runs.clear();
                doc->paragraphs[3]->addText("int",        keyword);
                doc->paragraphs[3]->addText(" ",          normal);
                doc->paragraphs[3]->addText("factorial",  ident);
                doc->paragraphs[3]->addText("(",          normal);
                doc->paragraphs[3]->addText("int",        keyword);
                doc->paragraphs[3]->addText(" n) {",      normal);
            }
        }
        editorCache->cache_dirty();
    }

    // -----------------------------------------------------------------------
    // Minimal Markdown -> Document parser.
    //
    // Same shape as the one in nanovg-colorfont-atlas.cpp; lifted here so
    // example1 stays self-contained.  Supports:
    //    # / ## / ### headers   **bold**   *italic*   `code`
    //    ```fenced code blocks```
    // Blank lines break paragraphs.
    // -----------------------------------------------------------------------
    static void parse_markdown(nanogui::Document& doc, const std::string& md) {
        doc.paragraphs.clear();

        // Dark text on light preview background.
        Style normal;  normal.fontSize = 16.0f;
                       normal.fgColor  = nvgRGBA( 30,  30,  35, 255);
        Style bold     = normal; bold.bold      = true;
        Style italic   = normal; italic.italic  = true;
        Style code     = normal; code.monospace = true; code.fontSize = 14.0f;
                                code.fgColor   = nvgRGBA( 20,  20,  25, 255);
                                code.bgColor   = nvgRGBA(225, 225, 232, 255);
        Style h1 = normal; h1.fontSize = 28.0f; h1.bold = true;
        Style h2 = normal; h2.fontSize = 22.0f; h2.bold = true;
        Style h3 = normal; h3.fontSize = 18.0f; h3.bold = true;

        std::istringstream iss(md);
        std::string line;
        Paragraph* current = nullptr;
        bool       inCode  = false;
        std::string codeBuf;

        auto append_inline = [&](Paragraph* p, const std::string& text) {
            size_t i = 0;
            while (i < text.size()) {
                if (i + 1 < text.size() && text[i] == '*' && text[i+1] == '*') {
                    size_t s = i + 2;
                    size_t e = text.find("**", s);
                    if (e != std::string::npos) {
                        if (e > s) p->addText(text.substr(s, e - s), bold);
                        i = e + 2; continue;
                    }
                } else if (text[i] == '*' && (i == 0 || text[i-1] != '*')) {
                    size_t s = i + 1;
                    size_t e = text.find('*', s);
                    if (e != std::string::npos && e > s) {
                        p->addText(text.substr(s, e - s), italic);
                        i = e + 1; continue;
                    }
                } else if (text[i] == '`') {
                    size_t s = i + 1;
                    size_t e = text.find('`', s);
                    if (e != std::string::npos) {
                        if (e > s) p->addText(text.substr(s, e - s), code);
                        i = e + 1;
                        if (i < text.size() && std::isspace((unsigned char)text[i])) {
                            p->addText(" ", normal);
                            while (i < text.size()
                                   && std::isspace((unsigned char)text[i])) ++i;
                        }
                        continue;
                    }
                }
                size_t s = i;
                while (i < text.size() && text[i] != '*' && text[i] != '`') ++i;
                if (i > s) p->addText(text.substr(s, i - s), normal);
                else ++i;
            }
        };

        while (std::getline(iss, line)) {
            while (!line.empty() && std::isspace((unsigned char)line.back()))
                line.pop_back();

            if (line.empty()) {
                if (inCode) codeBuf += '\n';
                else        current = nullptr;
                continue;
            }

            if (line.size() >= 3 && line.substr(0, 3) == "```") {
                if (inCode) {
                    if (!codeBuf.empty()) {
                        auto* p = doc.addParagraph();
                        p->addText(codeBuf, code);
                    }
                    codeBuf.clear();
                    inCode  = false;
                } else {
                    inCode  = true;
                    codeBuf.clear();
                }
                current = nullptr;
                continue;
            }
            if (inCode) { codeBuf += line + "\n"; continue; }

            if (line[0] == '#') {
                size_t lvl = 0;
                while (lvl < line.size() && line[lvl] == '#') ++lvl;
                if (lvl > 0 && lvl < line.size()
                    && std::isspace((unsigned char)line[lvl])) {
                    const Style& hs = (lvl == 1) ? h1 : (lvl == 2) ? h2 : h3;
                    auto* p = doc.addParagraph();
                    p->addText(line.substr(lvl + 1), hs);
                    current = nullptr;
                    continue;
                }
            }

            if (!current) current = doc.addParagraph();
            else          current->addText(" ", normal); // soft wrap join
            append_inline(current, line);
        }

        if (inCode && !codeBuf.empty()) {
            auto* p = doc.addParagraph();
            p->addText(codeBuf, code);
        }
        if (doc.paragraphs.empty()) doc.addParagraph();
    }

    // -----------------------------------------------------------------------
    // CreateMarkdownDoc -- split-window markdown editor.
    //
    // Left pane:  TextEditor in Code mode -> the raw markdown source.
    //             (This is what the user edits.)
    // Right pane: TextEditor in RichText mode -> rendered preview.
    //             (Rebuilt live whenever the source changes.)
    //
    // NOTE: Caret/selection in RichText mode is intentionally not yet
    // implemented in TextEditor, so the right pane is read-only display.
    // -----------------------------------------------------------------------
    void CreateMarkdownDoc()
    {
        // FlexLayout on the Window: one main-axis item (the cache), with
        // AlignItems::Stretch filling the cross axis and flex_grow=1 on
        // the cache filling the main axis.  Result: the cache always
        // tracks the Window's content area as it's resized.
        Window* mdWindow = Make<Window>(this, "Markdown", true)
            .pos({60, 460})
            .size({820, 380})
            .visible(true)
            .layout(new GroupLayout());

        // Window body panel retains the Split via display list (layout => cache).
        // Split drag and editor caret/text callbacks call cache_dirty().
        auto* mdCache = Make<Widget>(mdWindow)
            .layout(new BoxLayout(Orientation::Horizontal, Alignment::Fill, 0, 0))
            .get();
        mdCache->set_grow_parent(true);
        mdCache->set_min_size(mdWindow->size());
        mdCache->set_cached(true);

        // Vertical drag bar separating the two panes (Split::Horizontal
        // means "widgets side-by-side, vertical divider").
        auto* split = new Split(mdCache, Split::Orientation::Horizontal);
        //split->set_fixed_size({800, 360});
        split->set_min_size(mdCache->size());
        //split->set_height(600);
        split->set_drag_position(0.5f);
        // Grow the Split (and therefore the editor panes) along with the
        // parent Window when it is resized.
        split->set_grow_parent(true);

        // -- left: source editor (Code mode) --------------------------------
        auto* source = new TextEditor(split, TextEditor::Mode::Code);
        source->set_padding(8);
        source->set_show_line_numbers(true);
        source->set_tab_width(4);
        source->set_expand_tab(true);
        source->set_background_color(Color(28, 28, 32, 255));
        // Light-gray foreground so the source is readable on the dark bg.
        {
            Style cs = source->code_style();
            cs.fgColor = nvgRGBA(215, 215, 220, 255);
            source->set_code_style(cs);
        }

        // -- right: rendered preview (RichText mode) ------------------------
        auto* preview = new TextEditor(split, TextEditor::Mode::RichText);
        preview->set_padding(12);
        preview->set_show_line_numbers(false);
        preview->set_read_only(true);
        preview->set_background_color(Color(245, 245, 245, 255));
        // Foreground for the preview is set per-run by the markdown parser.

        const std::string defaultMd =
            "# Markdown Demo\n"
            "\n"
            "Edit the **source** on the *left* and watch the rendered\n"
            "preview on the right update live.\n"
            "🐺💺💆🐡🐛\n"
            "\n"
            "## Inline styles\n"
            "\n"
            "This paragraph mixes **bold**, *italic* and `inline code`\n"
            "all in one line.\n"
            "\n"
            "### Fenced code block\n"
            "\n"
            "```\n"
            "int main() {\n"
            "    printf(\"hello, markdown\\n\");\n"
            "    return 0;\n"
            "}\n"
            "```\n"
            "\n"
            "## Notes\n"
            "\n"
            "The Document model is the same one used by\n"
            "`nanovg-colorfont-atlas.cpp`, so anything renderable\n"
            "there also renders here.\n";

        source->set_plain_text(defaultMd);

        // -- rebuild the preview document whenever the source changes ------
        auto rebuild = [source, preview, mdCache]() {
            auto pdoc = preview->document();
            if (!pdoc) return;
            parse_markdown(*pdoc, source->plain_text());
            mdCache->cache_dirty();
        };

        rebuild();
        source->change_callback = rebuild;
        // Caret movement (no content change) still needs a repaint.
        source->caret_callback = [mdCache](TextEditor::Position) {
            mdCache->cache_dirty();
        };
        // Selection drag in the preview bypasses the container's mouse_drag_event
        // (Screen dispatches directly to the drag widget), so the retained display list
        // never gets invalidated during the drag.  A caret_callback here fires
        // on every set_caret() call — including mid-drag — and keeps the cache
        // in sync so the selection highlight updates in real time.
        preview->caret_callback = [mdCache](TextEditor::Position) {
            mdCache->cache_dirty();
        };
    }

	// Necessary for closing open menus
	bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down, int modifiers)
    {
        // close all popup menus
        if (down)
        {
            if(Widget *w = find_widget(p)) {
                auto clicked_menu = dynamic_cast<PopupMenu *>(w->window());
                if (!clicked_menu)
                {
                    bool closed_a_menu = false;
                    for (auto it = m_children.begin(); it != m_children.end(); ++it)
                    {
                        Widget *child = *it;
                        if (child->visible() && !child->contains(p - m_pos) && dynamic_cast<PopupMenu *>(child))
                        {
                            child->set_visible(false);
                            closed_a_menu = true;
                        }
                    }
                    if (closed_a_menu)
                        return true;
                }
            }
        }
        return Screen::mouse_button_event(p, button, down, modifiers);
    }

public:
    ExampleApplication() : Screen(Vector2i(1024, 768), "NanoGUI Test") {
        inc_ref();

        // Start in dark mode (classic example1 look); switch via View menu.
        set_theme_mode(ThemeMode::Dark);
        if (m_theme)
            m_theme->m_disabled_text_color = Color(0, 153, 230, 230);

        CreateMenuBar();
        CreateMovingMenuBar();
        CreateMainWindow();
        CreateBasicWindow();
        CreateMiscWindow();
        CreateSmallWindow();
        //CreateTreeViewWindow();
        CreateControlsDefault();
        CreateMarkdownDoc();

        perform_layout();

        /* All NanoGUI widgets are initialized at this point. Now
           create shaders to draw the main window contents.

           NanoGUI comes with a simple wrapper around OpenGL 3, which
           eliminates most of the tedious and error-prone shader and buffer
           object management.
        */

        m_render_pass = new RenderPass({ this });
        m_render_pass->set_clear_color(0, m_background);

        m_shader = new Shader(
            m_render_pass,

            /* An identifying name */
            "a_simple_shader",

#if defined(NANOGUI_USE_OPENGL)
            R"(/* Vertex shader */
            #version 330
            uniform mat4 mvp;
            in vec3 position;
            void main() {
                gl_Position = mvp * vec4(position, 1.0);
            })",

            /* Fragment shader */
            R"(#version 330
            out vec4 color;
            uniform float intensity;
            void main() {
                color = vec4(vec3(intensity), 1.0);
            })"
#elif defined(NANOGUI_USE_GLES)
            R"(/* Vertex shader */
            precision highp float;
            uniform mat4 mvp;
            attribute vec3 position;
            void main() {
                gl_Position = mvp * vec4(position, 1.0);
            })",

            /* Fragment shader */
            R"(precision highp float;
            uniform float intensity;
            void main() {
                gl_FragColor = vec4(vec3(intensity), 1.0);
            })"
#elif defined(NANOGUI_USE_METAL)
            R"(using namespace metal;
            struct VertexOut {
                float4 position [[position]];
            };

            vertex VertexOut vertex_main(const device packed_float3 *position,
                                         constant float4x4 &mvp,
                                         uint id [[vertex_id]]) {
                VertexOut vert;
                vert.position = mvp * float4(position[id], 1.f);
                return vert;
            })",

            /* Fragment shader */
            R"(using namespace metal;
            fragment float4 fragment_main(const constant float &intensity) {
                return float4(intensity);
            })"
#endif
        );

        uint32_t indices[3 * 2] = {
            0, 1, 2,
            2, 3, 0
        };

        float positions[3 * 4] = {
            -1.f, -1.f, 0.f,
            1.f, -1.f, 0.f,
            1.f, 1.f, 0.f,
            -1.f, 1.f, 0.f
        };

        m_shader->set_buffer("indices", VariableType::UInt32, { 3 * 2 }, indices);
        m_shader->set_buffer("position", VariableType::Float32, { 4, 3 }, positions);
        m_shader->set_uniform("intensity", 0.5f);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        if (action == GLFW_PRESS && m_menubar &&
            m_menubar->process_shortcuts(modifiers, key))
            return true;
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            set_visible(false);
            return true;
        }
        return false;
    }

    virtual void draw(NVGcontext* ctx) {
        /* Animate the scrollbar */
       // m_progress->set_value(std::fmod((float)glfwGetTime() / 10, 1.0f));

        /* Draw the user interface */
        Screen::draw(ctx);
    }

    virtual void draw_contents() {
        Matrix4f mvp = Matrix4f::scale(Vector3f(
            (float)m_size.y() / (float)m_size.x() * 0.25f, 0.25f, 0.25f)) *
            Matrix4f::rotate(Vector3f(0, 0, 1), (float)glfwGetTime());

        m_shader->set_uniform("mvp", mvp);

        m_render_pass->resize(framebuffer_size());
        m_render_pass->begin();

        m_shader->begin();
        m_shader->draw_array(Shader::PrimitiveType::Triangle, 0, 6, true);
        m_shader->end();

        m_render_pass->end();
    }
private:
    ProgressBar* m_progress;
    ref<Shader> m_shader;
    ref<RenderPass> m_render_pass;

    Label* TreeButtonSelected;

    using ImageHolder = std::unique_ptr<uint8_t[], void(*)(void*)>;
    std::vector<std::pair<ref<Texture>, ImageHolder>> m_images;
    int m_current_image;
};

int main(int /* argc */, char** /* argv */) {
    try {
        nanogui::init();

        /* scoped variables */ {
            ref<ExampleApplication> app = new ExampleApplication();
            app->dec_ref();
            app->draw_all();
            app->set_visible(true);
            nanogui::mainloop(1 / 30.f * 1000);
        }

        nanogui::shutdown();
    }
    catch (const std::exception& e) {
        std::string error_msg = std::string("Caught a fatal error: ") + std::string(e.what());
#if defined(_WIN32)
        MessageBoxA(nullptr, error_msg.c_str(), NULL, MB_ICONERROR | MB_OK);
#else
        std::cerr << error_msg << std::endl;
#endif
        return -1;
    }
    catch (...) {
        std::cerr << "Caught an unknown error!" << std::endl;
    }

    return 0;
}
