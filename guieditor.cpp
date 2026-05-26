/* guieditor.cpp -- A professional-grade GUI editor using NanoGUI.
 Based on the provided example application.

 (C) Roger Davenport 2025

 All rights reserved. Use of this source code is governed by a
 BSD-style license that can be found in the LICENSE.txt file.
*/

#include "guieditor.h"
#include "guieditor_json.h"
#include <nanogui/toolbutton.h>
#include <nanogui/icons.h>
#include <nanogui/messagedialog.h>
#include <nanogui/slider.h>
#include <nanogui/colorpicker.h>
#include <nanogui/graph.h>
#include <nanogui/imagepanel.h>
#include <nanogui/folderdialog.h>
#include <nanogui/textarea.h>
#include <nanogui/progressbar.h>
#include <nanogui/split.h>
#include <nanogui/menu.h>
#include <nanogui/texteditor.h>

#include <iostream>
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <thread>
#include <chrono>
#if !defined(__linux__)
#include <memory>
#include <sstream>
#include <array>
#endif

// Pull json2cpp generator as a module (no main)
#define JSON2CPP_AS_MODULE
#include "json2cpp.cpp"
#undef JSON2CPP_AS_MODULE

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
# pragma warning(disable: 4505) // don't warn about dead code in stb_image.h
#elif defined(__GNUC__)
# pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>

// ============================================================================
// Handle geometry helper — shared by hit-testing, canvas drawing, and resize.
// Returns the top-left corner of each of the 8 resize-handle squares (8x8 px).
// ============================================================================
static std::array<Vector2i, 8> compute_handle_positions(
    const Vector2i& abs_pos, const Vector2i& size)
{
    return {{
        { abs_pos.x() - 4,                   abs_pos.y() - 4                  }, // 0: TL
        { abs_pos.x() + size.x() - 4,        abs_pos.y() - 4                  }, // 1: TR
        { abs_pos.x() - 4,                   abs_pos.y() + size.y() - 4       }, // 2: BL
        { abs_pos.x() + size.x() - 4,        abs_pos.y() + size.y() - 4       }, // 3: BR
        { abs_pos.x() + (size.x() - 8) / 2,  abs_pos.y() - 4                  }, // 4: TM
        { abs_pos.x() + (size.x() - 8) / 2,  abs_pos.y() + size.y() - 4       }, // 5: BM
        { abs_pos.x() - 4,                   abs_pos.y() + (size.y() - 8) / 2 }, // 6: LM
        { abs_pos.x() + size.x() - 4,        abs_pos.y() + (size.y() - 8) / 2 }  // 7: RM
    }};
}

// ============================================================================
// EditorWidget<Base> — CRTP mixin that wraps any NanoGUI widget class with:
//   • Edit-mode event blocking  (all input suppressed when test_mode == false)
//   • Visual overlays           (selection border + drop-target highlight)
//
// Simple aliases (e.g. using TestLabel = EditorWidget<Label>) are sufficient
// for most types.  Subclasses that need extra visual hints only override the
// virtual draw_type_hints() hook — all boilerplate stays in this one template.
// ============================================================================
template<typename Base>
class EditorWidget : public Base {
public:
    using Base::Base;   // inherit all constructors from Base

protected:
    GUIEditor* get_editor() const {
        // screen() returns const Screen* in a const method; we know the underlying
        // object is a non-const GUIEditor so const_cast is safe here.
        return dynamic_cast<GUIEditor*>(const_cast<Screen*>(this->screen()));
    }
    bool is_edit_mode() const {
        const GUIEditor* ed = get_editor();
        return ed && !ed->test_mode;
    }

    /// Override in concrete subclasses to draw type-specific visual hints
    /// (called after the standard border overlay).
    virtual void draw_type_hints(NVGcontext* /*ctx*/) {}

private:
    void draw_editor_overlay(NVGcontext* ctx) {
        GUIEditor* ed = get_editor();
        if (!ed) return;

        // Single selection OR group selection
        bool selected = (ed->selected_widget == this);
        if (!selected) {
            for (auto* w : ed->selected_widgets)
                if (w == this) { selected = true; break; }
        }
        bool in_group  = selected && (ed->selected_widgets.size() > 1);
        bool edit_mode = !ed->test_mode;

        // Drop-target highlight when dragging over a container
        if ((ed->dragging || ed->group_dragging) && ed->potential_parent == this) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, this->m_pos.x(), this->m_pos.y(),
                         this->m_size.x(), this->m_size.y());
            nvgFillColor(ctx, Color(255, 255, 0, 120));
            nvgFill(ctx);
            nvgRestore(ctx);
        }

        // Border: green = single selected, blue = group selected, red = edit mode
        if (selected || edit_mode) {
            Color border = selected
                ? (in_group ? Color(50, 150, 255, 255) : Color(0, 255, 0, 255))
                : Color(255, 0, 0, 255);
            float width  = selected ? 2.0f : 1.5f;
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, this->m_pos.x() + 1, this->m_pos.y() + 1,
                         this->m_size.x() - 1, this->m_size.y() - 1);
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, width);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }
    }

public:
    // --- Event overrides: block all input in edit mode ---
    bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override {
        if (is_edit_mode()) return false;
        return Base::mouse_button_event(p, button, down, modifiers);
    }
    bool mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override {
        if (is_edit_mode()) return false;
        return Base::mouse_motion_event(p, rel, button, modifiers);
    }
    bool scroll_event(const Vector2i& p, const Vector2f& rel) override {
        if (is_edit_mode()) return false;
        return Base::scroll_event(p, rel);
    }
    bool mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override {
        if (is_edit_mode()) return false;
        return Base::mouse_drag_event(p, rel, button, modifiers);
    }
    bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (is_edit_mode()) return false;
        return Base::keyboard_event(key, scancode, action, modifiers);
    }
    bool keyboard_character_event(unsigned int codepoint) override {
        if (is_edit_mode()) return false;
        return Base::keyboard_character_event(codepoint);
    }

    void draw(NVGcontext* ctx) override {
        Base::draw(ctx);
        draw_editor_overlay(ctx);
        draw_type_hints(ctx);
    }
};

// ============================================================================
// Simple type aliases — no extra draw hints needed beyond the standard overlay
// ============================================================================
using TestWidget      = EditorWidget<Widget>;
using TestLabel       = EditorWidget<Label>;
using TestButton      = EditorWidget<Button>;
using TestDropdown    = EditorWidget<Dropdown>;
using TestSlider      = EditorWidget<Slider>;
using TestSplit       = EditorWidget<Split>;
using TestGraph       = EditorWidget<Graph>;
using TestImagePanel  = EditorWidget<ImagePanel>;
using TestProgressBar = EditorWidget<ProgressBar>;
using TestTextArea    = EditorWidget<TextArea>;

// ============================================================================
// Types with type-specific visual hints in draw_type_hints()
// ============================================================================

class TestTextBox : public EditorWidget<TextBox> {
public:
    using EditorWidget<TextBox>::EditorWidget;
protected:
    void draw_type_hints(NVGcontext* ctx) override {
        GUIEditor* ed = get_editor();
        if (ed && ed->selected_widget != this && !ed->test_mode) {
            nvgSave(ctx);
            nvgFontSize(ctx, 12.0f);
            nvgFontFace(ctx, "sans");
            nvgFillColor(ctx, Color(255, 0, 0, 255));
            nvgText(ctx, this->m_pos.x() + 5, this->m_pos.y() + 15,
                    "EDIT MODE OFF", nullptr);
            nvgRestore(ctx);
        }
    }
};

class TestCheckBox : public EditorWidget<CheckBox> {
public:
    using EditorWidget<CheckBox>::EditorWidget;
protected:
    void draw_type_hints(NVGcontext* ctx) override {
        GUIEditor* ed = get_editor();
        if (ed && ed->selected_widget != this && !ed->test_mode) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgCircle(ctx, this->m_pos.x() + 10, this->m_pos.y() + 10, 8);
            nvgStrokeColor(ctx, Color(255, 0, 0, 255));
            nvgStrokeWidth(ctx, 1.5f);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }
    }
};

class TestColorPicker : public EditorWidget<ColorPicker> {
public:
    using EditorWidget<ColorPicker>::EditorWidget;
protected:
    void draw_type_hints(NVGcontext* ctx) override {
        GUIEditor* ed = get_editor();
        if (ed && ed->selected_widget != this && !ed->test_mode) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, this->m_pos.x(), this->m_pos.y(),
                           this->m_size.x(), this->m_size.y(), 4);
            nvgStrokeColor(ctx, Color(255, 0, 0, 255));
            nvgStrokeWidth(ctx, 1.5f);
            nvgStroke(ctx);
            nvgFontSize(ctx, 12.0f);
            nvgFontFace(ctx, "sans");
            nvgFillColor(ctx, Color(255, 255, 255, 255));
            nvgTextAlign(ctx, NVG_ALIGN_CENTER);
            nvgText(ctx,
                    this->m_pos.x() + this->m_size.x() / 2.0f,
                    this->m_pos.y() + this->m_size.y() / (2.0f + 15.0f),
                    "DISABLED", nullptr);
            nvgRestore(ctx);
        }
    }
};

class TestWindow : public EditorWidget<Window> {
public:
    using EditorWidget<Window>::EditorWidget;
protected:
    void draw_type_hints(NVGcontext* ctx) override {
        GUIEditor* ed = get_editor();
        if (ed && ed->selected_widget != this && !ed->test_mode) {
            nvgSave(ctx);
            nvgFontSize(ctx, 14.0f);
            nvgFontFace(ctx, "sans");
            nvgFillColor(ctx, Color(255, 255, 255, 255));
            nvgTextAlign(ctx, NVG_ALIGN_CENTER);
            nvgText(ctx,
                    this->m_pos.x() + this->m_size.x() / 2.0f,
                    this->m_pos.y() + 20.0f,
                    "TEST MODE: OFF", nullptr);
            nvgRestore(ctx);
        }
    }
};

