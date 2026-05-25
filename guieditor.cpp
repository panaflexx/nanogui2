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
#include <nanogui/menu.h>

#include <iostream>
#if !defined(__linux__)
#include <memory>
#include <sstream>
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
# pragma warning(disable: 4505) // don't warn about dead code in stb_image.h
#elif defined(__GNUC__)
# pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>

/* NEW: TEST MODE MANAGER - SINGLETON TO TRACK TEST MODE STATE */
class TestModeManager {
private:
    bool m_testModeEnabled = false;
    static TestModeManager* instance;

    TestModeManager() {}

public:
    static TestModeManager* getInstance() {
        if (!instance) {
            instance = new TestModeManager();
        }
        return instance;
    }

    bool isTestModeEnabled() const { return m_testModeEnabled; }
    void setTestModeEnabled(bool enabled) { m_testModeEnabled = enabled; }
};

TestModeManager* TestModeManager::instance = nullptr;

/* NEW: BASE CLASS FOR ALL TEST-MODE WIDGETS */
class TestWidget : public Widget {
public:
    TestWidget(Widget *parent) : Widget(parent) {}

    /* OVERRIDE: Block input events when test mode is OFF */
    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false; // Block event when test mode is off
        }
        return Widget::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Widget::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Widget::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Widget::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Widget::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Widget::keyboard_character_event(codepoint);
    }

    /* OVERRIDE: Draw border (green if selected, red if test mode is OFF and not selected) */
    virtual void draw(NVGcontext *ctx) override {
		GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        if (editor && editor->dragging && editor->potential_parent == this) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgFillColor(ctx, Color(255, 255, 0, 120)); // Semi-transparent yellow
            nvgFill(ctx);
            nvgRestore(ctx);
        }

        Widget::draw(ctx);

        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw border only when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x() + 1, m_pos.y() + 1, m_size.x() - 1, m_size.y() - 1);
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }
    }
};

/* NEW: TEST-MODE SPECIFIC WIDGET CLASSES */
class TestLabel : public Label {
public:
    TestLabel(Widget *parent, const std::string &caption = "", const std::string &font = "sans")
        : Label(parent, caption, font) {}

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Label::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Label::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Label::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Label::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Label::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Label::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        Label::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw border only when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }
    }
};

class TestButton : public Button {
public:
    TestButton(Widget *parent, const std::string &caption = "", int icon = 0)
        : Button(parent, caption, icon) {}

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Button::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Button::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Button::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Button::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Button::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Button::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        Button::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw border only when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }
    }
};

class TestTextBox : public TextBox {
public:
    TestTextBox(Widget *parent) : TextBox(parent) {}

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return TextBox::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return TextBox::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return TextBox::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return TextBox::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return TextBox::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return TextBox::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        TextBox::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw border and text when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);

            // Draw "EDIT MODE OFF" text only when not selected and test mode is OFF
            if (!(editor && editor->selected_widget == this) && !TestModeManager::getInstance()->isTestModeEnabled()) {
                nvgFontSize(ctx, 12.0f);
                nvgFontFace(ctx, "sans");
                nvgFillColor(ctx, Color(255, 0, 0, 255));
                nvgText(ctx, m_pos.x() + 5, m_pos.y() + 15, "EDIT MODE OFF", nullptr);
            }
            nvgRestore(ctx);
        }
    }
};

class TestDropdown : public Dropdown {
public:
    TestDropdown(Widget *parent, const std::vector<std::string> &items = {})
        : Dropdown(parent, Dropdown::ComboBox, "Dropdown") {

    }

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Dropdown::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Dropdown::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Dropdown::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Dropdown::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Dropdown::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Dropdown::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        Dropdown::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw borders when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);

            // Draw inner red border only when not selected and test mode is OFF
            if (!(editor && editor->selected_widget == this) && !TestModeManager::getInstance()->isTestModeEnabled()) {
                nvgBeginPath(ctx);
                nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
                nvgStrokeColor(ctx, Color(255, 0, 0, 255));
                nvgStrokeWidth(ctx, 1.0f);
                nvgStroke(ctx);
            }
            nvgRestore(ctx);
        }
    }
};

class TestCheckBox : public CheckBox {
public:
    TestCheckBox(Widget *parent, const std::string &caption = "",
                const std::function<void(bool)> &callback = {})
        : CheckBox(parent, caption, callback) {}

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return CheckBox::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return CheckBox::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return CheckBox::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return CheckBox::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return CheckBox::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return CheckBox::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        CheckBox::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw borders when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);

            // Draw red circle only when not selected and test mode is OFF
            if (!(editor && editor->selected_widget == this) && !TestModeManager::getInstance()->isTestModeEnabled()) {
                nvgBeginPath(ctx);
                nvgCircle(ctx, m_pos.x() + 10, m_pos.y() + 10, 8);
                nvgStrokeColor(ctx, Color(255, 0, 0, 255));
                nvgStrokeWidth(ctx, 1.5f);
                nvgStroke(ctx);
            }
            nvgRestore(ctx);
        }
    }
};

class TestSlider : public Slider {
public:
    TestSlider(Widget *parent) : Slider(parent) {}

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Slider::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Slider::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Slider::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Slider::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Slider::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Slider::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        Slider::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw borders when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);

            // Draw extra red border only when not selected and test mode is OFF
            if (!(editor && editor->selected_widget == this) && !TestModeManager::getInstance()->isTestModeEnabled()) {
                nvgBeginPath(ctx);
                nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
                nvgStrokeColor(ctx, Color(255, 0, 0, 255));
                nvgStrokeWidth(ctx, 1.5f);
                nvgStroke(ctx);
            }
            nvgRestore(ctx);
        }
    }
};

class TestColorPicker : public ColorPicker {
public:
    TestColorPicker(Widget *parent, const Color &color = Color(0, 255))
        : ColorPicker(parent, color) {}

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return ColorPicker::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false; // Fixed: Added return statement
        }
        return ColorPicker::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return ColorPicker::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return ColorPicker::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return ColorPicker::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return ColorPicker::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        ColorPicker::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw borders and text when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);

            // Draw extra red border and text only when not selected and test mode is OFF
            if (!(editor && editor->selected_widget == this) && !TestModeManager::getInstance()->isTestModeEnabled()) {
                nvgBeginPath(ctx);
                nvgRoundedRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y(), 4);
                nvgStrokeColor(ctx, Color(255, 0, 0, 255));
                nvgStrokeWidth(ctx, 1.5f);
                nvgStroke(ctx);

                nvgFontSize(ctx, 12.0f);
                nvgFontFace(ctx, "sans");
                nvgFillColor(ctx, Color(255, 255, 255, 255));
                nvgTextAlign(ctx, NVG_ALIGN_CENTER);
                nvgText(ctx, m_pos.x() + m_size.x() / 2.0f, m_pos.y() + m_size.y() / (2.0 + 15.0), "DISABLED", nullptr);
            }
            nvgRestore(ctx);
        }
    }
};

class TestWindow : public Window {
public:
    TestWindow(Widget *parent, const std::string &title = "", bool modal = false)
        : Window(parent, title, modal) {}

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Window::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Window::mouse_motion_event(p, rel, button, modifiers);
    }

    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Window::scroll_event(p, rel);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Window::mouse_drag_event(p, rel, button, modifiers);
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Window::keyboard_event(key, scancode, action, modifiers);
    }

    virtual bool keyboard_character_event(unsigned int codepoint) override {
        if (!TestModeManager::getInstance()->isTestModeEnabled()) {
            return false;
        }
        return Window::keyboard_character_event(codepoint);
    }

    virtual void draw(NVGcontext *ctx) override {
        Window::draw(ctx);

        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());
        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        // Draw border and text when selected or test mode is OFF
        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 2.0f);
            nvgStroke(ctx);

            // Draw text only when not selected and test mode is OFF
            if (!(editor && editor->selected_widget == this) && !TestModeManager::getInstance()->isTestModeEnabled()) {
                nvgFontSize(ctx, 14.0f);
                nvgFontFace(ctx, "sans");
                nvgFillColor(ctx, Color(255, 255, 255, 255));
                nvgTextAlign(ctx, NVG_ALIGN_CENTER);
                nvgText(ctx, m_pos.x() + m_size.x() / 2.0f, m_pos.y() + 20, "TEST MODE: OFF", nullptr);
            }
            nvgRestore(ctx);
        }
    }
};

class TestCanvasWindow : public Window {
public:
    TestCanvasWindow(Widget *parent, const std::string &title = "")
        : Window(parent, title) {}

    virtual void draw(NVGcontext *ctx) override {
        // Draw yellow highlight if this is the potential parent during dragging
        GUIEditor* editor = dynamic_cast<GUIEditor*>(screen());

        Window::draw(ctx);

        if (editor && editor->dragging && editor->potential_parent == this) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgFillColor(ctx, Color(255, 255, 0, 120)); // Semi-transparent yellow
            nvgFill(ctx);
            nvgRestore(ctx);
        }

        Color border = (editor && editor->selected_widget == this) ? Color(0, 255, 0, 255) : Color(255, 0, 0, 255);

        if ((editor && editor->selected_widget == this) || !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgBeginPath(ctx);
            nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
            nvgStrokeColor(ctx, border);
            nvgStrokeWidth(ctx, (editor && editor->selected_widget == this) ? 2.0f : 1.5f);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }

		// New: Draw resize handles if selected and test mode is OFF
        if (editor && editor->selected_widget && !TestModeManager::getInstance()->isTestModeEnabled()) {
            nvgSave(ctx);
            nvgResetScissor(ctx); // Reset scissor to allow drawing outside clipped area
			// Get selected widget bounds
			Vector2i w_pos = editor->selected_widget->absolute_position();
			Vector2i w_size = editor->selected_widget->size();

            float hs = 8.0f; // Handle size
            Color handleFill = Color(255, 255, 255, 255); // White fill
            Color handleStroke = Color(0, 0, 0, 128); // Black outline

            nvgBeginPath(ctx);
            // Top-left
            nvgRect(ctx, w_pos.x() - hs / 2, w_pos.y() - hs / 2, hs, hs);
            // Top-right
            nvgRect(ctx, w_pos.x() + w_size.x() - hs / 2, w_pos.y() - hs / 2, hs, hs);
            // Bottom-left
            nvgRect(ctx, w_pos.x() - hs / 2, w_pos.y() + w_size.y() - hs / 2, hs, hs);
            // Bottom-right
            nvgRect(ctx, w_pos.x() + w_size.x() - hs / 2, w_pos.y() + w_size.y() - hs / 2, hs, hs);
            // Top-middle
            nvgRect(ctx, w_pos.x() + w_size.x() / 2.0 - hs / 2.0, w_pos.y() - hs / 2, hs, hs);
            // Bottom-middle
            nvgRect(ctx, w_pos.x() + w_size.x() / 2.0 - hs / 2.0, w_pos.y() + w_size.y() - hs / 2, hs, hs);
            // Left-middle
            nvgRect(ctx, w_pos.x() - hs / 2, w_pos.y() + w_size.y() / 2.0 - hs / 2.0, hs, hs);
            // Right-middle
            nvgRect(ctx, w_pos.x() + w_size.x() - hs / 2.0, w_pos.y() + w_size.y() / 2.0 - hs / 2, hs, hs);

            nvgFillColor(ctx, handleFill);
            nvgFill(ctx);
            nvgStrokeColor(ctx, handleStroke);
            nvgStrokeWidth(ctx, 1.0f);
            nvgStroke(ctx);

            nvgRestore(ctx);
        }
    }
};

/* GUIEditor Implementations */
GUIEditor::GUIEditor() : Screen(Vector2i(1024, 768), "GUI Editor") {
    // Editor window
    editor_win = new Window(this, "");
    editor_win->set_position(Vector2i(0, 0));
    editor_win->set_layout(new BoxLayout(Orientation::Vertical));
    editor_win->set_min_width(250);
    editor_win->set_min_height(size().y());

    // Toolbar: 4x4 grid
    Widget *toolbar = new Widget(editor_win);
    toolbar->set_layout(new GridLayout(Orientation::Horizontal, 4, Alignment::Minimum, 5, 5));

    std::vector<int> toolbar_icons = {
        FA_MOUSE_POINTER, // Select
        FA_WINDOW_MAXIMIZE, // Window
        FA_TH, // Pane (Widget)
        FA_COLUMNS, // Split
        FA_TAG, // Label
        FA_KEYBOARD, // Textbox
        FA_HAND_POINT_UP, // Button
        FA_CARET_DOWN, // Dropdown
        FA_CHECK_SQUARE, // Checkbox
        FA_SLIDERS_H, // Slider
        FA_PALETTE, // Color Picker
        FA_CHART_LINE, // Graph
        FA_IMAGE, // Image
        FA_FOLDER_OPEN, // Folder Dialog
        FA_QUESTION_CIRCLE, // Placeholder
        FA_TRASH // Delete (placeholder)
    };

    std::vector<std::string> toolbar_tooltips = {
        "Select Tool",
        "Window",
        "Widget Pane",
        "Split View",
        "Label",
        "Text Box",
        "Button",
        "Dropdown",
        "Checkbox",
        "Slider",
        "Color Picker",
        "Graph",
        "Image",
        "Folder Dialog",
        "Placeholder",
        "Delete"
    };

    for (size_t i = 0; i < toolbar_icons.size(); ++i) {
        ToolButton *tb = new ToolButton(toolbar, toolbar_icons[i]);
        tb->set_flags(Button::ToggleButton);
        tb->set_tooltip(toolbar_tooltips[i]);
        tb->set_callback([this, tb, icon = toolbar_icons[i]] {
            for (auto b : tool_buttons) {
                if (b != tb) b->set_pushed(false);
            }
            tb->set_pushed(true);
            current_tool = icon;
        });
        tool_buttons.push_back(tb);
    }

    // Load/Save buttons row
    Widget *fileRow = new Widget(editor_win);
    fileRow->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 5));
    Button *load_btn = new Button(fileRow, "Load", FA_FOLDER_OPEN);
    load_btn->set_tooltip("Load layout from JSON file");
    load_btn->set_callback([this] {
        auto results = nanogui::file_dialog(
            { {"json", "JSON Layout"} }, false, false, "");
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
        auto results = nanogui::file_dialog(
            { {"json", "JSON Layout"} }, true, false, "");
        if (results.empty()) return;
        std::string path = results.front();
        if (path.empty()) return;
        if (!guieditor_json::save_layout(this, path)) {
            new MessageDialog(this, MessageDialog::Type::Warning,
                              "Save failed", "Could not save JSON layout to " + path);
        }
    });

    // Test mode toggle
    Widget *testModeRow = new Widget(editor_win);
    testModeRow->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Fill, 0, 5));

    test_mode_checkbox = new CheckBox(testModeRow, "Test Mode");
    test_mode_checkbox->set_height(25);
    test_mode_checkbox->set_min_height(25);
    test_mode_checkbox->set_callback([this](bool checked) {
        selected_widget = nullptr; // deselect current widget focus
        TestModeManager::getInstance()->setTestModeEnabled(checked);

        // Force redraw to update all widgets' appearance
        perform_layout();
        draw_all();
		//deferred_tasks.push_back([this] { update_properties(); });
		async([this] { printf("run async\n"); update_properties(); });
    });

    // Set initial state
    test_mode_checkbox->set_checked(false);
    TestModeManager::getInstance()->setTestModeEnabled(false);

    // Properties pane
    auto plabel = new Label(editor_win, "Properties", "sans-bold");
	plabel->set_fixed_size( Vector2i(200,25) );

    properties_pane = new Widget(editor_win);
    GridLayout* layout = new GridLayout(Orientation::Horizontal, 2,
                                       Alignment::Middle, 15, 5);
    layout->set_col_alignment({ Alignment::Maximum, Alignment::Fill });
    layout->set_spacing(Orientation::Horizontal, 10);
    properties_pane->set_layout(layout);

    update_properties();

    // Canvas window (empty for placing widgets)
    //canvas_win = new Window(this, "Canvas");
	canvas_win = new TestCanvasWindow(this, "Canvas");
    canvas_win->set_position(Vector2i(280, 15));
    canvas_win->set_size(Vector2i(100, 70));
    canvas_win->set_layout(nullptr); // Absolute positioning
    canvas_win->set_id("CANVAS"); // Set ID for canvas_win

    perform_layout();
    canvas_win->set_size(Vector2i(700, 700));
}

Vector2i GUIEditor::snap(const Vector2i &pos) {
    if (snap_grid_size == 0) return pos;
    return Vector2i((pos.x() / snap_grid_size) * snap_grid_size, (pos.y() / snap_grid_size) * snap_grid_size);
}