// ============================================================================
// TestCanvasWindow — the editor canvas.
// Not an EditorWidget because it needs its own full draw() (resize handles,
// drop highlight, canvas border) and must NOT block its own events.
// ============================================================================
class TestCanvasWindow : public Window {
public:
    TestCanvasWindow(Widget* parent, const std::string& title = "")
        : Window(parent, title) {}

    void draw(NVGcontext* ctx) override {
        GUIEditor* ed = dynamic_cast<GUIEditor*>(screen());

        Window::draw(ctx);

        // Drop-target yellow highlight
        if (ed && ed->dragging && ed->potential_parent == this) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgFillColor(ctx, Color(255, 255, 0, 120));
            nvgFill(ctx);
            nvgRestore(ctx);
        }

        // Canvas border
        if (ed && (ed->selected_widget == this || !ed->test_mode)) {
            Color border = (ed->selected_widget == this)
                           ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);
            float bw = (ed->selected_widget == this) ? 2.0f : 1.5f;
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, bw);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }

        // Resize handles for the currently selected widget
        if (ed && ed->selected_widget && !ed->test_mode) {
            auto handles = compute_handle_positions(
                ed->selected_widget->absolute_position(),
                ed->selected_widget->size());

            constexpr float hs = 8.0f;
            nvgSave(ctx);
            nvgResetScissor(ctx);
            nvgBeginPath(ctx);
            for (const auto& hp : handles)
                nvgRect(ctx, hp.x(), hp.y(), hs, hs);
            nvgFillColor(ctx, Color(255, 255, 255, 255));
            nvgFill(ctx);
            nvgStrokeColor(ctx, Color(0, 0, 0, 128));
            nvgStrokeWidth(ctx, 1.0f);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }
    }
};

// ============================================================================
// Widget registry — one WidgetDef per placeable type.
// To add a new widget: append one entry here, then add a toolbar icon.
// ============================================================================
void GUIEditor::init_widget_registry() {
    m_widget_defs = {
        {
            FA_WINDOW_MAXIMIZE, "Window", "WINDOW", {200, 150},
            [](Widget* parent) -> Widget* {
                auto* w = new TestWindow(parent, "New Window");
                w->set_size(Vector2i(200, 150));
                w->set_layout(new GroupLayout());
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestWindow*>(w) != nullptr; }
        },
        {
            FA_TH, "Pane", "PANE", {150, 100},
            [](Widget* parent) -> Widget* {
                auto* w = new TestWidget(parent);
                w->set_fixed_size(Vector2i(150, 100));
                w->set_layout(new GroupLayout());
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestWidget*>(w) != nullptr; }
        },
        {
            FA_TAG, "Label", "LABEL", {100, 20},
            [](Widget* parent) -> Widget* {
                auto* w = new TestLabel(parent, "Label");
                w->set_fixed_size(Vector2i(100, 20));
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestLabel*>(w) != nullptr; }
        },
        {
            FA_HAND_POINT_UP, "Button", "BUTTON", {100, 25},
            [](Widget* parent) -> Widget* {
                auto* w = new TestButton(parent, "Button");
                w->set_fixed_size(Vector2i(100, 25));
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestButton*>(w) != nullptr; }
        },
        {
            FA_KEYBOARD, "Text Box", "TEXTBOX", {150, 25},
            [](Widget* parent) -> Widget* {
                auto* w = new TestTextBox(parent);
                w->set_fixed_size(Vector2i(150, 25));
                w->set_value("Text");
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestTextBox*>(w) != nullptr; }
        },
        {
            FA_CARET_DOWN, "Dropdown", "DROPDOWN", {150, 25},
            [](Widget* parent) -> Widget* {
                auto* dd = new TestDropdown(parent, Dropdown::ComboBox, "Dropdown");
                dd->set_fixed_size(Vector2i(150, 25));
                dd->set_width(150);
                dd->set_text_color(Color(255, 255, 255, 255));
                for (const std::string& item : {"Item 1", "Item 2"}) {
                    dd->add_item({item, item + "_item"}, 0, nullptr, {{0, 0}}, true);
                }
                for (Widget* child : dd->popup()->children()) {
                    if (auto* mi = dynamic_cast<MenuItem*>(child))
                        mi->set_callback([mi] { std::cout << "Selected: " << mi->caption() << "\n"; });
                }
                dd->set_selected_callback([dd](int idx) {
                    if (auto* item = dd->popup()->item(idx))
                        std::cout << "Dropdown - Selected: " << item->caption() << "\n";
                });
                return dd;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestDropdown*>(w) != nullptr; }
        },
        {
            FA_CHECK_SQUARE, "Checkbox", "CHECKBOX", {150, 25},
            [](Widget* parent) -> Widget* {
                auto* w = new TestCheckBox(parent, "Checkbox");
                w->set_fixed_size(Vector2i(150, 25));
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestCheckBox*>(w) != nullptr; }
        },
        {
            FA_SLIDERS_H, "Slider", "SLIDER", {150, 25},
            [](Widget* parent) -> Widget* {
                auto* w = new TestSlider(parent);
                w->set_fixed_size(Vector2i(150, 25));
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestSlider*>(w) != nullptr; }
        },
        {
            FA_PALETTE, "Color Picker", "COLORPICKER", {100, 100},
            [](Widget* parent) -> Widget* {
                auto* w = new TestColorPicker(parent, Color(255, 0, 0, 255));
                w->set_fixed_size(Vector2i(100, 100));
                return w;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestColorPicker*>(w) != nullptr; }
        },
        {
            FA_COLUMNS, "Split", "SPLIT", {300, 150},
            [](Widget* parent) -> Widget* {
                // Split requires exactly 2 children — add two empty panes automatically.
                auto* sp = new TestSplit(parent, Split::Orientation::Horizontal);
                sp->set_fixed_size(Vector2i(300, 150));
                auto* pane1 = new TestWidget(sp);
                pane1->set_layout(new GroupLayout());
                auto* pane2 = new TestWidget(sp);
                pane2->set_layout(new GroupLayout());
                return sp;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestSplit*>(w) != nullptr; }
        },
        {
            FA_CHART_LINE, "Graph", "GRAPH", {200, 100},
            [](Widget* parent) -> Widget* {
                auto* g = new TestGraph(parent, "Graph");
                g->set_fixed_size(Vector2i(200, 100));
                g->set_values({0.2f,0.5f,0.3f,0.8f,0.6f,0.4f,0.7f,0.9f,0.1f,0.5f});
                return g;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestGraph*>(w) != nullptr; }
        },
        {
            FA_IMAGE, "Image Panel", "IMAGEPANEL", {200, 150},
            [](Widget* parent) -> Widget* {
                auto* ip = new TestImagePanel(parent);
                ip->set_fixed_size(Vector2i(200, 150));
                return ip;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestImagePanel*>(w) != nullptr; }
        },
        {
            FA_FOLDER_OPEN, "Progress Bar", "PROGRESSBAR", {200, 25},
            [](Widget* parent) -> Widget* {
                auto* pb = new TestProgressBar(parent);
                pb->set_fixed_size(Vector2i(200, 25));
                pb->set_value(0.5f);
                return pb;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestProgressBar*>(w) != nullptr; }
        },
        {
            FA_QUESTION_CIRCLE, "Text Area", "TEXTAREA", {200, 100},
            [](Widget* parent) -> Widget* {
                auto* ta = new TestTextArea(parent);
                ta->set_fixed_size(Vector2i(200, 100));
                ta->append("Text Area");
                return ta;
            },
            [](Widget* w) -> bool { return dynamic_cast<TestTextArea*>(w) != nullptr; }
        },
    };
}

// ============================================================================
// GUIEditor constructor
// ============================================================================
GUIEditor::GUIEditor() : Screen(Vector2i(1024, 768), "GUI Editor") {
    init_widget_registry();

    // Editor panel window (left side)
    editor_win = new Window(this, "");
    editor_win->set_position(Vector2i(0, 0));
    editor_win->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill));
    editor_win->set_min_width(250);
    editor_win->set_min_height(size().y());

    // Toolbar container — a BoxLayout row that sizes itself to its content,
    // preventing the editor_win Fill alignment from stretching the inner
    // GridLayout and spreading the buttons across the full panel width.
    Widget *toolbar_row = new Widget(editor_win);
    toolbar_row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Minimum, 5, 0));

    Widget *toolbar = new Widget(toolbar_row);
    toolbar->set_layout(new GridLayout(Orientation::Horizontal, 4, Alignment::Minimum, 5, 5));

    std::vector<int> toolbar_icons = {
        FA_MOUSE_POINTER, FA_WINDOW_MAXIMIZE, FA_TH,           FA_COLUMNS,
        FA_TAG,           FA_KEYBOARD,        FA_HAND_POINT_UP, FA_CARET_DOWN,
        FA_CHECK_SQUARE,  FA_SLIDERS_H,       FA_PALETTE,       FA_CHART_LINE,
        FA_IMAGE,         FA_FOLDER_OPEN,     FA_QUESTION_CIRCLE, FA_TRASH
    };
    std::vector<std::string> toolbar_tooltips = {
        "Select Tool",  "Window",       "Widget Pane",  "Split View",
        "Label",        "Text Box",     "Button",       "Dropdown",
        "Checkbox",     "Slider",       "Color Picker", "Graph",
        "Image Panel",  "Progress Bar", "Text Area",    "Delete"
    };

    for (size_t i = 0; i < toolbar_icons.size(); ++i) {
        ToolButton *tb = new ToolButton(toolbar, toolbar_icons[i]);
        tb->set_flags(Button::ToggleButton);
        tb->set_tooltip(toolbar_tooltips[i]);
        tb->set_callback([this, tb, icon = toolbar_icons[i]] {
            for (auto b : tool_buttons)
                if (b != tb) b->set_pushed(false);
            tb->set_pushed(true);
            current_tool = icon;
            // Update the properties header to show the tool's widget type
            if (properties_label) {
                std::string type_name;
                for (const auto& def : m_widget_defs)
                    if (def.icon == icon) { type_name = def.type_name; break; }
                properties_label->set_caption(
                    type_name.empty() ? "Properties" : type_name + " Properties");
            }
        });
        tool_buttons.push_back(tb);
    }

	// Load / Save row
    Widget *fileRow = new Widget(editor_win);
    fileRow->set_layout(
    	new GridLayout(Orientation::Horizontal, 2,
                                        Alignment::Minimum, 15, 5)
		);

    Button *load_btn = new Button(fileRow, "Load", FA_FOLDER_OPEN);
    load_btn->set_tooltip("Load layout from JSON file");
    load_btn->set_callback([this] {
        auto results = nanogui::file_dialog({{"json", "JSON Layout"}}, false, false, "");
        if (results.empty()) return;
        std::string path = results.front();
        if (path.empty()) return;
        if (!guieditor_json::load_layout(this, path)) {
            new MessageDialog(this, MessageDialog::Type::Warning,
                              "Load failed", "Could not load JSON layout from " + path);
        } else {
            perform_layout();
            redraw();
        }
    });

    Button *save_btn = new Button(fileRow, "Save", FA_SAVE);
    save_btn->set_tooltip("Save layout to JSON file");
    save_btn->set_callback([this] {
        auto results = nanogui::file_dialog({{"json", "JSON Layout"}}, true, false, "");
        if (results.empty()) return;
        std::string path = results.front();
        if (path.empty()) return;
        if (!guieditor_json::save_layout(this, path)) {
            new MessageDialog(this, MessageDialog::Type::Warning,
                              "Save failed", "Could not save JSON layout to " + path);
        }
    });

    // Edit Code button — generates C++ via json2cpp module and opens editor window
    Button *edit_code_btn = new Button(fileRow, "Edit Code", FA_CODE);
    edit_code_btn->set_tooltip("Generate C++ code from current layout and open in editor");
    edit_code_btn->set_callback([this] {
        open_code_editor_window();
    });

    // Test button — toggles test mode, stays in sync with the checkbox below
    test_mode_button = new Button(fileRow, "Test", FA_PLAY);
    test_mode_button->set_flags(Button::ToggleButton);
    test_mode_button->set_tooltip("Toggle Test Mode (run the UI without editor overlays)");
    test_mode_button->set_change_callback([this](bool pushed) {
        test_mode = pushed;
        test_mode_checkbox->set_checked(pushed);
        selected_widget  = nullptr;
        selected_widgets.clear();
        perform_layout();
        draw_all();
        async([this] { update_properties(); });
    });

    // Test-mode toggle (checkbox keeps the button in sync when used directly)
    Widget *testModeRow = new Widget(editor_win);
    testModeRow->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Fill, 0, 5));

    test_mode_checkbox = new CheckBox(testModeRow, "Test Mode");
    test_mode_checkbox->set_height(25);
    test_mode_checkbox->set_min_height(25);
    test_mode_checkbox->set_callback([this](bool checked) {
        test_mode = checked;
        test_mode_button->set_pushed(checked);
        selected_widget  = nullptr;
        selected_widgets.clear();
        perform_layout();
        draw_all();
        async([this] { update_properties(); });
    });
    test_mode_checkbox->set_checked(false);
    test_mode = false;

    // Properties section
    properties_label = new Label(editor_win, "Properties", "sans-bold");
    properties_label->set_fixed_size(Vector2i(220, 25));

    properties_pane = new Widget(editor_win);
    GridLayout* layout = new GridLayout(Orientation::Horizontal, 2,
                                        Alignment::Minimum, 15, 5);
    layout->set_col_alignment({Alignment::Maximum, Alignment::Fill});
    layout->set_spacing(Orientation::Horizontal, 10);
    properties_pane->set_layout(layout);

    update_properties();

    // Canvas window (absolute positioning)
    canvas_win = new TestCanvasWindow(this, "Canvas");
    canvas_win->set_position(Vector2i(280, 15));
    canvas_win->set_size(Vector2i(100, 70));
    canvas_win->set_layout(nullptr);
    canvas_win->set_id("CANVAS");

    perform_layout();
    canvas_win->set_size(Vector2i(700, 700));
}

// ============================================================================
// Snap helper
// ============================================================================
Vector2i GUIEditor::snap(const Vector2i& pos) {
    if (snap_grid_size == 0) return pos;
    return Vector2i((pos.x() / snap_grid_size) * snap_grid_size,
                    (pos.y() / snap_grid_size) * snap_grid_size);
}

// ============================================================================
// Properties panel — public entry point
// ============================================================================
bool GUIEditor::update_properties() {
    // Clear existing properties
    while (properties_pane->child_count() > 0)
        properties_pane->remove_child(
            properties_pane->child_at(properties_pane->child_count() - 1));

    // Update the section header
    if (properties_label) {
        if (selected_widgets.size() > 1)
            properties_label->set_caption(std::to_string(selected_widgets.size()) + " Widgets Selected");
        else if (selected_widget)
            properties_label->set_caption(getWidgetTypeName(selected_widget) + " Properties");
        else
            properties_label->set_caption("Properties");
    }

    // Group selection: just show the count
    if (selected_widgets.size() > 1) {
        new Label(properties_pane, std::to_string(selected_widgets.size()) + " widgets selected");
        perform_layout();
        redraw();
        return true;
    }

    if (!selected_widget) {
        new Label(properties_pane, "No widget selected");
        perform_layout();
        redraw();
        return false;
    }

    if (selected_widget == canvas_win) add_canvas_properties();
    add_common_properties();
    add_type_properties();
    add_layout_properties();
    add_dimension_properties();
    add_animation_properties();
    add_color_properties();

    perform_layout();
    redraw();
    return true;
}

// ----------------------------------------------------------------------------
// Canvas-specific: snap grid + resizable toggle
// ----------------------------------------------------------------------------
void GUIEditor::add_canvas_properties() {
    new Label(properties_pane, "Snapping:", "sans-bold");
    Dropdown *snap_combo = new Dropdown(properties_pane,
        {"Off", "5", "10", "15", "20", "25"}, {},
        Dropdown::Mode::ComboBox, "Snapping");
    int idx = (snap_grid_size == 0) ? 0 : (snap_grid_size / 5);
    snap_combo->set_selected_index(idx);
    snap_combo->set_selected_callback([this](int index) {
        snap_grid_size = (index == 0) ? 0 : (index * 5);
    });
    snap_combo->set_min_height(20);

    new Label(properties_pane, "Resizable:", "sans-bold");
    CheckBox *resize_checkbox = new CheckBox(properties_pane, "");
    resize_checkbox->set_checked(canvas_win->resizable());
    resize_checkbox->set_callback([this](bool checked) {
        canvas_win->set_resizable(checked);
    });
}

// ----------------------------------------------------------------------------
// Common: Widget type (read-only), Parent ID (read-only), ID (editable)
// ----------------------------------------------------------------------------
void GUIEditor::add_common_properties() {
    new Label(properties_pane, "Widget:", "sans-bold");
    TextBox *type_box = new TextBox(properties_pane);
    type_box->set_value(getWidgetTypeName(selected_widget));
    type_box->set_editable(false);
    type_box->set_min_height(20);

    new Label(properties_pane, "Parent ID:", "sans-bold");
    TextBox *parent_id_box = new TextBox(properties_pane);
    Widget  *par = selected_widget->parent();
    parent_id_box->set_value(par && par != this ? par->id() : "None");
    parent_id_box->set_editable(false);
    parent_id_box->set_min_height(20);

    new Label(properties_pane, "ID:", "sans-bold");
    TextBox *id_box = new TextBox(properties_pane);
    id_box->set_value(selected_widget->id());
    id_box->set_callback([this](const std::string& v) {
        if (!selected_widget) return false;
        selected_widget->set_id(v);
        perform_layout();
        redraw();
        return true;
    });
    id_box->set_min_height(20);
}