bool GUIEditor::update_properties() {
    // Clear existing properties
    while (properties_pane->child_count() > 0) {
        properties_pane->remove_child(properties_pane->child_at(properties_pane->child_count() - 1));
    }

    if (!selected_widget) {
        new Label(properties_pane, "No widget selected");
        perform_layout();
        redraw();
        return false;
    }

	if (selected_widget == canvas_win) {
        new Label(properties_pane, "Snapping:", "sans-bold");
		Dropdown *snap_combo = new Dropdown(properties_pane, {"Off", "5", "10", "15", "20", "25"},
											{}, Dropdown::Mode::ComboBox, "Snapping");
        //ComboBox *snap_combo = new ComboBox(properties_pane, {"Off", "5", "10", "15", "20", "25"});
        int idx = (snap_grid_size == 0) ? 0 : (snap_grid_size / 5);
        snap_combo->set_selected_index(idx);
        snap_combo->set_selected_callback([this](int index) {
            snap_grid_size = (index == 0) ? 0 : (index * 5);
        });
        snap_combo->set_min_height(20);

        new Label(properties_pane, "Resizable:", "sans-bold");
		CheckBox *resize_checkbox = new CheckBox(properties_pane, "");
		resize_checkbox->set_checked( canvas_win->resizable() );
		resize_checkbox->set_callback([this](bool checked) {
			canvas_win->set_resizable( checked );
			// Force redraw to update all widgets' appearance
			//canvas_win->perform_layout();
		});

    }

    /* WIDGET TYPE DISPLAY */
    new Label(properties_pane, "Widget:", "sans-bold");
    TextBox *type_box = new TextBox(properties_pane);
    type_box->set_value(getWidgetTypeName(selected_widget));
    type_box->set_editable(false);
    type_box->set_min_height(20);

    /* PARENT ID DISPLAY */
    new Label(properties_pane, "Parent ID:", "sans-bold");
    TextBox *parent_id_box = new TextBox(properties_pane);
    Widget *parent = selected_widget->parent();
    parent_id_box->set_value(parent && parent != this ? parent->id() : "None");
    parent_id_box->set_editable(false);
    parent_id_box->set_min_height(20);

    /* UNIQUE ID DISPLAY */
    new Label(properties_pane, "ID:", "sans-bold");
    TextBox *id_box = new TextBox(properties_pane);
    id_box->set_value(selected_widget->id());
    id_box->set_callback([this](const std::string &v) {
        if (!selected_widget) return false;
        selected_widget->set_id(v);
        perform_layout();
        redraw();
        return true;
    });
    id_box->set_min_height(20);

    /* TYPE-SPECIFIC TEXT PROPERTIES */
    if (Label *lbl = dynamic_cast<Label *>(selected_widget)) {
        new Label(properties_pane, "Caption:", "sans-bold");
        TextBox *caption_box = new TextBox(properties_pane);
        caption_box->set_value(lbl->caption());
        caption_box->set_callback([this, lbl](const std::string &v) {
            if (!selected_widget) return false;
            lbl->set_caption(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout();
            redraw();
            return true;
        });
        caption_box->set_min_height(20);
    } else if (CheckBox *cb = dynamic_cast<CheckBox *>(selected_widget)) {
        new Label(properties_pane, "Caption:", "sans-bold");
        TextBox *caption_box = new TextBox(properties_pane);
        caption_box->set_value(cb->caption());
        caption_box->set_callback([this, cb](const std::string &v) {
            if (!selected_widget) return false;
            cb->set_caption(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout();
            redraw();
            return true;
        });
        caption_box->set_min_height(20);
    } else if (Window *win = dynamic_cast<Window *>(selected_widget)) {
        new Label(properties_pane, "Title:", "sans-bold");
        TextBox *title_box = new TextBox(properties_pane);
        title_box->set_value(win->title());
        title_box->set_callback([this, win](const std::string &v) {
            if (!selected_widget) return false;
            win->set_title(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout();
            redraw();
            return true;
        });
        title_box->set_min_height(20);
    } else if (TextBox *tb = dynamic_cast<TextBox *>(selected_widget)) {
        new Label(properties_pane, "Value:", "sans-bold");
        TextBox *value_box = new TextBox(properties_pane);
        value_box->set_value(tb->value());
        value_box->set_callback([this, tb](const std::string &v) {
            if (!selected_widget) return false;
            tb->set_value(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout();
            redraw();
            return true;
        });
        value_box->set_min_height(20);
    } else if (Dropdown *dropdown = dynamic_cast<Dropdown *>(selected_widget)) {
        new Label(properties_pane, "Items:", "sans-bold");
        Widget *items_container = new Widget(properties_pane);
        items_container->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 5));
		for (int idx = 0; idx < dropdown->popup()->child_count(); ++idx) {
			if (MenuItem *mi = dynamic_cast<MenuItem*>(dropdown->popup()->child_at(idx))) {
                Widget *row = new Widget(items_container);
                row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 5));
                TextBox *caption_box = new TextBox(row);
                caption_box->set_value(mi->caption());
                caption_box->set_min_width(150);
                caption_box->set_callback([mi](const std::string &v) {
                    mi->set_caption(v);
					/*
                    if (!v.empty()) {
                        mi->set_shortcuts({{GLFW_MOD_SUPER, (int)v[0]}});
                    } else {
                        mi->set_shortcuts({});
                    }
					*/
                    //perform_layout();
                    //redraw();
					//deferred_tasks.push_back([this] { update_properties(); });
					//async([this] { printf("run async\n"); update_properties(); });
                    return true;
                });
                Button *remove_btn = new Button(row, "", FA_MINUS);
                remove_btn->set_min_width(30);
                remove_btn->set_callback([this, idx, dropdown] {
					//deferred_tasks.push_back([this] { update_properties(); });
					dropdown->remove_item(idx);
					async([this] {
						printf("run async\n");
						update_properties();
					});
                });
            }
        }
        // Add new item button
        Widget *add_row = new Widget(items_container);
        add_row->set_layout(new BoxLayout(Orientation::Horizontal));
        Button *add_btn = new Button(add_row, "", FA_PLUS);
        add_btn->set_min_width(30);
        add_btn->set_callback([this, dropdown] {
            std::string new_caption = "New";
            Widget *child = dropdown->add_item({new_caption, new_caption + "_tooltip"}, 0, nullptr, {0,0}, true);
			if (MenuItem *mi = dynamic_cast<MenuItem*>(child)) {
				mi->set_callback([mi] { std::cout << "Selected item: " << mi->caption() << "\n"; });
			}
			async([this] { printf("run async\n"); update_focus(nullptr); update_properties(); });
        });
    } else if (Button *btn = dynamic_cast<Button *>(selected_widget)) {
        new Label(properties_pane, "Caption:", "sans-bold");
        TextBox *caption_box = new TextBox(properties_pane);
        caption_box->set_value(btn->caption());
        caption_box->set_callback([this, btn](const std::string &v) {
            if (!selected_widget) return false;
            btn->set_caption(v);
            selected_widget->perform_layout(m_nvg_context);
            perform_layout();
            redraw();
            return true;
        });
        caption_box->set_min_height(20);
    }

    /* LAYOUT CONTROLS - Only for container widgets */
    if (canHaveLayout(selected_widget)) {
        new Label(properties_pane, "Layout:", "sans-bold");
        ComboBox *layout_combo = new ComboBox(properties_pane, {
            "None", "Box Layout", "Grid Layout", "Advanced Grid", "Flex Layout", "Group Layout"
        });

        std::string current_layout = getCurrentLayoutType(selected_widget);
        layout_combo->set_selected_index(getLayoutTypeIndex(current_layout));

        layout_combo->set_callback([this](int index) {
            if (!selected_widget) return;
            applyLayoutType(selected_widget, index);
            update_properties();
        });
        layout_combo->set_min_height(20);

        addLayoutSpecificControls(selected_widget);
    }

    /* POSITION X */
    new Label(properties_pane, "Position X:", "sans-bold");
    IntBox<int> *pos_x = new IntBox<int>(properties_pane);
    pos_x->set_value(selected_widget->position().x());
    pos_x->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i pos = selected_widget->position();
        pos.x() = v;
        selected_widget->set_position(pos);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    });
    pos_x->set_min_height(20);

    /* POSITION Y */
    new Label(properties_pane, "Position Y:", "sans-bold");
    IntBox<int> *pos_y = new IntBox<int>(properties_pane);
    pos_y->set_value(selected_widget->position().y());
    pos_y->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i pos = selected_widget->position();
        pos.y() = v;
        selected_widget->set_position(pos);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    });
    pos_y->set_min_height(20);

    /* WIDTH */
    new Label(properties_pane, "Width:", "sans-bold");
    IntBox<int> *width_box = new IntBox<int>(properties_pane);
    width_box->set_value(selected_widget->width());
    width_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i size = selected_widget->size();
        size.x() = v;
        selected_widget->set_size(size);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    });
    width_box->set_min_height(20);

    /* HEIGHT */
    new Label(properties_pane, "Height:", "sans-bold");
    IntBox<int> *height_box = new IntBox<int>(properties_pane);
    height_box->set_value(selected_widget->height());
    height_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i size = selected_widget->size();
        size.y() = v;
        selected_widget->set_size(size);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    });
    height_box->set_min_height(20);

	/* FIXED WIDTH */
    new Label(properties_pane, "Fxd Width:", "sans-bold");
    IntBox<int> *fwidth_box = new IntBox<int>(properties_pane);
    fwidth_box->set_value(selected_widget->fixed_width());
    fwidth_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i size = selected_widget->size();
        size.x() = v;
        selected_widget->set_fixed_size(size);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    });
    fwidth_box->set_min_height(20);

    /* FIXED HEIGHT */
    new Label(properties_pane, "Fxd Height:", "sans-bold");
    IntBox<int> *fheight_box = new IntBox<int>(properties_pane);
    fheight_box->set_value(selected_widget->fixed_height());
    fheight_box->set_callback([this](int v) {
        if (!selected_widget) return false;
        Vector2i size = selected_widget->size();
        size.y() = v;
        selected_widget->set_fixed_size(size);
        selected_widget->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    });
    fheight_box->set_min_height(20);


    /* ANIMATION CONTROLS */
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

    /* ANIMATION DURATION (seconds) */
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

    /* ANIMATION START / STOP BUTTONS */
    new Label(properties_pane, "Anim Run:", "sans-bold");
    Widget *anim_buttons = new Widget(properties_pane);
    anim_buttons->set_layout(new BoxLayout(Orientation::Horizontal,
                                           Alignment::Middle, 0, 5));
    Button *anim_start_btn = new Button(anim_buttons, "Start", FA_PLAY);
    anim_start_btn->set_tooltip("Start the selected widget's animation");
    anim_start_btn->set_callback([this]() {
        if (!selected_widget) return;
        // Ensure visibility so animations like SlideOpen are observable
        selected_widget->set_visible(true);
        selected_widget->start_animation();
        redraw();
    });
    Button *anim_stop_btn = new Button(anim_buttons, "Stop", FA_STOP);
    anim_stop_btn->set_tooltip("Stop the selected widget's animation");
    anim_stop_btn->set_callback([this]() {
        if (!selected_widget) return;
        selected_widget->stop_animation();
        redraw();
    });


    /* BACKGROUND COLOR */
    new Label(properties_pane, "BG Color:", "sans-bold");
    ColorPicker *bg_color = new ColorPicker(properties_pane);
    bg_color->set_callback([this](const Color /*&c*/) {
        if (!selected_widget) return false;
        // Placeholder for background color implementation
        perform_layout();
        redraw();
        return true;
    });
    bg_color->set_min_height(20);

    perform_layout();
    redraw();
    return true;
}