// ----------------------------------------------------------------------------
// Type-specific: caption / title / value / items depending on widget type
// ----------------------------------------------------------------------------
void GUIEditor::add_type_properties() {
    if (auto* lbl = dynamic_cast<Label*>(selected_widget)) {
        new Label(properties_pane, "Caption:", "sans-bold");
        TextBox *b = new TextBox(properties_pane);
        b->set_value(lbl->caption());
        b->set_callback([this, lbl](const std::string& v) {
            if (!selected_widget) return false;
            lbl->set_caption(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout(); redraw();
            return true;
        });
        b->set_min_height(20);

    } else if (auto* cb = dynamic_cast<CheckBox*>(selected_widget)) {
        new Label(properties_pane, "Caption:", "sans-bold");
        TextBox *b = new TextBox(properties_pane);
        b->set_value(cb->caption());
        b->set_callback([this, cb](const std::string& v) {
            if (!selected_widget) return false;
            cb->set_caption(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout(); redraw();
            return true;
        });
        b->set_min_height(20);

    } else if (auto* win = dynamic_cast<Window*>(selected_widget)) {
        new Label(properties_pane, "Title:", "sans-bold");
        TextBox *b = new TextBox(properties_pane);
        b->set_value(win->title());
        b->set_callback([this, win](const std::string& v) {
            if (!selected_widget) return false;
            win->set_title(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout(); redraw();
            return true;
        });
        b->set_min_height(20);

    } else if (auto* tb = dynamic_cast<TextBox*>(selected_widget)) {
        new Label(properties_pane, "Value:", "sans-bold");
        TextBox *b = new TextBox(properties_pane);
        b->set_value(tb->value());
        b->set_callback([this, tb](const std::string& v) {
            if (!selected_widget) return false;
            tb->set_value(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout(); redraw();
            return true;
        });
        b->set_min_height(20);

    } else if (auto* dropdown = dynamic_cast<Dropdown*>(selected_widget)) {
        new Label(properties_pane, "Items:", "sans-bold");
        Widget *items_container = new Widget(properties_pane);
        items_container->set_layout(
            new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 5));

        for (int i = 0; i < dropdown->popup()->child_count(); ++i) {
            if (auto* mi = dynamic_cast<MenuItem*>(dropdown->popup()->child_at(i))) {
                Widget *row = new Widget(items_container);
                row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 5));
                TextBox *caption_box = new TextBox(row);
                caption_box->set_value(mi->caption());
                caption_box->set_min_width(150);
                caption_box->set_callback([mi](const std::string& v) {
                    mi->set_caption(v);
                    return true;
                });
                Button *remove_btn = new Button(row, "", FA_MINUS);
                remove_btn->set_min_width(30);
                remove_btn->set_callback([this, i, dropdown] {
                    dropdown->remove_item(i);
                    async([this] { update_properties(); });
                });
            }
        }

        // Add-item row
        Widget *add_row = new Widget(items_container);
        add_row->set_layout(new BoxLayout(Orientation::Horizontal));
        Button *add_btn = new Button(add_row, "", FA_PLUS);
        add_btn->set_min_width(30);
        add_btn->set_callback([this, dropdown] {
            Widget *child = dropdown->add_item({"New", "New_item"}, 0, nullptr, {{0, 0}}, true);
            if (auto* mi = dynamic_cast<MenuItem*>(child))
                mi->set_callback([mi] { std::cout << "Selected item: " << mi->caption() << "\n"; });
            async([this] { update_focus(nullptr); update_properties(); });
        });

    } else if (auto* btn = dynamic_cast<Button*>(selected_widget)) {
        new Label(properties_pane, "Caption:", "sans-bold");
        TextBox *b = new TextBox(properties_pane);
        b->set_value(btn->caption());
        b->set_callback([this, btn](const std::string& v) {
            if (!selected_widget) return false;
            btn->set_caption(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout(); redraw();
            return true;
        });
        b->set_min_height(20);

    } else if (auto* graph = dynamic_cast<Graph*>(selected_widget)) {
        new Label(properties_pane, "Caption:", "sans-bold");
        TextBox *b = new TextBox(properties_pane);
        b->set_value(graph->caption());
        b->set_callback([this, graph](const std::string& v) {
            if (!selected_widget) return false;
            graph->set_caption(v);
            perform_layout(); redraw();
            return true;
        });
        b->set_min_height(20);

    } else if (auto* pb = dynamic_cast<ProgressBar*>(selected_widget)) {
        new Label(properties_pane, "Value:", "sans-bold");
        FloatBox<float> *fb = new FloatBox<float>(properties_pane);
        fb->set_value(pb->value());
        fb->set_editable(true);
        fb->set_spinnable(true);
        fb->set_value_increment(0.05f);
        fb->set_min_max_values(0.0f, 1.0f);
        fb->set_callback([this, pb](float v) {
            if (!selected_widget) return false;
            pb->set_value(v);
            redraw();
            return true;
        });
        fb->set_min_height(20);

    } else if (auto* sp = dynamic_cast<Split*>(selected_widget)) {
        new Label(properties_pane, "Orientation:", "sans-bold");
        ComboBox *oc = new ComboBox(properties_pane, {"Horizontal", "Vertical"});
        oc->set_selected_index(sp->orientation() == Split::Orientation::Horizontal ? 0 : 1);
        oc->set_callback([this, sp](int i) {
            if (!selected_widget) return;
            sp->set_orientation(i == 0 ? Split::Orientation::Horizontal
                                       : Split::Orientation::Vertical);
            perform_layout(); redraw();
        });
        oc->set_min_height(20);

        new Label(properties_pane, "Split Pos:", "sans-bold");
        FloatBox<float> *fp = new FloatBox<float>(properties_pane);
        fp->set_value(sp->drag_position());
        fp->set_editable(true);
        fp->set_spinnable(true);
        fp->set_value_increment(0.05f);
        fp->set_min_max_values(0.0f, 1.0f);
        fp->set_callback([this, sp](float v) {
            if (!selected_widget) return false;
            sp->set_drag_position(v);
            perform_layout(); redraw();
            return true;
        });
        fp->set_min_height(20);
    }
}

// ----------------------------------------------------------------------------
// Layout: combo to choose layout type + layout-specific controls
// ----------------------------------------------------------------------------
void GUIEditor::add_layout_properties() {
    if (!canHaveLayout(selected_widget)) return;

    new Label(properties_pane, "Layout:", "sans-bold");
    ComboBox *layout_combo = new ComboBox(properties_pane, {
        "None", "Box Layout", "Grid Layout", "Advanced Grid", "Flex Layout", "Group Layout"
    });
    layout_combo->set_selected_index(
        getLayoutTypeIndex(getCurrentLayoutType(selected_widget)));
    layout_combo->set_callback([this](int index) {
        if (!selected_widget) return;
        applyLayoutType(selected_widget, index);
        update_properties();
    });
    layout_combo->set_min_height(20);
    addLayoutSpecificControls(selected_widget);
}

// ----------------------------------------------------------------------------
// Dimensions: position X/Y, size W/H, fixed size W/H
// ----------------------------------------------------------------------------
void GUIEditor::add_dimension_properties() {
    // Position X
    new Label(properties_pane, "Position X:", "sans-bold");
    IntBox<int> *pos_x = new IntBox<int>(properties_pane);
    pos_x->set_value(selected_widget->position().x());
    pos_x->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i pos = selected_widget->position(); pos.x() = v;
        selected_widget->set_position(pos);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    pos_x->set_min_height(20);

    // Position Y
    new Label(properties_pane, "Position Y:", "sans-bold");
    IntBox<int> *pos_y = new IntBox<int>(properties_pane);
    pos_y->set_value(selected_widget->position().y());
    pos_y->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i pos = selected_widget->position(); pos.y() = v;
        selected_widget->set_position(pos);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    pos_y->set_min_height(20);

    // Width
    new Label(properties_pane, "Width:", "sans-bold");
    IntBox<int> *width_box = new IntBox<int>(properties_pane);
    width_box->set_value(selected_widget->width());
    width_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i sz = selected_widget->size(); sz.x() = v;
        selected_widget->set_size(sz);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    width_box->set_min_height(20);

    // Height
    new Label(properties_pane, "Height:", "sans-bold");
    IntBox<int> *height_box = new IntBox<int>(properties_pane);
    height_box->set_value(selected_widget->height());
    height_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i sz = selected_widget->size(); sz.y() = v;
        selected_widget->set_size(sz);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    height_box->set_min_height(20);

    // Fixed Width
    new Label(properties_pane, "Fxd Width:", "sans-bold");
    IntBox<int> *fwidth_box = new IntBox<int>(properties_pane);
    fwidth_box->set_value(selected_widget->fixed_width());
    fwidth_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i sz = selected_widget->size(); sz.x() = v;
        selected_widget->set_fixed_size(sz);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    fwidth_box->set_min_height(20);

    // Fixed Height
    new Label(properties_pane, "Fxd Height:", "sans-bold");
    IntBox<int> *fheight_box = new IntBox<int>(properties_pane);
    fheight_box->set_value(selected_widget->fixed_height());
    fheight_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i sz = selected_widget->size(); sz.y() = v;
        selected_widget->set_fixed_size(sz);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    fheight_box->set_min_height(20);
}

// ----------------------------------------------------------------------------
// Animation: type combo, duration, start/stop buttons
// ----------------------------------------------------------------------------
void GUIEditor::add_animation_properties() {
    new Label(properties_pane, "Animation:", "sans-bold");
    ComboBox *anim_combo = new ComboBox(properties_pane, {
        "None", "Sproing", "Warble", "Rotate",
        "SlideOpen", "SlideClose", "SlideUp", "SlideDown"
    });
    anim_combo->set_selected_index((int)selected_widget->animation_type());
    anim_combo->set_callback([this](int index) {
        if (!selected_widget) return;
        selected_widget->set_animation_type(
            static_cast<Widget::AnimationType>(index));
    });
    anim_combo->set_min_height(20);

    new Label(properties_pane, "Anim Time:", "sans-bold");
    FloatBox<double> *anim_duration = new FloatBox<double>(properties_pane);
    anim_duration->set_value(selected_widget->animation_duration());
    anim_duration->set_units("s");
    anim_duration->set_editable(true);
    anim_duration->set_spinnable(true);
    anim_duration->set_value_increment(0.1);
    anim_duration->set_min_max_values(0.0, 60.0);
    anim_duration->set_callback([this](double v) {
        if (!selected_widget) return false;
        selected_widget->set_animation_duration(v);
        return true;
    });
    anim_duration->set_min_height(20);

    new Label(properties_pane, "Anim Run:", "sans-bold");
    Widget *anim_buttons = new Widget(properties_pane);
    anim_buttons->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 5));

    Button *anim_start_btn = new Button(anim_buttons, "Start", FA_PLAY);
    anim_start_btn->set_tooltip("Start the selected widget's animation");
    anim_start_btn->set_callback([this] {
        if (!selected_widget) return;
        selected_widget->set_visible(true);
        selected_widget->start_animation();
        redraw();
    });

    Button *anim_stop_btn = new Button(anim_buttons, "Stop", FA_STOP);
    anim_stop_btn->set_tooltip("Stop the selected widget's animation");
    anim_stop_btn->set_callback([this] {
        if (!selected_widget) return;
        selected_widget->stop_animation();
        redraw();
    });
}