bool GUIEditor::canHaveLayout(Widget* widget) {
    return dynamic_cast<Window*>(widget) ||
           dynamic_cast<TestWindow*>(widget) ||
           dynamic_cast<TestWidget*>(widget) ||
           (widget != canvas_win && widget->child_count() > 0);
}

std::string GUIEditor::getCurrentLayoutType(Widget* widget) {
    Layout* layout = widget->layout();
    if (!layout) return "None";

    if (dynamic_cast<BoxLayout*>(layout)) return "Box Layout";
    if (dynamic_cast<GridLayout*>(layout)) return "Grid Layout";
    if (dynamic_cast<AdvancedGridLayout*>(layout)) return "Advanced Grid";
    if (dynamic_cast<FlexLayout*>(layout)) return "Flex Layout";
    if (dynamic_cast<GroupLayout*>(layout)) return "Group Layout";

    return "Unknown";
}

int GUIEditor::getLayoutTypeIndex(const std::string& type) {
    if (type == "None") return 0;
    if (type == "Box Layout") return 1;
    if (type == "Grid Layout") return 2;
    if (type == "Advanced Grid") return 3;
    if (type == "Flex Layout") return 4;
    if (type == "Group Layout") return 5;
    return 0;
}

void GUIEditor::applyLayoutType(Widget* widget, int type_index) {
    Layout* new_layout = nullptr;

    switch (type_index) {
        case 0: // None
            new_layout = nullptr;
            break;
        case 1: // Box Layout
            new_layout = new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 5);
            break;
        case 2: // Grid Layout
            new_layout = new GridLayout(Orientation::Horizontal, 2, Alignment::Fill, 10, 5);
            break;
        case 3: // Advanced Grid
            new_layout = new AdvancedGridLayout({100, 100}, {30, 30}, 10);
            break;
        case 4: // Flex Layout
            new_layout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                      AlignItems::Stretch, 10, 5);
            break;
        case 5: // Group Layout
            new_layout = new GroupLayout(10, 5, 15, 5);
            break;
    }

    widget->set_layout(new_layout);
    widget->perform_layout(m_nvg_context);
    // Ensure parent widgets update their layout
    if (Widget* parent = widget->parent()) {
        parent->perform_layout(m_nvg_context);
    }
    perform_layout();
    redraw();
}

void GUIEditor::addLayoutSpecificControls(Widget* widget) {
    Layout* layout = widget->layout();
    if (!layout) return;

    // Box Layout Controls
    if (BoxLayout* box_layout = dynamic_cast<BoxLayout*>(layout)) {
        addBoxLayoutControls(box_layout);
    }
    // Grid Layout Controls
    else if (GridLayout* grid_layout = dynamic_cast<GridLayout*>(layout)) {
        addGridLayoutControls(grid_layout);
    }
    // Flex Layout Controls
    else if (FlexLayout* flex_layout = dynamic_cast<FlexLayout*>(layout)) {
        addFlexLayoutControls(flex_layout);
    }
    // Group Layout Controls
    else if (GroupLayout* group_layout = dynamic_cast<GroupLayout*>(layout)) {
        addGroupLayoutControls(group_layout);
    }
}

void GUIEditor::addBoxLayoutControls(BoxLayout* layout) {
    // Orientation
    new Label(properties_pane, "Orientation:", "sans-bold");
    ComboBox *orientation_combo = new ComboBox(properties_pane, {"Horizontal", "Vertical"});
    orientation_combo->set_selected_index(layout->orientation() == Orientation::Horizontal ? 0 : 1);
    orientation_combo->set_callback([this, layout](int index) {
        layout->set_orientation(index == 0 ? Orientation::Horizontal : Orientation::Vertical);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
    });
    orientation_combo->set_min_height(20);

    // Alignment
    new Label(properties_pane, "Alignment:", "sans-bold");
    ComboBox *align_combo = new ComboBox(properties_pane, {"Minimum", "Middle", "Maximum", "Fill"});
    align_combo->set_selected_index((int)layout->alignment());
    align_combo->set_callback([this, layout](int index) {
        layout->set_alignment((Alignment)index);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
    });
    align_combo->set_min_height(20);

    // Margin
    new Label(properties_pane, "Margin:", "sans-bold");
    IntBox<int> *margin_box = new IntBox<int>(properties_pane);
    margin_box->set_value(layout->margin());
    margin_box->set_callback([this, layout](int v) {
        layout->set_margin(v);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
        return true;
    });
    margin_box->set_min_height(20);

    // Spacing
    new Label(properties_pane, "Spacing:", "sans-bold");
    IntBox<int> *spacing_box = new IntBox<int>(properties_pane);
    spacing_box->set_value(layout->spacing());
    spacing_box->set_callback([this, layout](int v) {
        layout->set_spacing(v);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
        return true;
    });
    spacing_box->set_min_height(20);
}

void GUIEditor::addGridLayoutControls(GridLayout* layout) {
    // Resolution (columns/rows)
    new Label(properties_pane, "Resolution:", "sans-bold");
    IntBox<int> *resolution_box = new IntBox<int>(properties_pane);
    resolution_box->set_value(layout->resolution());
    resolution_box->set_callback([this, layout](int v) {
        layout->set_resolution(std::max(1, v));
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
        return true;
    });
    resolution_box->set_min_height(20);

    // Orientation
    new Label(properties_pane, "Orientation:", "sans-bold");
    ComboBox *orientation_combo = new ComboBox(properties_pane, {"Horizontal", "Vertical"});
    orientation_combo->set_selected_index(layout->orientation() == Orientation::Horizontal ? 0 : 1);
    orientation_combo->set_callback([this, layout](int index) {
        layout->set_orientation(index == 0 ? Orientation::Horizontal : Orientation::Vertical);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
    });
    orientation_combo->set_min_height(20);

    // Tables
	// TODO add themes
    new Label(properties_pane, "Table:", "sans-bold");
    ComboBox *table_combo = new ComboBox(properties_pane, {"None", "Pinkish Theme"});
    //orientation_combo->set_selected_index(layout->orientation() == Orientation::Horizontal ? 0 : 1);
    table_combo->set_callback([this, layout](int index) {
		if(index > 0) {
			TableTheme theme;
			theme.header_background = Color(100, 100, 100, 255);
			theme.even_row_background = Color(150, 140, 140, 255);
			theme.odd_row_background = Color(175, 155, 155, 255);
			theme.border_color = Color(50, 50, 50, 128);
			theme.border_width = 1.0f;
			theme.shade_rows = true;
			theme.first_row_is_header = true;
			if( selected_widget->layout() ) {
				layout->enable_draw_table(theme);
				layout->set_margin( 4 );
			}
		} else {
				layout->disable_draw_table();
		}

        perform_layout();
        redraw();
    });
    orientation_combo->set_min_height(20);
}

void GUIEditor::addFlexLayoutControls(FlexLayout* layout) {
    // Flex Direction
    new Label(properties_pane, "Direction:", "sans-bold");
    ComboBox *direction_combo = new ComboBox(properties_pane, {
        "Row", "Row Reverse", "Column", "Column Reverse"
    });
    direction_combo->set_selected_index((int)layout->direction());
    direction_combo->set_callback([this, layout](int index) {
        layout->set_direction((FlexDirection)index);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
    });
    direction_combo->set_min_height(20);

    // Justify Content
    new Label(properties_pane, "Justify:", "sans-bold");
    ComboBox *justify_combo = new ComboBox(properties_pane, {
        "Flex Start", "Flex End", "Center", "Space Between", "Space Around", "Space Evenly"
    });
    justify_combo->set_selected_index((int)layout->justify_content());
    justify_combo->set_callback([this, layout](int index) {
        layout->set_justify_content((JustifyContent)index);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
    });
    justify_combo->set_min_height(20);

    // Align Items
    new Label(properties_pane, "Align Items:", "sans-bold");
    ComboBox *align_combo = new ComboBox(properties_pane, {
        "Flex Start", "Flex End", "Center", "Stretch", "Baseline"
    });
    align_combo->set_selected_index((int)layout->align_items());
    align_combo->set_callback([this, layout](int index) {
        layout->set_align_items((AlignItems)index);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
    });
    align_combo->set_min_height(20);
}

void GUIEditor::addGroupLayoutControls(GroupLayout* layout) {
    // Margin
    new Label(properties_pane, "Margin:", "sans-bold");
    IntBox<int> *margin_box = new IntBox<int>(properties_pane);
    margin_box->set_value(layout->margin());
    margin_box->set_callback([this, layout](int v) {
        layout->set_margin(v);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
        return true;
    });
    margin_box->set_min_height(20);

    // Spacing
    new Label(properties_pane, "Spacing:", "sans-bold");
    IntBox<int> *spacing_box = new IntBox<int>(properties_pane);
    spacing_box->set_value(layout->spacing());
    spacing_box->set_callback([this, layout](int v) {
        layout->set_spacing(v);
        selected_widget->perform_layout(m_nvg_context);
        if (Widget* parent = selected_widget->parent()) {
            parent->perform_layout(m_nvg_context);
        }
        perform_layout();
        redraw();
        return true;
    });
    spacing_box->set_min_height(20);
}

std::string GUIEditor::getWidgetTypeName(Widget *widget) {
    if (widget == canvas_win) return "Canvas";
    if (dynamic_cast<TestWindow *>(widget)) return "Window";
    if (dynamic_cast<TestWidget *>(widget)) return "Pane";
    if (dynamic_cast<TestLabel *>(widget)) return "Label";
    if (dynamic_cast<TestButton *>(widget)) return "Button";
    if (dynamic_cast<TestTextBox *>(widget)) return "Text Box";
    if (dynamic_cast<TestDropdown *>(widget)) return "Dropdown";
    if (dynamic_cast<TestCheckBox *>(widget)) return "Checkbox";
    if (dynamic_cast<TestSlider *>(widget)) return "Slider";
    if (dynamic_cast<TestColorPicker *>(widget)) return "Color Picker";
    return "Widget";
}

std::string GUIEditor::generateUniqueId(int icon) {
    switch (icon) {
        case FA_WINDOW_MAXIMIZE: return "WINDOW" + std::to_string(++window_count);
        case FA_TH: return "PANE" + std::to_string(++pane_count);
        case FA_TAG: return "LABEL" + std::to_string(++label_count);
        case FA_HAND_POINT_UP: return "BUTTON" + std::to_string(++button_count);
        case FA_KEYBOARD: return "TEXTBOX" + std::to_string(++textbox_count);
        case FA_CARET_DOWN: return "DROPDOWN" + std::to_string(++dropdown_count);
        case FA_CHECK_SQUARE: return "CHECKBOX" + std::to_string(++checkbox_count);
        case FA_SLIDERS_H: return "SLIDER" + std::to_string(++slider_count);
        case FA_PALETTE: return "COLORPICKER" + std::to_string(++colorpicker_count);
        case FA_CHART_LINE: return "GRAPH" + std::to_string(++graph_count);
        case FA_IMAGE: return "IMAGE" + std::to_string(++image_count);
        default: return "WIDGET" + std::to_string(window_count + label_count + button_count + 1);
    }
}

Widget* GUIEditor::create_widget_by_type(const std::string& type, Widget* parent) {
    if (!parent) parent = canvas_win;
    Widget* w = nullptr;
    if (type == "Window") {
        TestWindow *sub_win = new TestWindow(parent, "New Window");
        sub_win->set_size(Vector2i(200, 150));
        sub_win->set_layout(new GroupLayout());
        w = sub_win;
    } else if (type == "Pane" || type == "View" || type == "Widget") {
        TestWidget *pane = new TestWidget(parent);
        pane->set_fixed_size(Vector2i(150, 100));
        pane->set_layout(new GroupLayout());
        w = pane;
    } else if (type == "Label") {
        TestLabel *lbl = new TestLabel(parent, "Label");
        lbl->set_fixed_size(Vector2i(100, 20));
        w = lbl;
    } else if (type == "Button") {
        TestButton *btn = new TestButton(parent, "Button");
        btn->set_fixed_size(Vector2i(100, 25));
        w = btn;
    } else if (type == "Text Box" || type == "TextBox") {
        TestTextBox *tb = new TestTextBox(parent);
        tb->set_fixed_size(Vector2i(150, 25));
        tb->set_value("Text");
        w = tb;
    } else if (type == "Dropdown") {
        TestDropdown *dropdown = new TestDropdown(parent);
        dropdown->set_fixed_size(Vector2i(150, 25));
        dropdown->set_width(150);
        dropdown->set_text_color(Color(255, 255, 255, 255));
        w = dropdown;
    } else if (type == "Checkbox" || type == "CheckBox") {
        TestCheckBox *cb = new TestCheckBox(parent, "Checkbox");
        cb->set_fixed_size(Vector2i(150, 25));
        w = cb;
    } else if (type == "Slider") {
        TestSlider *sl = new TestSlider(parent);
        sl->set_fixed_size(Vector2i(150, 25));
        w = sl;
    } else if (type == "Color Picker" || type == "ColorPicker") {
        TestColorPicker *cp = new TestColorPicker(parent, Color(255, 0, 0, 255));
        cp->set_fixed_size(Vector2i(100, 100));
        w = cp;
    }
    return w;
}

void GUIEditor::clear_canvas() {
    selected_widget = nullptr;
    if (!canvas_win) return;
    // Remove all children of canvas_win
    while (canvas_win->child_count() > 0) {
        canvas_win->remove_child_at(0);
    }
    // Reset the top widget (canvas) layout. The loader will re-apply
    // whatever layout (if any) the saved JSON specifies for the Canvas.
    canvas_win->set_layout(nullptr);
    update_properties();
}

// Recursive search widget tree
static bool is_child(Widget *find, Widget *search) {
	for (Widget *child : search->children()) {
		if(child == find)
			return true;
		else
			return is_child(child, search);
	}
	return false;
}