// ----------------------------------------------------------------------------
// Color: background color picker (placeholder)
// ----------------------------------------------------------------------------
void GUIEditor::add_color_properties() {
    new Label(properties_pane, "BG Color:", "sans-bold");
    ColorPicker *bg_color = new ColorPicker(properties_pane);
    bg_color->set_callback([this](const Color /*&c*/) {
        if (!selected_widget) return false;
        perform_layout();
        redraw();
        return true;
    });
    bg_color->set_min_height(20);
}

// ============================================================================
// Layout helpers
// ============================================================================
bool GUIEditor::canHaveLayout(Widget* widget) {
    if (dynamic_cast<TestSplit*>(widget)) return false; // Split manages its own layout
    return dynamic_cast<Window*>(widget) ||
           dynamic_cast<TestWidget*>(widget) ||
           (widget != canvas_win && widget->child_count() > 0);
}

std::string GUIEditor::getCurrentLayoutType(Widget* widget) {
    Layout* layout = widget->layout();
    if (!layout)                                  return "None";
    if (dynamic_cast<BoxLayout*>(layout))         return "Box Layout";
    if (dynamic_cast<GridLayout*>(layout))        return "Grid Layout";
    if (dynamic_cast<AdvancedGridLayout*>(layout)) return "Advanced Grid";
    if (dynamic_cast<FlexLayout*>(layout))        return "Flex Layout";
    if (dynamic_cast<GroupLayout*>(layout))       return "Group Layout";
    return "Unknown";
}

int GUIEditor::getLayoutTypeIndex(const std::string& type) {
    if (type == "None")          return 0;
    if (type == "Box Layout")    return 1;
    if (type == "Grid Layout")   return 2;
    if (type == "Advanced Grid") return 3;
    if (type == "Flex Layout")   return 4;
    if (type == "Group Layout")  return 5;
    return 0;
}

void GUIEditor::applyLayoutType(Widget* widget, int type_index) {
    Layout* new_layout = nullptr;
    switch (type_index) {
        case 1: new_layout = new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 5); break;
        case 2: new_layout = new GridLayout(Orientation::Horizontal, 2, Alignment::Fill, 10, 5); break;
        case 3: new_layout = new AdvancedGridLayout({100, 100}, {30, 30}, 10); break;
        case 4: new_layout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                            AlignItems::Stretch, 10, 5); break;
        case 5: new_layout = new GroupLayout(10, 5, 15, 5); break;
        default: break; // case 0: nullptr = no layout
    }
    widget->set_layout(new_layout);
    widget->perform_layout(m_nvg_context);
    if (Widget* par = widget->parent())
        par->perform_layout(m_nvg_context);
    perform_layout();
    redraw();
}

void GUIEditor::addLayoutSpecificControls(Widget* widget) {
    Layout* layout = widget->layout();
    if (!layout) return;
    if (auto* l = dynamic_cast<BoxLayout*>(layout))   { addBoxLayoutControls(l);   return; }
    if (auto* l = dynamic_cast<GridLayout*>(layout))  { addGridLayoutControls(l);  return; }
    if (auto* l = dynamic_cast<FlexLayout*>(layout))  { addFlexLayoutControls(l);  return; }
    if (auto* l = dynamic_cast<GroupLayout*>(layout)) { addGroupLayoutControls(l); return; }
}