bool GUIEditor::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    m_redraw = true; // Force redraw on all mouse events
    Widget *clicked_widget = find_widget(p);

    if (Screen::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    if (!TestModeManager::getInstance()->isTestModeEnabled() && button == GLFW_MOUSE_BUTTON_1 && down) {
        // NEW: Check for resize handle hit first
        Widget* hit_handle_widget = find_widget_with_handle(p);
        if (hit_handle_widget) {
            if (hit_handle_widget != selected_widget) {
                selected_widget = hit_handle_widget;
                update_properties();
            }
            // Determine which handle was hit
            Vector2i w_pos = hit_handle_widget->absolute_position();
            Vector2i w_size = hit_handle_widget->size();
            Vector2i handle_pos[8] = {
                {w_pos.x() - 4, w_pos.y() - 4}, // 0: top-left
                {w_pos.x() + w_size.x() - 4, w_pos.y() - 4}, // 1: top-right
                {w_pos.x() - 4, w_pos.y() + w_size.y() - 4}, // 2: bottom-left
                {w_pos.x() + w_size.x() - 4, w_pos.y() + w_size.y() - 4}, // 3: bottom-right
                {w_pos.x() + (w_size.x() - 8) / 2, w_pos.y() - 4}, // 4: top-middle
                {w_pos.x() + (w_size.x() - 8) / 2, w_pos.y() + w_size.y() - 4}, // 5: bottom-middle
                {w_pos.x() - 4, w_pos.y() + (w_size.y() - 8) / 2}, // 6: left-middle
                {w_pos.x() + w_size.x() - 4, w_pos.y() + (w_size.y() - 8) / 2} // 7: right-middle
            };
            for (int i = 0; i < 8; ++i) {
                Vector2i rel = p - handle_pos[i];
                if (rel.x() >= 0 && rel.y() >= 0 && rel.x() < 8 && rel.y() < 8) {
                    resize_handle = i;
                    break;
                }
            }
            resizing = true;
            resize_start_pos = selected_widget->position();
            resize_start_size = selected_widget->size();
            drag_start = p;
            return true;
        }

        if (current_tool == FA_TRASH) {
            // Delete tool: Remove the clicked widget if valid
            if (clicked_widget && clicked_widget->window() != editor_win && clicked_widget != canvas_win) {
                Widget *parent = clicked_widget->parent();
                if (parent) {
                    // If the clicked widget is selected, clear the selection
                    if (selected_widget == clicked_widget) {
                        selected_widget = nullptr;
                    }
					set_focused(false);
					notify_widget_destroyed(clicked_widget);
                    // Increment reference to prevent deletion during removal
                    clicked_widget->inc_ref();
                    parent->remove_child(clicked_widget);
                    // Update UI
                    //parent->perform_layout(m_nvg_context);
                    //perform_layout();
                    update_properties();
                    redraw();
                    clicked_widget->dec_ref();
                    return true;
                }
            }
            return false; // No valid widget to delete
        }

        // Find the deepest container widget (TestWindow, TestWidget, or canvas_win) for placing new widgets
        Widget *target_container = canvas_win;
        Vector2i relative_pos = p - canvas_win->absolute_position();
        // Clamp position inside container
        if (relative_pos.x() < 0 || relative_pos.y() < 0)
            return false;

        for (Widget *child : canvas_win->children()) {
            if (dynamic_cast<TestWindow*>(child) || dynamic_cast<TestWidget*>(child)) {
                Vector2i child_pos = child->absolute_position();
                Vector2i child_size = child->size();
                Vector2i local_p = p - child_pos;
                if (local_p.x() >= 0 && local_p.y() >= 0 && local_p.x() < child_size.x() && local_p.y() < child_size.y()) {
                    target_container = child;
                    relative_pos = local_p;
                    break;
                }
            }
        }

        if (current_tool == FA_MOUSE_POINTER) {
            // Selection logic: Select the deepest widget, or the container if no child is hit

            if (clicked_widget && (clicked_widget==canvas_win || is_child(clicked_widget, canvas_win) )) {
                // Select the deepest widget (could be a container or child), excluding editor_win and its children
                printf("Selected widget %s\n", clicked_widget->id().c_str());
                selected_widget = clicked_widget;
                update_properties();
                dragging = true;
                drag_start = p;
                // Store the offset from the widget's top-left corner to the click position
                drag_offset = p - clicked_widget->absolute_position();
                // Store the original parent for reparenting logic
                original_parent = clicked_widget->parent();
            } else {
                // Deselect if clicking on editor_win or outside a valid widget
                selected_widget = nullptr;
                update_properties();
            }
        } else if (current_tool != 0 && current_tool != FA_TRASH) {
            // Place new widget in the target container
            Widget *new_w = nullptr;
            switch (current_tool) {
                case FA_WINDOW_MAXIMIZE: {
                    TestWindow *sub_win = new TestWindow(target_container, "New Window");
                    sub_win->set_position(relative_pos);
                    sub_win->set_size(Vector2i(200, 150));
                    sub_win->set_layout(new GroupLayout());
                    new_w = sub_win;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_TH: {
                    TestWidget *pane = new TestWidget(target_container);
                    pane->set_position(relative_pos);
                    pane->set_fixed_size(Vector2i(150, 100));
                    pane->set_layout(new GroupLayout());
                    new_w = pane;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_TAG: {
                    TestLabel *lbl = new TestLabel(target_container, "Label");
                    lbl->set_position(relative_pos);
                    lbl->set_fixed_size(Vector2i(100, 20));
                    new_w = lbl;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_HAND_POINT_UP: {
                    TestButton *btn = new TestButton(target_container, "Button");
                    btn->set_position(relative_pos);
                    btn->set_fixed_size(Vector2i(100, 25));
                    new_w = btn;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_KEYBOARD: {
                    TestTextBox *tb = new TestTextBox(target_container);
                    tb->set_position(relative_pos);
                    tb->set_fixed_size(Vector2i(150, 25));
                    tb->set_value("Text");
                    new_w = tb;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_CARET_DOWN: {
                    TestDropdown *dropdown = new TestDropdown(target_container);
                    dropdown->set_position(relative_pos);
                    dropdown->set_fixed_size(Vector2i(150, 25));
                    dropdown->set_width(150);
                    dropdown->set_text_color(Color(255, 255, 255, 255));
                    std::vector<std::string> items = {"Item 1", "Item 2"};
                    for (const auto& item : items) {
                        std::vector<Shortcut> shortcuts;
                        if (!item.empty()) {
                            shortcuts = {{0, 0}};
                        }
                        dropdown->add_item(
                            {item, item + "_item"}, 0,
                            nullptr,
                            shortcuts,
                            true
                        );
                    }
                    // Set item callbacks after adding
                    for (Widget *child : dropdown->popup()->children()) {
                        if (MenuItem *mi = dynamic_cast<MenuItem*>(child)) {
                            mi->set_callback([mi] { std::cout << "Selected item: " << mi->caption() << "\n"; });
                        }
                    }
                    dropdown->set_selected_callback([dropdown](int idx) {
                        if (auto item = dropdown->popup()->item(idx))
                            std::cout << "Dropdown callback - Selected item: " << item->caption() << "\n";
                    });
                    new_w = dropdown;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_CHECK_SQUARE: {
                    TestCheckBox *cb = new TestCheckBox(target_container, "Checkbox");
                    cb->set_position(relative_pos);
                    cb->set_fixed_size(Vector2i(150, 25));
                    new_w = cb;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_SLIDERS_H: {
                    TestSlider *sl = new TestSlider(target_container);
                    sl->set_position(relative_pos);
                    sl->set_fixed_size(Vector2i(150, 25));
                    new_w = sl;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_PALETTE: {
                    TestColorPicker *cp = new TestColorPicker(target_container, Color(255, 0, 0, 255));
                    cp->set_position(relative_pos);
                    cp->set_fixed_size(Vector2i(100, 100));
                    new_w = cp;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                default:
                    break;
            }
            if (new_w) {
                selected_widget = new_w;
                update_properties();
                perform_layout();
                redraw();
                return true;
            }
        }
    } else if (!down && dragging) {
        // Handle reparenting when the mouse button is released
        if (selected_widget && canvas_win != selected_widget) {
            // Find the container under the mouse
            Widget *new_parent = canvas_win;
            Vector2i new_pos = p - canvas_win->absolute_position() - drag_offset;
			if(selected_widget != canvas_win)
				new_pos = snap(new_pos);

            for (Widget *child : canvas_win->children()) {
                if (dynamic_cast<TestWindow*>(child) || dynamic_cast<TestWidget*>(child)) {
                    Vector2i child_pos = child->absolute_position();
                    Vector2i child_size = child->size();
                    Vector2i local_p = p - child_pos;
                    if (local_p.x() >= 0 && local_p.y() >= 0 && local_p.x() < child_size.x() && local_p.y() < child_size.y()) {
                        new_parent = child;
                        new_pos = local_p - drag_offset;
                        break;
                    }
                }
            }
            // Only reparent if the new parent is different, not editor_win, and not the widget itself
            if (new_parent != selected_widget->parent() && new_parent->window() != editor_win &&
                new_parent != selected_widget) {
                // Reparent the widget
                Widget* current_parent = selected_widget->parent();
                if (current_parent) {
                    selected_widget->inc_ref(); // Prevent widget from being deleted
                    current_parent->remove_child(selected_widget);
                    new_parent->add_child(selected_widget);
                    // Constrain position to new parent's bounds
                    Vector2i parent_size = new_parent->size();
                    Vector2i widget_size = selected_widget->size();
                    new_pos.x() = std::max(0, std::min(new_pos.x(), parent_size.x() - widget_size.x()));
                    new_pos.y() = std::max(0, std::min(new_pos.y(), parent_size.y() - widget_size.y()));
                    selected_widget->set_position(new_pos);
                    // Update layouts
                    if (original_parent) {
                        original_parent->perform_layout(m_nvg_context);
                    }
                    new_parent->perform_layout(m_nvg_context);
                    perform_layout();
                    update_properties();
                    selected_widget->dec_ref(); // Prevent widget from being deleted
                }
            } else {
                // If not reparenting, ensure the position is updated in the current parent
                Widget* current_parent = selected_widget->parent();
                if (current_parent) {
                    Vector2i parent_pos = current_parent->absolute_position();
                    new_pos = p - parent_pos - drag_offset;
					new_pos = snap(new_pos);
                    Vector2i parent_size = current_parent->size();
                    Vector2i widget_size = selected_widget->size();
                    new_pos.x() = std::max(0, std::min(new_pos.x(), parent_size.x() - widget_size.x()));
                    new_pos.y() = std::max(0, std::min(new_pos.y(), parent_size.y() - widget_size.y()));
                    selected_widget->set_position(new_pos);
                    current_parent->perform_layout(m_nvg_context);
                    perform_layout();
                    update_properties();
                }
            }
        }
        dragging = false;
        drag_offset = Vector2i(0, 0); // Reset offset
        original_parent = nullptr; // Reset original parent
		potential_parent = nullptr; // Reset potential parent
    } else if (!down && resizing) {
        // NEW: End resizing
        resizing = false;
        resize_handle = -1;
        update_properties();
    }

    return false;
}

#if 0
bool GUIEditor::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    m_redraw = true; // Force redraw on all mouse events

    if (Screen::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    if (!TestModeManager::getInstance()->isTestModeEnabled() && button == GLFW_MOUSE_BUTTON_1 && down) {
        // Find the deepest widget at the click position
        Widget *clicked_widget = find_widget(p);

        if (current_tool == FA_TRASH) {
            // Delete tool: Remove the clicked widget if valid
            if (clicked_widget && clicked_widget->window() != editor_win && clicked_widget != canvas_win) {
                Widget *parent = clicked_widget->parent();
                if (parent) {
                    // If the clicked widget is selected, clear the selection
                    if (selected_widget == clicked_widget) {
                        selected_widget = nullptr;
                    }
					set_focused(false);
					notify_widget_destroyed(clicked_widget);
                    // Increment reference to prevent deletion during removal
                    clicked_widget->inc_ref();
                    parent->remove_child(clicked_widget);
                    // Update UI
                    //parent->perform_layout(m_nvg_context);
                    //perform_layout();
                    update_properties();
                    redraw();
                    clicked_widget->dec_ref();
                    return true;
                }
            }
            return false; // No valid widget to delete
        }

        // Find the deepest container widget (TestWindow, TestWidget, or canvas_win) for placing new widgets
        Widget *target_container = canvas_win;
        Vector2i relative_pos = p - canvas_win->absolute_position();
        // Clamp position inside container
        if (relative_pos.x() < 0 || relative_pos.y() < 0)
            return false;

        for (Widget *child : canvas_win->children()) {
            if (dynamic_cast<TestWindow*>(child) || dynamic_cast<TestWidget*>(child)) {
                Vector2i child_pos = child->absolute_position();
                Vector2i child_size = child->size();
                Vector2i local_p = p - child_pos;
                if (local_p.x() >= 0 && local_p.y() >= 0 && local_p.x() < child_size.x() && local_p.y() < child_size.y()) {
                    target_container = child;
                    relative_pos = local_p;
                    break;
                }
            }
        }

        if (current_tool == FA_MOUSE_POINTER) {
            // Selection logic: Select the deepest widget, or the container if no child is hit

            if (clicked_widget && (clicked_widget==canvas_win || is_child(clicked_widget, canvas_win) )) {
                // Select the deepest widget (could be a container or child), excluding editor_win and its children
                printf("Selected widget %s\n", clicked_widget->id().c_str());
                selected_widget = clicked_widget;
                update_properties();
                dragging = true;
                drag_start = p;
                // Store the offset from the widget's top-left corner to the click position
                drag_offset = p - clicked_widget->absolute_position();
                // Store the original parent for reparenting logic
                original_parent = clicked_widget->parent();
            } else {
                // Deselect if clicking on editor_win or outside a valid widget
                selected_widget = nullptr;
                update_properties();
            }
        } else if (current_tool != 0 && current_tool != FA_TRASH) {
            // Place new widget in the target container
            Widget *new_w = nullptr;
            switch (current_tool) {
                case FA_WINDOW_MAXIMIZE: {
                    TestWindow *sub_win = new TestWindow(target_container, "New Window");
                    sub_win->set_position(relative_pos);
                    sub_win->set_size(Vector2i(200, 150));
                    sub_win->set_layout(new GroupLayout());
                    new_w = sub_win;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_TH: {
                    TestWidget *pane = new TestWidget(target_container);
                    pane->set_position(relative_pos);
                    pane->set_fixed_size(Vector2i(150, 100));
                    pane->set_layout(new GroupLayout());
                    new_w = pane;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_TAG: {
                    TestLabel *lbl = new TestLabel(target_container, "Label");
                    lbl->set_position(relative_pos);
                    lbl->set_fixed_size(Vector2i(100, 20));
                    new_w = lbl;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_HAND_POINT_UP: {
                    TestButton *btn = new TestButton(target_container, "Button");
                    btn->set_position(relative_pos);
                    btn->set_fixed_size(Vector2i(100, 25));
                    new_w = btn;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_KEYBOARD: {
                    TestTextBox *tb = new TestTextBox(target_container);
                    tb->set_position(relative_pos);
                    tb->set_fixed_size(Vector2i(150, 25));
                    tb->set_value("Text");
                    new_w = tb;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_CARET_DOWN: {
                    TestDropdown *dropdown = new TestDropdown(target_container);
                    dropdown->set_position(relative_pos);
                    dropdown->set_fixed_size(Vector2i(150, 25));
                    dropdown->set_width(150);
                    dropdown->set_text_color(Color(255, 255, 255, 255));
                    std::vector<std::string> items = {"Item 1", "Item 2"};
                    for (const auto& item : items) {
                        std::vector<Shortcut> shortcuts;
                        if (!item.empty()) {
                            shortcuts = {{0, 0}};
                        }
                        dropdown->add_item(
                            {item, item + "_item"}, 0,
                            nullptr,
                            shortcuts,
                            true
                        );
                    }
                    // Set item callbacks after adding
                    for (Widget *child : dropdown->popup()->children()) {
                        if (MenuItem *mi = dynamic_cast<MenuItem*>(child)) {
                            mi->set_callback([mi] { std::cout << "Selected item: " << mi->caption() << "\n"; });
                        }
                    }
                    dropdown->set_selected_callback([dropdown](int idx) {
                        if (auto item = dropdown->popup()->item(idx))
                            std::cout << "Dropdown callback - Selected item: " << item->caption() << "\n";
                    });
                    new_w = dropdown;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_CHECK_SQUARE: {
                    TestCheckBox *cb = new TestCheckBox(target_container, "Checkbox");
                    cb->set_position(relative_pos);
                    cb->set_fixed_size(Vector2i(150, 25));
                    new_w = cb;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_SLIDERS_H: {
                    TestSlider *sl = new TestSlider(target_container);
                    sl->set_position(relative_pos);
                    sl->set_fixed_size(Vector2i(150, 25));
                    new_w = sl;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                case FA_PALETTE: {
                    TestColorPicker *cp = new TestColorPicker(target_container, Color(255, 0, 0, 255));
                    cp->set_position(relative_pos);
                    cp->set_fixed_size(Vector2i(100, 100));
                    new_w = cp;
                    new_w->set_id(generateUniqueId(current_tool));
                }
                break;
                default:
                    break;
            }
            if (new_w) {
                selected_widget = new_w;
                update_properties();
                perform_layout();
                redraw();
                return true;
            }
        }
    } else if (!down && dragging) {
        // Handle reparenting when the mouse button is released
        if (selected_widget && canvas_win != selected_widget) {
            // Find the container under the mouse
            Widget *new_parent = canvas_win;
            Vector2i new_pos = p - canvas_win->absolute_position() - drag_offset;
			if(selected_widget != canvas_win)
				new_pos = snap(new_pos);

            for (Widget *child : canvas_win->children()) {
                if (dynamic_cast<TestWindow*>(child) || dynamic_cast<TestWidget*>(child)) {
                    Vector2i child_pos = child->absolute_position();
                    Vector2i child_size = child->size();
                    Vector2i local_p = p - child_pos;
                    if (local_p.x() >= 0 && local_p.y() >= 0 && local_p.x() < child_size.x() && local_p.y() < child_size.y()) {
                        new_parent = child;
                        new_pos = local_p - drag_offset;
                        break;
                    }
                }
            }
            // Only reparent if the new parent is different, not editor_win, and not the widget itself
            if (new_parent != selected_widget->parent() && new_parent->window() != editor_win &&
                new_parent != selected_widget) {
                // Reparent the widget
                Widget* current_parent = selected_widget->parent();
                if (current_parent) {
                    selected_widget->inc_ref(); // Prevent widget from being deleted
                    current_parent->remove_child(selected_widget);
                    new_parent->add_child(selected_widget);
                    // Constrain position to new parent's bounds
                    Vector2i parent_size = new_parent->size();
                    Vector2i widget_size = selected_widget->size();
                    new_pos.x() = std::max(0, std::min(new_pos.x(), parent_size.x() - widget_size.x()));
                    new_pos.y() = std::max(0, std::min(new_pos.y(), parent_size.y() - widget_size.y()));
                    selected_widget->set_position(new_pos);
                    // Update layouts
                    if (original_parent) {
                        original_parent->perform_layout(m_nvg_context);
                    }
                    new_parent->perform_layout(m_nvg_context);
                    perform_layout();
                    update_properties();
                    selected_widget->dec_ref(); // Prevent widget from being deleted
                }
            } else {
                // If not reparenting, ensure the position is updated in the current parent
                Widget* current_parent = selected_widget->parent();
                if (current_parent) {
                    Vector2i parent_pos = current_parent->absolute_position();
                    new_pos = p - parent_pos - drag_offset;
					new_pos = snap(new_pos);
                    Vector2i parent_size = current_parent->size();
                    Vector2i widget_size = selected_widget->size();
                    new_pos.x() = std::max(0, std::min(new_pos.x(), parent_size.x() - widget_size.x()));
                    new_pos.y() = std::max(0, std::min(new_pos.y(), parent_size.y() - widget_size.y()));
                    selected_widget->set_position(new_pos);
                    current_parent->perform_layout(m_nvg_context);
                    perform_layout();
                    update_properties();
                }
            }
        }
        dragging = false;
        drag_offset = Vector2i(0, 0); // Reset offset
        original_parent = nullptr; // Reset original parent
		potential_parent = nullptr; // Reset potential parent
    }

    return false;
}

bool GUIEditor::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (Screen::mouse_motion_event(p, rel, button, modifiers)) {
        return true;
    }

    if (dragging && !TestModeManager::getInstance()->isTestModeEnabled() &&
        (button & (1 << GLFW_MOUSE_BUTTON_1)) && selected_widget) {
        // Find the container under the mouse
        Widget *current_parent = selected_widget->parent();

        if (!current_parent) return false;
		if (canvas_win == selected_widget) {
			dragging = false;
			drag_offset = Vector2i(0, 0); // Reset offset
			original_parent = nullptr; // Reset original parent
			potential_parent = nullptr; // Reset potential parent
			return false; // Safety check
		}

        Vector2i parent_pos = current_parent->absolute_position();
        Vector2i new_pos = p - parent_pos - drag_offset;
		if(selected_widget != canvas_win)
				new_pos = snap(new_pos);

        // Constrain position to current parent's bounds
        Vector2i parent_size = current_parent->size();
        Vector2i widget_size = selected_widget->size();
        new_pos.x() = std::max(0, std::min(new_pos.x(), parent_size.x() - widget_size.x()));
        new_pos.y() = std::max(0, std::min(new_pos.y(), parent_size.y() - widget_size.y()));

        // Update position relative to current parent (reparenting happens on release)
        selected_widget->set_position(new_pos);
        drag_start = p; // Update drag_start to current mouse position

		// Find potential new parent for highlighting
        Widget *new_potential_parent = canvas_win;
        for (Widget *child : canvas_win->children()) {
            if (dynamic_cast<TestWindow*>(child) || dynamic_cast<TestWidget*>(child)) {
                Vector2i child_pos = child->absolute_position();
                Vector2i child_size = child->size();
                Vector2i local_p = p - child_pos;
                if (local_p.x() >= 0 && local_p.y() >= 0 && local_p.x() < child_size.x() && local_p.y() < child_size.y()) {
                    new_potential_parent = child;
                    break;
                }
            }
        }
        // Only set potential parent if it's a valid reparenting target
        if (new_potential_parent != selected_widget->parent() && new_potential_parent->window() != editor_win &&
            new_potential_parent != selected_widget) {
            if (potential_parent != new_potential_parent) {
                potential_parent = new_potential_parent;
                redraw();
            }
        } else if (potential_parent != nullptr) {
            potential_parent = nullptr;
            redraw();
        }

        // Update parent layout if applicable
        current_parent->perform_layout(m_nvg_context);
        perform_layout();
        update_properties();
        return true;
    }
    return false;
}
#endif // OLD

/* Updated: mouse_motion_event - Changed set_size(new_size) to set_fixed_size(new_size) in resizing logic to match manual fixed_size behavior for leaf widgets like TextBox and Label. */
bool GUIEditor::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (Screen::mouse_motion_event(p, rel, button, modifiers)) {
        return true;
    }

    if (dragging && !TestModeManager::getInstance()->isTestModeEnabled() &&
        (button & (1 << GLFW_MOUSE_BUTTON_1)) && selected_widget) {
        // Find the container under the mouse
        Widget *current_parent = selected_widget->parent();

        if (!current_parent) return false;
		if (canvas_win == selected_widget) {
			dragging = false;
			drag_offset = Vector2i(0, 0); // Reset offset
			original_parent = nullptr; // Reset original parent
			potential_parent = nullptr; // Reset potential parent
			return false; // Safety check
		}

        Vector2i parent_pos = current_parent->absolute_position();
        Vector2i new_pos = p - parent_pos - drag_offset;
		if(selected_widget != canvas_win)
				new_pos = snap(new_pos);

        // Constrain position to current parent's bounds
        Vector2i parent_size = current_parent->size();
        Vector2i widget_size = selected_widget->size();
        new_pos.x() = std::max(0, std::min(new_pos.x(), parent_size.x() - widget_size.x()));
        new_pos.y() = std::max(0, std::min(new_pos.y(), parent_size.y() - widget_size.y()));

        // Update position relative to current parent (reparenting happens on release)
        selected_widget->set_position(new_pos);
        drag_start = p; // Update drag_start to current mouse position

		// Find potential new parent for highlighting
        Widget *new_potential_parent = canvas_win;
        for (Widget *child : canvas_win->children()) {
            if (dynamic_cast<TestWindow*>(child) || dynamic_cast<TestWidget*>(child)) {
                Vector2i child_pos = child->absolute_position();
                Vector2i child_size = child->size();
                Vector2i local_p = p - child_pos;
                if (local_p.x() >= 0 && local_p.y() >= 0 && local_p.x() < child_size.x() && local_p.y() < child_size.y()) {
                    new_potential_parent = child;
                    break;
                }
            }
        }
        // Only set potential parent if it's a valid reparenting target
        if (new_potential_parent != selected_widget->parent() && new_potential_parent->window() != editor_win &&
            new_potential_parent != selected_widget) {
            if (potential_parent != new_potential_parent) {
                potential_parent = new_potential_parent;
                redraw();
            }
        } else if (potential_parent != nullptr) {
            potential_parent = nullptr;
            redraw();
        }

        // Update parent layout if applicable
        current_parent->perform_layout(m_nvg_context);
        perform_layout();
        update_properties();
        return true;
    } else if (resizing && !TestModeManager::getInstance()->isTestModeEnabled() && selected_widget) {
        // Handle resizing based on handle type
        Widget* parent = selected_widget->parent();
        if (!parent) return false;

        Vector2i parent_abs_pos = parent->absolute_position();
        Vector2i start_mouse_rel = drag_start - parent_abs_pos;
        Vector2i curr_mouse_rel = p - parent_abs_pos;
        Vector2i delta = curr_mouse_rel - start_mouse_rel;

        Vector2i new_pos = resize_start_pos;
        Vector2i new_size = resize_start_size;

        switch (resize_handle) {
            case 0: // top-left
                new_pos.x() += delta.x();
                new_pos.y() += delta.y();
                new_size.x() -= delta.x();
                new_size.y() -= delta.y();
                break;
            case 1: // top-right
                new_pos.y() += delta.y();
                new_size.x() += delta.x();
                new_size.y() -= delta.y();
                break;
            case 2: // bottom-left
                new_pos.x() += delta.x();
                new_size.x() -= delta.x();
                new_size.y() += delta.y();
                break;
            case 3: // bottom-right
                new_size.x() += delta.x();
                new_size.y() += delta.y();
                break;
            case 4: // top-middle
                new_pos.y() += delta.y();
                new_size.y() -= delta.y();
                break;
            case 5: // bottom-middle
                new_size.y() += delta.y();
                break;
            case 6: // left-middle
                new_pos.x() += delta.x();
                new_size.x() -= delta.x();
                break;
            case 7: // right-middle
                new_size.x() += delta.x();
                break;
        }

        // Clamp size to minimum
        new_size.x() = std::max(20, new_size.x());
        new_size.y() = std::max(20, new_size.y());

        // Clamp position to parent bounds
        Vector2i psize = parent->size();
        new_pos.x() = std::max(0, std::min(new_pos.x(), static_cast<int>(psize.x() - new_size.x())));
        new_pos.y() = std::max(0, std::min(new_pos.y(), static_cast<int>(psize.y() - new_size.y())));

        // Snap position
        new_pos = snap(new_pos);

        selected_widget->set_position(new_pos);
        selected_widget->set_fixed_size(new_size);

        parent->perform_layout(m_nvg_context);
        perform_layout();
        redraw();
        return true;
    }
    return false;
}

bool GUIEditor::mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) {
    if (!TestModeManager::getInstance()->isTestModeEnabled() &&
        dragging && selected_widget && (button & (1 << GLFW_MOUSE_BUTTON_1))) {

		if(selected_widget == canvas_win) {
			// Disable dragging the canvas - it causes weird issues
			return false;
		}
        // Use current parent for position updates during drag
        Widget *current_parent = selected_widget->parent();
        if (!current_parent) return false;
        Vector2i parent_pos = current_parent->absolute_position();
        Vector2i new_pos = p - parent_pos - drag_offset;

        // Constrain position to current parent's bounds
        Vector2i parent_size = current_parent->size();
        Vector2i widget_size = selected_widget->size();
        new_pos.x() = std::max(0, std::min(new_pos.x(), parent_size.x() - widget_size.x()));
        new_pos.y() = std::max(0, std::min(new_pos.y(), parent_size.y() - widget_size.y()));

        // Update position relative to current parent (reparenting happens on release)
        selected_widget->set_position(new_pos);
        drag_start = p; // Update drag_start to current mouse position

        // Update parent layout if applicable
        current_parent->perform_layout(m_nvg_context);
        perform_layout();
        update_properties();
        return true;
    }
    return false;
}

bool GUIEditor::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboard_event(key, scancode, action, modifiers))
        return true;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        set_visible(false);
        return true;
    }
    return false;
}

void GUIEditor::draw(NVGcontext *ctx) {
    // Draw grid dots on canvas when snapping is enabled
    if (snap_grid_size > 0) {
        Vector2i canvas_pos = canvas_win->absolute_position();
        Vector2i canvas_size = canvas_win->size();
        nvgSave(ctx);
        nvgFillColor(ctx, Color(255, 255, 255, 255)); // Bright white dots
        for (int x = snap_grid_size; x <= canvas_size.x(); x += snap_grid_size) {
            for (int y = snap_grid_size; y <= canvas_size.y(); y += snap_grid_size) {
                nvgBeginPath(ctx);
                nvgCircle(ctx, canvas_pos.x() + x, canvas_pos.y() + y, 1.0f);
                nvgFill(ctx);
            }
        }
        nvgRestore(ctx);
    }

    Screen::draw(ctx);
}

/* New: find_widget_with_handle - Public entry point for recursive handle detection under canvas_win. */
Widget* GUIEditor::find_widget_with_handle(const Vector2i& p) {
    return find_widget_with_handle_recursive(canvas_win, p);
}

/* New: find_widget_with_handle_recursive - Private recursive helper to find the deepest widget whose handle contains p. */
Widget* GUIEditor::find_widget_with_handle_recursive(Widget* w, const Vector2i& p) {
    if (!w || w->window() == editor_win) return nullptr;

    // Recurse children first to prefer deepest
    for (Widget* child : w->children()) {
        Widget* hit = find_widget_with_handle_recursive(child, p);
        if (hit) return hit;
    }

    // Check self handles
    Vector2i w_pos = w->absolute_position();
    Vector2i w_size = w->size();
    Vector2i handle_pos[8] = {
        {w_pos.x() - 4, w_pos.y() - 4}, // 0: top-left
        {w_pos.x() + w_size.x() - 4, w_pos.y() - 4}, // 1: top-right
        {w_pos.x() - 4, w_pos.y() + w_size.y() - 4}, // 2: bottom-left
        {w_pos.x() + w_size.x() - 4, w_pos.y() + w_size.y() - 4}, // 3: bottom-right
        {w_pos.x() + (w_size.x() - 8) / 2, w_pos.y() - 4}, // 4: top-middle
        {w_pos.x() + (w_size.x() - 8) / 2, w_pos.y() + w_size.y() - 4}, // 5: bottom-middle
        {w_pos.x() - 4, w_pos.y() + (w_size.y() - 8) / 2}, // 6: left-middle
        {w_pos.x() + w_size.x() - 4, w_pos.y() + (w_size.y() - 8) / 2} // 7: right-middle
    };
    for (int i = 0; i < 8; ++i) {
        Vector2i rel = p - handle_pos[i];
        if (rel.x() >= 0 && rel.y() >= 0 && rel.x() < 8 && rel.y() < 8) {
            return w;
        }
    }
    return nullptr;
}

// Makes background window resize with system window (screen)
bool GUIEditor::resize_event(const Vector2i &size) {
	if (editor_win) {
		editor_win->set_min_height(size.y());
		perform_layout();
	}
	Screen::resize_event(size);
	return true;
}

int main(int /* argc */, char ** /* argv */) {
        try {
            nanogui::init();

            /* scoped variables */
            {
                ref<GUIEditor> app = new GUIEditor();
                //app->dec_ref();
                app->set_visible(true);
                app->draw_all();
                nanogui::mainloop();
            }

            nanogui::shutdown();
        } catch (const std::exception &e) {
            std::string error_msg = std::string("Caught a fatal error: ") + std::string(e.what());
#if defined(_WIN32)
            MessageBoxA(nullptr, error_msg.c_str(), NULL, MB_ICONERROR | MB_OK);
#else
            std::cerr << error_msg << std::endl;
#endif
            return -1;
        } catch (...) {
            std::cerr << "Caught an unknown error!" << std::endl;
        }

        return 0;
}