void GUIEditor::addBoxLayoutControls(BoxLayout* layout) {
    new Label(properties_pane, "Orientation:", "sans-bold");
    ComboBox *oc = new ComboBox(properties_pane, {"Horizontal", "Vertical"});
    oc->set_selected_index(layout->orientation() == Orientation::Horizontal ? 0 : 1);
    oc->set_callback([this, layout](int i) {
        layout->set_orientation(i == 0 ? Orientation::Horizontal : Orientation::Vertical);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
    });
    oc->set_min_height(20);

    new Label(properties_pane, "Alignment:", "sans-bold");
    ComboBox *ac = new ComboBox(properties_pane, {"Minimum", "Middle", "Maximum", "Fill"});
    ac->set_selected_index((int)layout->alignment());
    ac->set_callback([this, layout](int i) {
        layout->set_alignment((Alignment)i);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
    });
    ac->set_min_height(20);

    new Label(properties_pane, "Margin:", "sans-bold");
    IntBox<int> *mb = new IntBox<int>(properties_pane);
    mb->set_value(layout->margin());
    mb->set_callback([this, layout](int v) {
        layout->set_margin(v);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    mb->set_min_height(20);

    new Label(properties_pane, "Spacing:", "sans-bold");
    IntBox<int> *sb = new IntBox<int>(properties_pane);
    sb->set_value(layout->spacing());
    sb->set_callback([this, layout](int v) {
        layout->set_spacing(v);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    sb->set_min_height(20);
}

void GUIEditor::addGridLayoutControls(GridLayout* layout) {
    new Label(properties_pane, "Resolution:", "sans-bold");
    IntBox<int> *rb = new IntBox<int>(properties_pane);
    rb->set_value(layout->resolution());
    rb->set_callback([this, layout](int v) {
        layout->set_resolution(std::max(1, v));
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    rb->set_min_height(20);

    new Label(properties_pane, "Orientation:", "sans-bold");
    ComboBox *oc = new ComboBox(properties_pane, {"Horizontal", "Vertical"});
    oc->set_selected_index(layout->orientation() == Orientation::Horizontal ? 0 : 1);
    oc->set_callback([this, layout](int i) {
        layout->set_orientation(i == 0 ? Orientation::Horizontal : Orientation::Vertical);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
    });
    oc->set_min_height(20);

    new Label(properties_pane, "Table:", "sans-bold");
    ComboBox *tc = new ComboBox(properties_pane, {"None", "Pinkish Theme"});
    tc->set_callback([this, layout](int index) {
        if (index > 0) {
            TableTheme theme;
            theme.header_background   = Color(100, 100, 100, 255);
            theme.even_row_background = Color(150, 140, 140, 255);
            theme.odd_row_background  = Color(175, 155, 155, 255);
            theme.border_color        = Color(50,  50,  50,  128);
            theme.border_width        = 1.0f;
            theme.shade_rows          = true;
            theme.first_row_is_header = true;
            if (selected_widget->layout())
                layout->enable_draw_table(theme);
            layout->set_margin(4);
        } else {
            layout->disable_draw_table();
        }
        perform_layout(); redraw();
    });
    tc->set_min_height(20);
}

void GUIEditor::addFlexLayoutControls(FlexLayout* layout) {
    new Label(properties_pane, "Direction:", "sans-bold");
    ComboBox *dc = new ComboBox(properties_pane,
        {"Row", "Row Reverse", "Column", "Column Reverse"});
    dc->set_selected_index((int)layout->direction());
    dc->set_callback([this, layout](int i) {
        layout->set_direction((FlexDirection)i);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
    });
    dc->set_min_height(20);

    new Label(properties_pane, "Justify:", "sans-bold");
    ComboBox *jc = new ComboBox(properties_pane,
        {"Flex Start", "Flex End", "Center", "Space Between", "Space Around", "Space Evenly"});
    jc->set_selected_index((int)layout->justify_content());
    jc->set_callback([this, layout](int i) {
        layout->set_justify_content((JustifyContent)i);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
    });
    jc->set_min_height(20);

    new Label(properties_pane, "Align Items:", "sans-bold");
    ComboBox *ac = new ComboBox(properties_pane,
        {"Flex Start", "Flex End", "Center", "Stretch", "Baseline"});
    ac->set_selected_index((int)layout->align_items());
    ac->set_callback([this, layout](int i) {
        layout->set_align_items((AlignItems)i);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
    });
    ac->set_min_height(20);
}

void GUIEditor::addGroupLayoutControls(GroupLayout* layout) {
    new Label(properties_pane, "Margin:", "sans-bold");
    IntBox<int> *mb = new IntBox<int>(properties_pane);
    mb->set_value(layout->margin());
    mb->set_callback([this, layout](int v) {
        layout->set_margin(v);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    mb->set_min_height(20);

    new Label(properties_pane, "Spacing:", "sans-bold");
    IntBox<int> *sb = new IntBox<int>(properties_pane);
    sb->set_value(layout->spacing());
    sb->set_callback([this, layout](int v) {
        layout->set_spacing(v);
        selected_widget->perform_layout(m_nvg_context);
        if (auto* p = selected_widget->parent()) p->perform_layout(m_nvg_context);
        perform_layout(); redraw();
        return true;
    });
    sb->set_min_height(20);
}

// ============================================================================
// Registry-based helpers: type names, ID generation, creation, placement
// ============================================================================
std::string GUIEditor::getWidgetTypeName(Widget *widget) {
    if (widget == canvas_win) return "Canvas";
    for (const auto& def : m_widget_defs)
        if (def.matches(widget)) return def.type_name;
    return "Widget";
}

std::string GUIEditor::generateUniqueId(int icon) {
    for (const auto& def : m_widget_defs) {
        if (def.icon == icon) {
            int count = ++m_widget_counters[icon];
            return def.id_prefix + std::to_string(count);
        }
    }
    return "WIDGET" + std::to_string(++m_widget_counters[-1]);
}

// Used by the JSON loader — normalises historical name aliases.
Widget* GUIEditor::create_widget_by_type(const std::string& type, Widget* parent) {
    if (!parent) parent = canvas_win;
    std::string t = type;
    if      (t == "View" || t == "Widget") t = "Pane";
    else if (t == "TextBox")               t = "Text Box";
    else if (t == "CheckBox")              t = "Checkbox";
    else if (t == "ColorPicker")           t = "Color Picker";
    else if (t == "ImagePanel")            t = "Image Panel";
    else if (t == "ProgressBar")           t = "Progress Bar";
    else if (t == "TextArea")              t = "Text Area";

    // Split is special: the registry factory auto-adds two panes for interactive
    // placement, but the JSON loader supplies its own children from the file.
    // Return a bare Split here so build_from_json can attach the saved panes.
    if (t == "Split") {
        auto* sp = new TestSplit(parent, Split::Orientation::Horizontal);
        return sp;
    }

    for (const auto& def : m_widget_defs)
        if (def.type_name == t) return def.factory(parent);
    return nullptr;
}

// Places a widget on the canvas at a given local position via the registry.
Widget* GUIEditor::place_widget(int icon, Widget* target, const Vector2i& pos) {
    for (const auto& def : m_widget_defs) {
        if (def.icon == icon) {
            Widget* w = def.factory(target);
            w->set_position(pos);
            w->set_id(generateUniqueId(icon));
            return w;
        }
    }
    return nullptr;
}

void GUIEditor::clear_canvas() {
    selected_widget  = nullptr;
    selected_widgets.clear();
    if (!canvas_win) return;
    while (canvas_win->child_count() > 0)
        canvas_win->remove_child_at(0);
    canvas_win->set_layout(nullptr);
    update_properties();
}

// ============================================================================
// Mouse / keyboard event handling
// ============================================================================

// Recursive child test (used by selection logic)
static bool is_child(Widget *find, Widget *search) {
    for (Widget *child : search->children()) {
        if (child == find) return true;
        else return is_child(child, search);
    }
    return false;
}

// Collect all canvas-descendant widgets whose screen-space bounds intersect
// [rb_min, rb_max).  Direct children of a TestSplit are skipped because they
// are structural panes, not user-placed widgets.
static void collect_widgets_in_rect(Widget* container,
                                    const Vector2i& rb_min,
                                    const Vector2i& rb_max,
                                    std::vector<Widget*>& result) {
    for (Widget* child : container->children()) {
        // Skip structural Split panes
        if (dynamic_cast<TestSplit*>(container)) continue;

        Vector2i cp = child->absolute_position();
        Vector2i cs = child->size();
        bool intersects = (cp.x()          < rb_max.x() &&
                           cp.x() + cs.x() > rb_min.x() &&
                           cp.y()          < rb_max.y() &&
                           cp.y() + cs.y() > rb_min.y());
        if (intersects)
            result.push_back(child);

        // Recurse — but don't descend into leaves (only containers have children)
        collect_widgets_in_rect(child, rb_min, rb_max, result);
    }
}

// Returns the deepest container (TestWindow or TestWidget) under screen position p,
// starting the search from root. Skips `exclude` and its subtree (used to prevent
// dropping a widget into itself). Falls back to returning root if no match is found.
//
// TestSplit is treated as a transparent pass-through: it is not itself a valid
// placement target (Split manages its own two children), but its pane children are.
// If p falls on the Split divider (between the two panes) the Split is skipped and
// we fall back to root.
static Widget* find_deepest_container(Widget* root, const Vector2i& p,
                                       Widget* exclude = nullptr) {
    for (Widget* child : root->children()) {
        if (child == exclude) continue;

        Vector2i cp = child->absolute_position(), cs = child->size();
        Vector2i lp = p - cp;
        if (lp.x() < 0 || lp.y() < 0 || lp.x() >= cs.x() || lp.y() >= cs.y())
            continue; // p is not inside this child

        if (dynamic_cast<TestWindow*>(child) || dynamic_cast<TestWidget*>(child)) {
            // Valid container — recurse to find a deeper one
            return find_deepest_container(child, p, exclude);
        }

        if (dynamic_cast<TestSplit*>(child)) {
            // Pass-through: search inside the split for a pane target.
            // If p is on the divider (between the panes) inner == child so we
            // fall through and return root instead.
            Widget* inner = find_deepest_container(child, p, exclude);
            if (inner != child) return inner;
        }
    }
    return root; // root is the deepest valid container containing p
}

bool GUIEditor::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    if (has_modal_dialog()) {
        return Screen::mouse_button_event(p, button, down, modifiers);
    }
    m_redraw = true;
    Widget *clicked_widget = find_widget(p);

    if (Screen::mouse_button_event(p, button, down, modifiers))
        return true;

    if (!test_mode && button == GLFW_MOUSE_BUTTON_1 && down) {

        // --- Check for resize handle hit first ---
        Widget* hit = find_widget_with_handle(p);
        if (hit) {
            if (hit != selected_widget) {
                selected_widget = hit;
                update_properties();
            }
            auto handles = compute_handle_positions(hit->absolute_position(), hit->size());
            resize_handle = -1;
            for (int i = 0; i < 8; ++i) {
                Vector2i rel = p - handles[i];
                if (rel.x() >= 0 && rel.y() >= 0 && rel.x() < 8 && rel.y() < 8) {
                    resize_handle = i; break;
                }
            }
            resizing          = true;
            resize_start_pos  = selected_widget->position();
            resize_start_size = selected_widget->size();
            drag_start        = p;
            return true;
        }

        // --- Delete tool ---
        if (current_tool == FA_TRASH) {
            if (clicked_widget && clicked_widget->window() != editor_win
                                && clicked_widget != canvas_win) {
                Widget *par = clicked_widget->parent();
                if (par) {
                    if (selected_widget == clicked_widget) selected_widget = nullptr;
                    set_focused(false);
                    notify_widget_destroyed(clicked_widget);
                    clicked_widget->inc_ref();
                    par->remove_child(clicked_widget);
                    update_properties();
                    redraw();
                    clicked_widget->dec_ref();
                    return true;
                }
            }
            return false;
        }

        // --- Find deepest container for widget placement / selection ---
        // Guard: click must be within the canvas bounds
        Vector2i canvas_rel = p - canvas_win->absolute_position();
        if (canvas_rel.x() < 0 || canvas_rel.y() < 0) return false;

        // Recursively find the innermost container at this position.
        Widget *target_container = find_deepest_container(canvas_win, p);
        Vector2i relative_pos   = p - target_container->absolute_position();

        // --- Select tool ---
        if (current_tool == FA_MOUSE_POINTER) {
            bool on_canvas = clicked_widget &&
                (clicked_widget == canvas_win || is_child(clicked_widget, canvas_win));

            if (on_canvas && clicked_widget != canvas_win) {
                // Clicked on a canvas widget
                bool in_group  = !selected_widgets.empty() &&
                    std::find(selected_widgets.begin(), selected_widgets.end(),
                              clicked_widget) != selected_widgets.end();
                bool shift = (modifiers & GLFW_MOD_SHIFT)   != 0;
                bool ctrl  = (modifiers & GLFW_MOD_CONTROL) != 0;

                if (shift) {
                    // --- Shift+click: add widget to selection group ---
                    if (!in_group && clicked_widget != selected_widget) {
                        // Promote existing single selection into the group first
                        if (selected_widget && selected_widgets.empty())
                            selected_widgets.push_back(selected_widget);
                        selected_widgets.push_back(clicked_widget);
                        if (!selected_widgets.empty())
                            selected_widget = selected_widgets[0];
                    }
                    update_properties();

                } else if (ctrl) {
                    // --- Ctrl+click: remove widget from selection group ---
                    if (in_group) {
                        selected_widgets.erase(
                            std::remove(selected_widgets.begin(), selected_widgets.end(),
                                        clicked_widget),
                            selected_widgets.end());
                        if (selected_widgets.size() == 1) {
                            // Collapse back to single selection
                            selected_widget = selected_widgets[0];
                            selected_widgets.clear();
                        } else if (selected_widgets.empty()) {
                            selected_widget = nullptr;
                        }
                    } else if (clicked_widget == selected_widget) {
                        selected_widget = nullptr;  // deselect lone selection
                    }
                    update_properties();

                } else if (in_group && selected_widgets.size() > 1) {
                    // Start group drag
                    group_dragging   = true;
                    group_drag_start = p;
                    drag_initial_pos = p;
                    group_initial_positions.clear();
                    for (auto* w : selected_widgets)
                        group_initial_positions[w] = w->absolute_position();
                } else {
                    // Plain click: single select
                    selected_widgets.clear();
                    printf("Selected widget %s\n", clicked_widget->id().c_str());
                    selected_widget  = clicked_widget;
                    dragging         = true;
                    drag_start       = p;
                    drag_initial_pos = p;
                    drag_offset      = p - clicked_widget->absolute_position();
                    original_parent  = clicked_widget->parent();
                    update_properties();
                }
            } else if (on_canvas) {
                // Clicked on empty canvas — start rubber band
                selected_widget  = nullptr;
                selected_widgets.clear();
                rubber_banding    = true;
                rubber_band_start = rubber_band_end = p;
                update_properties();
            } else {
                // Clicked off canvas — deselect everything
                selected_widget  = nullptr;
                selected_widgets.clear();
                update_properties();
            }

        // --- Place tool (registry lookup) ---
        } else if (current_tool != 0 && current_tool != FA_TRASH) {
            Widget* new_w = place_widget(current_tool, target_container, relative_pos);
            if (new_w) {
                selected_widget = new_w;
                update_properties();
                perform_layout();
                redraw();
                return true;
            }
        }

    } else if (!down && dragging) {

        // --- Mouse released: finalise drag / reparent ---
        if (selected_widget && canvas_win != selected_widget) {
            // Find the deepest container under the drop point, excluding the
            // dragged widget itself so a container can't be dropped into itself.
            Widget *new_parent = find_deepest_container(canvas_win, p, selected_widget);
            Vector2i new_pos   = p - new_parent->absolute_position() - drag_offset;
            new_pos = snap(new_pos);

            // Only reparent if the mouse moved meaningfully from the initial click.
            // A plain click (e.g. selecting a Split pane) must never reparent:
            // find_deepest_container with the pane excluded falls back to canvas_win,
            // which would incorrectly satisfy the reparent condition.
            Vector2i moved = p - drag_initial_pos;
            bool actually_dragged = (std::abs(moved.x()) > 5 || std::abs(moved.y()) > 5);

            if (actually_dragged && new_parent != selected_widget->parent()
                && new_parent->window() != editor_win
                && new_parent != selected_widget
                && !dynamic_cast<TestSplit*>(selected_widget->parent())) {
                // Reparent
                Widget* cur = selected_widget->parent();
                if (cur) {
                    selected_widget->inc_ref();
                    cur->remove_child(selected_widget);
                    new_parent->add_child(selected_widget);
                    Vector2i ps = new_parent->size(), ws = selected_widget->size();
                    new_pos.x() = std::max(0, std::min(new_pos.x(), ps.x() - ws.x()));
                    new_pos.y() = std::max(0, std::min(new_pos.y(), ps.y() - ws.y()));
                    selected_widget->set_position(new_pos);
                    if (original_parent) original_parent->perform_layout(m_nvg_context);
                    new_parent->perform_layout(m_nvg_context);
                    perform_layout();
                    update_properties();
                    selected_widget->dec_ref();
                }
            } else {
                // Same parent: just update position
                Widget* cur = selected_widget->parent();
                if (cur) {
                    new_pos = p - cur->absolute_position() - drag_offset;
                    new_pos = snap(new_pos);
                    Vector2i ps = cur->size(), ws = selected_widget->size();
                    new_pos.x() = std::max(0, std::min(new_pos.x(), ps.x() - ws.x()));
                    new_pos.y() = std::max(0, std::min(new_pos.y(), ps.y() - ws.y()));
                    selected_widget->set_position(new_pos);
                    cur->perform_layout(m_nvg_context);
                    perform_layout();
                    update_properties();
                }
            }
        }
        dragging        = false;
        drag_offset     = Vector2i(0, 0);
        original_parent  = nullptr;
        potential_parent = nullptr;

    } else if (!down && resizing) {
        resizing      = false;
        resize_handle = -1;
        update_properties();

    } else if (!down && rubber_banding) {
        // --- Rubber band released: collect widgets in selection rect ---
        rubber_banding = false;
        selected_widgets.clear();

        Vector2i rb_min = {
            std::min(rubber_band_start.x(), rubber_band_end.x()),
            std::min(rubber_band_start.y(), rubber_band_end.y()) };
        Vector2i rb_max = {
            std::max(rubber_band_start.x(), rubber_band_end.x()),
            std::max(rubber_band_start.y(), rubber_band_end.y()) };

        // Only collect if band has meaningful size
        if (rb_max.x() - rb_min.x() > 2 || rb_max.y() - rb_min.y() > 2) {
            collect_widgets_in_rect(canvas_win, rb_min, rb_max, selected_widgets);

            // Remove redundant entries: if a widget's direct parent is also
            // selected, the parent drag will carry the child along anyway.
            selected_widgets.erase(
                std::remove_if(selected_widgets.begin(), selected_widgets.end(),
                    [this](Widget* w) {
                        for (auto* other : selected_widgets)
                            if (w->parent() == other) return true;
                        return false;
                    }),
                selected_widgets.end());
        }

        // Promote single result to ordinary single selection
        if (selected_widgets.size() == 1) {
            selected_widget = selected_widgets[0];
            selected_widgets.clear();
        } else if (!selected_widgets.empty()) {
            selected_widget = selected_widgets[0]; // primary for properties
        } else {
            selected_widget = nullptr;
        }
        update_properties();
        redraw();

    } else if (!down && group_dragging) {
        // --- Group drag released: move and optionally reparent all widgets ---
        group_dragging = false;
        potential_parent = nullptr;

        Vector2i delta = p - group_drag_start;
        bool actually_dragged = (std::abs(delta.x()) > 5 || std::abs(delta.y()) > 5);

        if (actually_dragged) {
            // Find drop target; reject it if it is (or is inside) any selected widget
            Widget* new_parent = find_deepest_container(canvas_win, p, nullptr);
            for (auto* w : selected_widgets) {
                for (Widget* anc = new_parent; anc; anc = anc->parent()) {
                    if (anc == w) { new_parent = canvas_win; break; }
                }
            }

            for (auto* w : selected_widgets) {
                if (dynamic_cast<TestSplit*>(w->parent())) continue; // never reparent split panes

                Widget* cur = w->parent();
                Vector2i new_abs = group_initial_positions[w] + delta;
                Vector2i new_pos = new_abs - new_parent->absolute_position();
                new_pos = snap(new_pos);
                Vector2i ps = new_parent->size(), ws = w->size();
                new_pos.x() = std::max(0, std::min(new_pos.x(), ps.x() - ws.x()));
                new_pos.y() = std::max(0, std::min(new_pos.y(), ps.y() - ws.y()));

                if (cur != new_parent) {
                    w->inc_ref();
                    cur->remove_child(w);
                    new_parent->add_child(w);
                    w->dec_ref();
                }
                w->set_position(new_pos);
            }
            new_parent->perform_layout(m_nvg_context);
            perform_layout();
            update_properties();
        }
        redraw();
    }

    return false;
}

bool GUIEditor::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (has_modal_dialog()) {
        return Screen::mouse_motion_event(p, rel, button, modifiers);
    }
    if (Screen::mouse_motion_event(p, rel, button, modifiers)) return true;

    // --- Rubber band update ---
    if (rubber_banding) {
        rubber_band_end = p;
        redraw();
        return true;
    }

    // --- Group drag movement ---
    if (group_dragging && !test_mode && (button & (1 << GLFW_MOUSE_BUTTON_1))) {
        Vector2i delta = p - group_drag_start;
        for (auto* w : selected_widgets) {
            if (dynamic_cast<TestSplit*>(w->parent())) continue;
            Widget* par = w->parent();
            Vector2i new_pos = (group_initial_positions[w] + delta) - par->absolute_position();
            new_pos = snap(new_pos);
            Vector2i ps = par->size(), ws = w->size();
            new_pos.x() = std::max(0, std::min(new_pos.x(), ps.x() - ws.x()));
            new_pos.y() = std::max(0, std::min(new_pos.y(), ps.y() - ws.y()));
            w->set_position(new_pos);
        }
        // Highlight the potential drop container
        Widget* new_pot = find_deepest_container(canvas_win, p, nullptr);
        bool self_or_child = false;
        for (auto* w : selected_widgets)
            for (Widget* a = new_pot; a; a = a->parent())
                if (a == w) { self_or_child = true; break; }
        potential_parent = self_or_child ? nullptr : (new_pot == canvas_win ? nullptr : new_pot);
        canvas_win->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    }

    if (dragging && !test_mode && (button & (1 << GLFW_MOUSE_BUTTON_1)) && selected_widget) {
        Widget *cur = selected_widget->parent();
        if (!cur) return false;
        if (canvas_win == selected_widget) {
            dragging        = false;
            drag_offset     = Vector2i(0, 0);
            original_parent = potential_parent = nullptr;
            return false;
        }

        Vector2i new_pos = p - cur->absolute_position() - drag_offset;
        if (selected_widget != canvas_win) new_pos = snap(new_pos);

        Vector2i ps = cur->size(), ws = selected_widget->size();
        new_pos.x() = std::max(0, std::min(new_pos.x(), ps.x() - ws.x()));
        new_pos.y() = std::max(0, std::min(new_pos.y(), ps.y() - ws.y()));
        selected_widget->set_position(new_pos);
        drag_start = p;

        // Update drop-target highlight — deepest container, excluding self
        Widget *new_pot = find_deepest_container(canvas_win, p, selected_widget);
        if (new_pot != selected_widget->parent()
            && new_pot->window() != editor_win
            && new_pot != selected_widget) {
            if (potential_parent != new_pot) { potential_parent = new_pot; redraw(); }
        } else if (potential_parent) {
            potential_parent = nullptr; redraw();
        }

        cur->perform_layout(m_nvg_context);
        perform_layout();
        update_properties();
        return true;

    } else if (resizing && !test_mode && selected_widget) {
        Widget* par = selected_widget->parent();
        if (!par) return false;

        Vector2i delta = (p - par->absolute_position())
                       - (drag_start - par->absolute_position());
        Vector2i new_pos  = resize_start_pos;
        Vector2i new_size = resize_start_size;

        switch (resize_handle) {
            case 0: new_pos.x()  += delta.x(); new_pos.y()  += delta.y();
                    new_size.x() -= delta.x(); new_size.y() -= delta.y(); break;
            case 1: new_pos.y()  += delta.y();
                    new_size.x() += delta.x(); new_size.y() -= delta.y(); break;
            case 2: new_pos.x()  += delta.x();
                    new_size.x() -= delta.x(); new_size.y() += delta.y(); break;
            case 3: new_size.x() += delta.x(); new_size.y() += delta.y(); break;
            case 4: new_pos.y()  += delta.y(); new_size.y() -= delta.y(); break;
            case 5: new_size.y() += delta.y(); break;
            case 6: new_pos.x()  += delta.x(); new_size.x() -= delta.x(); break;
            case 7: new_size.x() += delta.x(); break;
        }
        new_size.x() = std::max(20, new_size.x());
        new_size.y() = std::max(20, new_size.y());

        Vector2i psize = par->size();
        new_pos.x() = std::max(0, std::min(new_pos.x(), psize.x() - new_size.x()));
        new_pos.y() = std::max(0, std::min(new_pos.y(), psize.y() - new_size.y()));
        new_pos = snap(new_pos);

        selected_widget->set_position(new_pos);
        selected_widget->set_fixed_size(new_size);
        par->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    }
    return false;
}

bool GUIEditor::mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (has_modal_dialog()) {
        return Screen::mouse_drag_event(p, rel, button, modifiers);
    }
    if (!test_mode && dragging && selected_widget && (button & (1 << GLFW_MOUSE_BUTTON_1))) {
        if (selected_widget == canvas_win) return false;
        Widget *cur = selected_widget->parent();
        if (!cur) return false;

        Vector2i new_pos = p - cur->absolute_position() - drag_offset;
        Vector2i ps = cur->size(), ws = selected_widget->size();
        new_pos.x() = std::max(0, std::min(new_pos.x(), ps.x() - ws.x()));
        new_pos.y() = std::max(0, std::min(new_pos.y(), ps.y() - ws.y()));
        selected_widget->set_position(new_pos);
        drag_start = p;

        cur->perform_layout(m_nvg_context);
        perform_layout();
        update_properties();
        return true;
    }
    return false;
}

bool GUIEditor::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (has_modal_dialog()) {
        return Screen::keyboard_event(key, scancode, action, modifiers);
    }
    if (Screen::keyboard_event(key, scancode, action, modifiers)) return true;

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        set_visible(false);
        return true;
    }

    // Delete key: remove the selected widget(s) and their children
    if (key == GLFW_KEY_DELETE && action == GLFW_PRESS && !test_mode) {

        // Group delete
        if (selected_widgets.size() > 1) {
            for (auto* w : selected_widgets) {
                if (w == canvas_win || w->window() == editor_win) continue;
                Widget* par = w->parent();
                if (!par) continue;
                set_focused(false);
                notify_widget_destroyed(w);
                w->inc_ref();
                par->remove_child(w);
                w->dec_ref();
            }
            selected_widgets.clear();
            selected_widget = nullptr;
            perform_layout();
            update_properties();
            redraw();
            return true;
        }

        // Single delete
        if (selected_widget && selected_widget != canvas_win
            && selected_widget->window() != editor_win) {
            Widget *par = selected_widget->parent();
            if (par) {
                set_focused(false);
                notify_widget_destroyed(selected_widget);
                selected_widget->inc_ref();
                par->remove_child(selected_widget);
                selected_widget->dec_ref();
                selected_widget = nullptr;
                par->perform_layout(m_nvg_context);
                perform_layout();
                update_properties();
                redraw();
                return true;
            }
        }
    }

    return false;
}

// ============================================================================
// Draw: snap grid dots then all widgets
// ============================================================================
void GUIEditor::draw(NVGcontext *ctx) {
    if (snap_grid_size > 0) {
        Vector2i cpos = canvas_win->absolute_position();
        Vector2i csiz = canvas_win->size();
        nvgSave(ctx);
        nvgFillColor(ctx, Color(255, 255, 255, 255));
        for (int x = snap_grid_size; x <= csiz.x(); x += snap_grid_size) {
            for (int y = snap_grid_size; y <= csiz.y(); y += snap_grid_size) {
                nvgBeginPath(ctx);
                nvgCircle(ctx, cpos.x() + x, cpos.y() + y, 1.0f);
                nvgFill(ctx);
            }
        }
        nvgRestore(ctx);
    }
    Screen::draw(ctx);

    // Draw rubber-band selection rect on top of all widgets
    if (rubber_banding) {
        int x = std::min(rubber_band_start.x(), rubber_band_end.x());
        int y = std::min(rubber_band_start.y(), rubber_band_end.y());
        int w = std::abs(rubber_band_end.x() - rubber_band_start.x());
        int h = std::abs(rubber_band_end.y() - rubber_band_start.y());
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, w, h);
        nvgFillColor(ctx, Color(100, 160, 255, 50));
        nvgFill(ctx);
        nvgStrokeColor(ctx, Color(80, 140, 255, 220));
        nvgStrokeWidth(ctx, 1.5f);
        nvgStroke(ctx);
        nvgRestore(ctx);
    }
}

// ============================================================================
// Resize handle detection (uses shared compute_handle_positions)
// ============================================================================
Widget* GUIEditor::find_widget_with_handle(const Vector2i& p) {
    return find_widget_with_handle_recursive(canvas_win, p);
}

Widget* GUIEditor::find_widget_with_handle_recursive(Widget* w, const Vector2i& p) {
    if (!w || w->window() == editor_win) return nullptr;
    // Depth-first: prefer deepest widget
    for (Widget* child : w->children()) {
        Widget* hit = find_widget_with_handle_recursive(child, p);
        if (hit) return hit;
    }
    auto handles = compute_handle_positions(w->absolute_position(), w->size());
    for (int i = 0; i < 8; ++i) {
        Vector2i rel = p - handles[i];
        if (rel.x() >= 0 && rel.y() >= 0 && rel.x() < 8 && rel.y() < 8) return w;
    }
    return nullptr;
}

// ============================================================================
// Window resize: keep editor panel full height
// ============================================================================
bool GUIEditor::resize_event(const Vector2i &size) {
    if (editor_win) {
        editor_win->set_min_height(size.y());
        perform_layout();
    }
    Screen::resize_event(size);
    return true;
}

// ============================================================================
// Code generation / editor window
// ============================================================================

void GUIEditor::open_code_editor_window() {
    if (!canvas_win) return;

    // Use the shared implementation so we get the full recursive widget JSON
    DictValue* root = guieditor_json::build_canvas_dict(this);
    if (!root) {
        new MessageDialog(this, MessageDialog::Type::Warning,
                          "Code generation failed", "Could not build JSON tree.");
        return;
    }

    int canvas_w = 0, canvas_h = 0;
    DictValue* szv = dict_object_get(root, "size");
    if (szv && szv->type == DICT_ARRAY && szv->array_value.length >= 2) {
        canvas_w = (int)szv->array_value.items[0]->int64_value;
        canvas_h = (int)szv->array_value.items[1]->int64_value;
    }

    // fullmain=false => no mainapp.cpp, only the GuiClass
    Json2CppResult code = json2cpp_generate(root, "GuiClass", canvas_w, canvas_h, 1.0f, /*fullmain*/ false);
    dict_destroy(root);

    if (code.source.empty()) {
        new MessageDialog(this, MessageDialog::Type::Warning,
                          "Code generation failed", "json2cpp_generate returned empty source.");
        return;
    }

    // Create a new floating window with a code-mode TextEditor
    Window* code_win = new Window(this, "Generated C++ (editable)");
    code_win->set_modal(true);   // block interaction with the main canvas while open
    auto* flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::Stretch);
    code_win->set_layout(flex);
    code_win->set_position(Vector2i(120, 80));
    code_win->set_min_size(Vector2i(600, 400));
    code_win->set_resizable(true);

    // Explicit close button (top-right)
    Button *close_btn = new Button(code_win->button_panel(), "", FA_TIMES);
    close_btn->set_tooltip("Close");
    close_btn->set_callback([code_win] {
        // Defer dispose so the current mouse-button release finishes first.
        async([code_win] { code_win->dispose(); });
    });

    auto* editor = new TextEditor(code_win, TextEditor::Mode::Code);
    editor->set_background_color(Color(30, 30, 35, 255));
    Style code_style;
    code_style.fgColor = nvgRGBA(220, 220, 220, 255);
    code_style.fontSize = 13.0f;
    editor->set_code_style(code_style);
    editor->set_min_size({300,200});
    editor->set_height_flex(SizeMode::Expanding);
    editor->set_plain_text(code.source);   // only the class, no main

    // Optional button row
    Widget* btn_row = new Widget(code_win);
    btn_row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 6));
	btn_row->set_min_height(28);
    Button* copy_btn = new Button(btn_row, "Copy to clipboard", FA_CLIPBOARD);

    // Apply Flex items
    flex->set_flex_item(editor,   FlexLayout::FlexItem(1.0f));
    flex->set_flex_item(btn_row,  FlexLayout::FlexItem(1.0f));
    copy_btn->set_callback([copy_btn] {
        copy_btn->set_caption("Copied!");
        if (auto* sc = copy_btn->screen()) sc->redraw();

        // Sleep off the UI thread, then post the revert back to the main thread
        std::thread([copy_btn] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            async([copy_btn] {
                if (copy_btn->screen()) {
                    copy_btn->set_caption("Copy to clipboard");
                    copy_btn->screen()->redraw();
                }
            });
        }).detach();
    });

    perform_layout();
    code_win->center();
    redraw();
}

bool GUIEditor::has_modal_dialog() const {
    for (Widget* w : children()) {
        if (Window* win = dynamic_cast<Window*>(w)) {
            if (win->modal()) return true;
        }
    }
    return false;
}

// ============================================================================
// Entry point
// ============================================================================
int main(int /* argc */, char ** /* argv */) {
    try {
        nanogui::init();
        {
            ref<GUIEditor> app = new GUIEditor();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop();
        }
        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::string msg = std::string("Caught a fatal error: ") + e.what();
#if defined(_WIN32)
        MessageBoxA(nullptr, msg.c_str(), NULL, MB_ICONERROR | MB_OK);
#else
        std::cerr << msg << std::endl;
#endif
        return -1;
    } catch (...) {
        std::cerr << "Caught an unknown error!" << std::endl;
    }
    return 0;
}
