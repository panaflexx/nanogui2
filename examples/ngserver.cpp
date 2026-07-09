#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/window.h>
#include <nanogui/layout.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/widget.h>
#include <nanogui/textbox.h>
#include <nanogui/checkbox.h>
#include <nanogui/slider.h>
#include <nanogui/colorpicker.h>
#include <nanogui/graph.h>
#include <nanogui/imagepanel.h>
#include <nanogui/progressbar.h>
#include <nanogui/textarea.h>
#include <nanogui/split.h>
#include <iostream>
#include <string>
#include <functional>

// Include the dict.h for JSON parsing
#include "dict.h"
#include "guieditor_json.h"

using namespace nanogui;

// Event structure for runtime communication
struct GuiEvent {
    std::string id;
    std::string type;
    std::string data;

    GuiEvent(const std::string& id, const std::string& type, const std::string& data = "")
        : id(id), type(type), data(data) {}
};

// Global event callback function - runtime can set this
std::function<void(const GuiEvent&)> g_eventCallback = nullptr;

// Helper function to send events to runtime
void sendEventToRuntime(const GuiEvent& event) {
    if (g_eventCallback) {
        g_eventCallback(event);
    } else {
        // Default behavior - print to console
		// TODO: Send events back to runtime
		/*
        std::cout << "Event: id='" << event.id << "', type='" << event.type << "'";
        if (!event.data.empty()) {
            std::cout << ", data='" << event.data << "'";
        }
        std::cout << std::endl;
		*/
    }
}

// Custom Widget class with event support
class EventWidget : public Widget {
public:
    EventWidget(Widget* parent, const std::string& id) : Widget(parent), m_id(id) {}

    virtual bool mouse_enter_event(const Vector2i& p, bool enter) override {
        if (enter) {
            sendEventToRuntime(GuiEvent(m_id, "mouse_enter"));
        } else {
            sendEventToRuntime(GuiEvent(m_id, "mouse_leave"));
        }
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override {
        std::string eventType = down ? "mouse_down" : "mouse_up";
        std::string data = "button=" + std::to_string(button) + ",x=" + std::to_string(p.x()) + ",y=" + std::to_string(p.y());
        sendEventToRuntime(GuiEvent(m_id, eventType, data));
        return Widget::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override {
        // Only send motion events if a button is pressed (to avoid spam)
        if (button != 0) {
            std::string data = "x=" + std::to_string(p.x()) + ",y=" + std::to_string(p.y()) +
                             ",dx=" + std::to_string(rel.x()) + ",dy=" + std::to_string(rel.y());
            sendEventToRuntime(GuiEvent(m_id, "mouse_drag", data));
        }
        return Widget::mouse_motion_event(p, rel, button, modifiers);
    }

    const std::string& id() const { return m_id; }

protected:
    std::string m_id;
};

// Custom Button class with event support
class EventButton : public Button {
public:
    EventButton(Widget* parent, const std::string& caption, const std::string& id)
        : Button(parent, caption), m_id(id) {}

    virtual bool mouse_enter_event(const Vector2i& p, bool enter) override {
        if (enter) {
            sendEventToRuntime(GuiEvent(m_id, "mouse_enter"));
        } else {
            sendEventToRuntime(GuiEvent(m_id, "mouse_leave"));
        }
        return Button::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override {
        // Send our custom button click event
        if (down && button == GLFW_MOUSE_BUTTON_1) {
            sendEventToRuntime(GuiEvent(m_id, "button_click", caption()));
        }

        std::string eventType = down ? "mouse_down" : "mouse_up";
        std::string data = "button=" + std::to_string(button) + ",x=" + std::to_string(p.x()) + ",y=" + std::to_string(p.y());
        sendEventToRuntime(GuiEvent(m_id, eventType, data));

        return Button::mouse_button_event(p, button, down, modifiers);
    }

    const std::string& id() const { return m_id; }

protected:
    std::string m_id;
};

// Custom Window class with event support
class EventWindow : public Window {
public:
    EventWindow(Widget* parent, const std::string& title, const std::string& id, bool resizable = true)
        : Window(parent, title, resizable), m_id(id) {}

    virtual bool mouse_enter_event(const Vector2i& p, bool enter) override {
        if (enter) {
            sendEventToRuntime(GuiEvent(m_id, "mouse_enter"));
        } else {
            sendEventToRuntime(GuiEvent(m_id, "mouse_leave"));
        }
        return Window::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override {
        std::string eventType = down ? "mouse_down" : "mouse_up";
        std::string data = "button=" + std::to_string(button) + ",x=" + std::to_string(p.x()) + ",y=" + std::to_string(p.y());
        sendEventToRuntime(GuiEvent(m_id, eventType, data));
        return Window::mouse_button_event(p, button, down, modifiers);
    }

    virtual bool mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override {
        std::string data = "x=" + std::to_string(p.x()) + ",y=" + std::to_string(p.y()) +
                         ",dx=" + std::to_string(rel.x()) + ",dy=" + std::to_string(rel.y());
        sendEventToRuntime(GuiEvent(m_id, "window_drag", data));
        return Window::mouse_drag_event(p, rel, button, modifiers);
    }

	virtual bool mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override {
		return Window::mouse_motion_event( p, rel, button, modifiers );
	}

    const std::string& id() const { return m_id; }

protected:
    std::string m_id;
};

// Custom Label class with event support
class EventLabel : public Label {
public:
    EventLabel(Widget* parent, const std::string& caption, const std::string& id)
        : Label(parent, caption), m_id(id) {}

    virtual bool mouse_enter_event(const Vector2i& p, bool enter) override {
        if (enter) {
            sendEventToRuntime(GuiEvent(m_id, "mouse_enter"));
        } else {
            sendEventToRuntime(GuiEvent(m_id, "mouse_leave"));
        }
        return Label::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override {
        if (down && button == GLFW_MOUSE_BUTTON_1) {
            sendEventToRuntime(GuiEvent(m_id, "label_click", caption()));
        }

        std::string eventType = down ? "mouse_down" : "mouse_up";
        std::string data = "button=" + std::to_string(button) + ",x=" + std::to_string(p.x()) + ",y=" + std::to_string(p.y());
        sendEventToRuntime(GuiEvent(m_id, eventType, data));

        return Label::mouse_button_event(p, button, down, modifiers);
    }

    const std::string& id() const { return m_id; }

protected:
    std::string m_id;
};

class JsonGuiApplication : public Screen {
private:
	EventWindow* m_rootWindow = nullptr;
	float m_scale = 1.0f;

	static Vector2i scaleVec(const Vector2i& v, float s) {
		return Vector2i((int)std::round(v.x() * s), (int)std::round(v.y() * s));
	}

	void applyScaleRecursive(Widget* w) {
		if (!w || m_scale == 1.0f) return;
		if (w != m_rootWindow) {
			if (w->fixed_size().x() != 0 || w->fixed_size().y() != 0)
				w->set_fixed_size(scaleVec(w->fixed_size(), m_scale));
			w->set_size(scaleVec(w->size(), m_scale));
			w->set_position(scaleVec(w->position(), m_scale));
			// font_size() returns the effective size (theme default if not set
			// explicitly), so we always materialize a scaled value.
			w->set_font_size((int)std::round(w->font_size() * m_scale));
		}
		for (Widget* c : w->children())
			applyScaleRecursive(c);
	}

public:
	void setScale(float s) { m_scale = s; }
private:

    // Build a WidgetFactory that creates Event* widgets from JSON nodes
    // and applies ngserver-specific behavior (rootWindow sizing, default
    // layouts, etc.). Common properties (id, position, size, caption,
    // value, ...) are applied by guieditor_json after the factory returns.
    guieditor_json::WidgetFactory makeFactory() {
        return [this](const std::string& type, Widget* parent,
                      DictValue* json) -> Widget* {
            std::string id;
            DictValue* idVal = dict_object_get(json, "id");
            if (idVal && idVal->type == DICT_STRING && idVal->string_value)
                id = idVal->string_value;
            if (id.empty()) {
                std::cerr << "Missing/empty 'id' in widget of type '" << type << "'\n";
                return nullptr;
            }

            Widget* widget = nullptr;
            if (type == "Window") {
                std::string title;
                DictValue* v = dict_object_get(json, "title");
                if (v && v->type == DICT_STRING) title = v->string_value;

                bool resizable = false;
                v = dict_object_get(json, "resizable");
                if (v && v->type == DICT_BOOL) resizable = v->bool_value != 0;

                EventWindow* window = new EventWindow(this, title, id, resizable);
                widget = window;

                DictValue* rootVal = dict_object_get(json, "rootWindow");
                bool isRoot = rootVal && rootVal->type == DICT_BOOL && rootVal->bool_value;
                if (isRoot) {
                    window->set_size(this->size());
                    m_rootWindow = window;
                } else {
                    DictValue* wv = dict_object_get(json, "width");
                    DictValue* hv = dict_object_get(json, "height");
                    if (wv && hv) {
                        int w = 400, h = 300;
                        if (wv->type == DICT_NUMBER) w = (int)wv->number_value;
                        else if (wv->type == DICT_INT64) w = (int)wv->int64_value;
                        if (hv->type == DICT_NUMBER) h = (int)hv->number_value;
                        else if (hv->type == DICT_INT64) h = (int)hv->int64_value;
                        window->set_fixed_size(Vector2i(w, h));
                    }
                }
                // Default layout if not specified (loader sets it if present)
                if (!dict_object_get(json, "layout"))
                    window->set_layout(new GroupLayout());
            } else if (type == "View" || type == "Widget" || type == "Pane") {
                widget = new EventWidget(parent, id);
                if (!dict_object_get(json, "layout"))
                    widget->set_layout(new GroupLayout());
            } else if (type == "Button") {
                std::string label = "Button";
                DictValue* v = dict_object_get(json, "label");
                if (!v) v = dict_object_get(json, "caption");
                if (v && v->type == DICT_STRING) label = v->string_value;
                widget = new EventButton(parent, label, id);
            } else if (type == "Label") {
                std::string text = "Label";
                DictValue* v = dict_object_get(json, "text");
                if (!v) v = dict_object_get(json, "caption");
                if (v && v->type == DICT_STRING) text = v->string_value;
                widget = new EventLabel(parent, text, id);
            } else if (type == "Text Box" || type == "TextBox") {
                TextBox* tb = new TextBox(parent);
                widget = tb;
            } else if (type == "Checkbox" || type == "CheckBox") {
                std::string caption;
                DictValue* v = dict_object_get(json, "caption");
                if (v && v->type == DICT_STRING) caption = v->string_value;
                widget = new CheckBox(parent, caption);
            } else if (type == "Slider") {
                widget = new Slider(parent);
            } else if (type == "Color Picker" || type == "ColorPicker") {
                widget = new ColorPicker(parent);
            } else if (type == "Split") {
                // Children (the two panes) are added by the JSON loader.
                // Orientation and drag_position are applied by guieditor_json.
                widget = new Split(parent, Split::Orientation::Horizontal);
            } else if (type == "Graph") {
                std::string caption;
                DictValue* v = dict_object_get(json, "caption");
                if (v && v->type == DICT_STRING) caption = v->string_value;
                widget = new Graph(parent, caption);
            } else if (type == "Image Panel" || type == "ImagePanel") {
                widget = new ImagePanel(parent);
            } else if (type == "Progress Bar" || type == "ProgressBar") {
                auto* pb = new ProgressBar(parent);
                DictValue* v = dict_object_get(json, "value");
                if (v) {
                    double val = 0.0;
                    if (v->type == DICT_NUMBER)      val = v->number_value;
                    else if (v->type == DICT_INT64)  val = (double)v->int64_value;
                    pb->set_value((float)val);
                }
                widget = pb;
            } else if (type == "Text Area" || type == "TextArea") {
                widget = new TextArea(parent);
            } else {
                std::cerr << "Warning: Unknown widget type '" << type
                          << "', creating generic EventWidget\n";
                widget = new EventWidget(parent, id);
            }
            return widget;
        };
    }

    // Extract and validate ID from JSON object
    std::string extractId(DictValue* jsonObj) {
        DictValue* idVal = dict_object_get(jsonObj, "id");
        if (!idVal || idVal->type != DICT_STRING) {
            throw std::runtime_error("Missing mandatory 'id' field in widget definition");
        }

        std::string id = idVal->string_value;
        if (id.empty()) {
            throw std::runtime_error("Widget 'id' field cannot be empty");
        }

        return id;
    }

    // Widget factory to create widgets based on JSON type
    Widget* createWidgetFromJson(DictValue* jsonObj, Widget* parent) {
        if (!jsonObj || jsonObj->type != DICT_OBJECT) return nullptr;

        // Extract mandatory ID first
        std::string id = extractId(jsonObj);

        // Get the type
        DictValue* typeVal = dict_object_get(jsonObj, "type");
        if (!typeVal || typeVal->type != DICT_STRING) {
            throw std::runtime_error("Missing 'type' field for widget with id '" + id + "'");
        }

        std::string widgetType = typeVal->string_value;
        Widget* widget = nullptr;

		if (widgetType == "Window") {
			// Get title
			std::string title = "";
			DictValue* titleVal = dict_object_get(jsonObj, "title");
			if (titleVal && titleVal->type == DICT_STRING) {
				title = titleVal->string_value;
			}

			bool resizable = false;  // Default to false
			DictValue* resizableVal = dict_object_get(jsonObj, "resizable");
			if (resizableVal && resizableVal->type == DICT_BOOL) {
				resizable = resizableVal->bool_value != 0;
			}

			EventWindow* window = new EventWindow(this, title, id, resizable);
			widget = window;

			// Check for "rootWindow" key
			DictValue* rootWindowVal = dict_object_get(jsonObj, "rootWindow");
			bool isRootWindow = rootWindowVal && rootWindowVal->type == DICT_BOOL && rootWindowVal->bool_value;

			if (isRootWindow) {
				// Set window size to match Screen size exactly
				Vector2i screenSize = this->size();
				window->set_size(screenSize);
				m_rootWindow = window;
			} else {
				// Set dimensions if specified
				DictValue* widthVal = dict_object_get(jsonObj, "width");
				DictValue* heightVal = dict_object_get(jsonObj, "height");
				if (widthVal && heightVal) {
					int width = 400, height = 300;
					if (widthVal->type == DICT_NUMBER) width = (int)widthVal->number_value;
					if (widthVal->type == DICT_INT64) width = (int)widthVal->int64_value;
					if (heightVal->type == DICT_NUMBER) height = (int)heightVal->number_value;
					if (heightVal->type == DICT_INT64) height = (int)heightVal->int64_value;

					window->set_fixed_size(Vector2i(width, height));
				}
			}

			// Layout setup here stays the same...
			DictValue* layoutVal = dict_object_get(jsonObj, "layout");
			if (layoutVal && layoutVal->type == DICT_STRING) {
				std::string layoutType = layoutVal->string_value;

				if (layoutType == "GroupLayout") {
					window->set_layout(new GroupLayout());
				} else if (layoutType == "VBoxLayout") {
					window->set_layout(new BoxLayout(Orientation::Vertical));
				} else if (layoutType == "HBoxLayout") {
					window->set_layout(new BoxLayout(Orientation::Horizontal));
				} else if (layoutType == "default") {
					window->set_layout(new GroupLayout());
				} else {
					std::cout << "Warning: Unknown layout type '" << layoutType << "', using GroupLayout" << std::endl;
					window->set_layout(new GroupLayout());
				}
			} else {
				window->set_layout(new GroupLayout());
			}
		} else if (widgetType == "View") {
            widget = new EventWidget(parent, id);

            // Set layout if specified
            DictValue* layoutVal = dict_object_get(jsonObj, "layout");
            if (layoutVal && layoutVal->type == DICT_STRING) {
                std::string layoutType = layoutVal->string_value;

                if (layoutType == "GroupLayout") {
                    widget->set_layout(new GroupLayout());
                } else if (layoutType == "VBoxLayout") {
                    widget->set_layout(new BoxLayout(Orientation::Vertical));
                } else if (layoutType == "HBoxLayout") {
                    widget->set_layout(new BoxLayout(Orientation::Horizontal));
                } else if (layoutType == "default") {
                    widget->set_layout(new GroupLayout());
                } else {
                    // Unknown layout type, use default
                    std::cout << "Warning: Unknown layout type '" << layoutType << "', using GroupLayout" << std::endl;
                    widget->set_layout(new GroupLayout());
                }
            } else {
                // No layout specified, use default
                widget->set_layout(new GroupLayout());
            }

        } else if (widgetType == "Button") {
            std::string label = "Button";
            DictValue* labelVal = dict_object_get(jsonObj, "label");
            if (labelVal && labelVal->type == DICT_STRING) {
                label = labelVal->string_value;
            }

            widget = new EventButton(parent, label, id);

        } else if (widgetType == "Label") {
            std::string text = "Label";
            DictValue* textVal = dict_object_get(jsonObj, "text");
            if (textVal && textVal->type == DICT_STRING) {
                text = textVal->string_value;
            }

            widget = new EventLabel(parent, text, id);

        } else if (widgetType == "Split") {
            widget = new Split(parent, Split::Orientation::Horizontal);
        } else if (widgetType == "Graph") {
            std::string caption;
            DictValue* v = dict_object_get(jsonObj, "caption");
            if (v && v->type == DICT_STRING) caption = v->string_value;
            widget = new Graph(parent, caption);
        } else if (widgetType == "Image Panel" || widgetType == "ImagePanel") {
            widget = new ImagePanel(parent);
        } else if (widgetType == "Progress Bar" || widgetType == "ProgressBar") {
            auto* pb = new ProgressBar(parent);
            DictValue* v = dict_object_get(jsonObj, "value");
            if (v) {
                double val = 0.0;
                if (v->type == DICT_NUMBER)     val = v->number_value;
                else if (v->type == DICT_INT64) val = (double)v->int64_value;
                pb->set_value((float)val);
            }
            widget = pb;
        } else if (widgetType == "Text Area" || widgetType == "TextArea") {
            widget = new TextArea(parent);
        } else {
            // Unknown widget type, create generic widget
            std::cout << "Warning: Unknown widget type '" << widgetType << "', creating generic Widget" << std::endl;
            widget = new EventWidget(parent, id);
        }

        return widget;
    }

    // Recursively build widget hierarchy from JSON
    void buildWidgetHierarchy(DictValue* jsonObj, Widget* parent) {
        if (!jsonObj || jsonObj->type != DICT_OBJECT) return;

        // Create the widget
        Widget* widget = createWidgetFromJson(jsonObj, parent);
        if (!widget) return;

        // Process children if they exist
        DictValue* childrenVal = dict_object_get(jsonObj, "children");
        if (childrenVal && childrenVal->type == DICT_ARRAY) {
            for (size_t i = 0; i < childrenVal->array_value.length; i++) {
                DictValue* child = childrenVal->array_value.items[i];
                buildWidgetHierarchy(child, widget);
            }
        }
    }

public:
    JsonGuiApplication() : Screen(Vector2i(800, 600), "JSON GUI Application") {
        inc_ref();

        // JSON string with mandatory IDs
        const char* jsonString = R"({
          "id": "main_window",
          "type": "Window",
          "title": "Hello World App",
          "width": 400,
          "height": 300,
          "children": [
            {
              "id": "main_container",
              "type": "View",
              "layout": "VBoxLayout",
              "children": [
                {
                  "id": "hello_button",
                  "type": "Button",
                  "label": "Hello World"
                },
                {
                  "id": "goodbye_button",
                  "type": "Button",
                  "label": "Goodbye World"
                },
                {
                  "id": "info_label",
                  "type": "Label",
                  "text": "Hover over or click the buttons above"
                }
              ]
            }
          ]
        })";

        if (guieditor_json::load_layout_from_string(this, jsonString, makeFactory())) {
            applyScaleRecursive(this);
            perform_layout();
        } else
            std::cerr << "Error building GUI from embedded JSON" << std::endl;
    }

    // Alternative constructor that reads from file
    JsonGuiApplication(const std::string& jsonFilePath, float scale = 1.0f)
        : Screen(Vector2i(800, 600), "JSON GUI Application") {
        inc_ref();
        m_scale = scale;
        if (guieditor_json::load_layout_into(this, jsonFilePath, makeFactory())) {
            applyScaleRecursive(this);
            perform_layout();
        } else
            std::cerr << "Error loading GUI from " << jsonFilePath << std::endl;
    }

	virtual bool resize_event(const Vector2i& size) override {
		if (m_rootWindow) {
			m_rootWindow->set_size(size);
			perform_layout();  // update layouts accordingly
		} else if (this->layout()) {
			// Top widget (the Screen itself) has a layout set from JSON;
			// re-run it so children flow with the new screen size.
			perform_layout();
		}
		Screen::resize_event(size);
		return true;
	}

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            set_visible(false);
            return true;
        }
        return false;
    }

    virtual void draw(NVGcontext* ctx) override {
        Screen::draw(ctx);
    }
};

// Runtime API functions
namespace JsonGuiRuntime {
    // Set the global event callback
    void setEventCallback(std::function<void(const GuiEvent&)> callback) {
        g_eventCallback = callback;
    }

    // Clear the event callback
    void clearEventCallback() {
        g_eventCallback = nullptr;
    }
}

// Example runtime event handler
void handleGuiEvent(const GuiEvent& event) {
	return;

    std::cout << "Runtime received event: ID='" << event.id << "', Type='" << event.type << "'";
    if (!event.data.empty()) {
        std::cout << ", Data='" << event.data << "'";
    }
    std::cout << std::endl;

    // Example: Handle specific events
    if (event.id == "hello_button" && event.type == "button_click") {
        std::cout << "Hello button was clicked! Doing something special..." << std::endl;
    } else if (event.id == "goodbye_button" && event.type == "button_click") {
        std::cout << "Goodbye button was clicked! Preparing to exit..." << std::endl;
    } else if (event.type == "mouse_enter") {
        std::cout << "Mouse entered widget: " << event.id << std::endl;
    }
}

int main(int argc, char** argv) {
    try {
        nanogui::init();

        // Set up event callback before creating the application
        JsonGuiRuntime::setEventCallback(handleGuiEvent);

        // Parse command line arguments
        std::string jsonPath;
        float scale = 1.0f;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if ((a == "--scale" || a == "-s") && i + 1 < argc) {
                scale = (float)std::atof(argv[++i]);
                if (scale <= 0.0f) scale = 1.0f;
            } else if (a == "--help" || a == "-h") {
                std::cout << "Usage: " << argv[0]
                          << " [--scale|-s FACTOR] [layout.json]\n";
                nanogui::shutdown();
                return 0;
            } else {
                jsonPath = a;
            }
        }

        {
            ref<JsonGuiApplication> app;

            if (!jsonPath.empty()) {
                app = new JsonGuiApplication(jsonPath, scale);
            } else {
                // Use embedded JSON (scaling applies in this path too)
                app = new JsonGuiApplication();
                app->setScale(scale);
            }

            app->dec_ref();
            app->draw_all();
            app->set_visible(true);
            nanogui::mainloop(1 / 60.f * 1000);
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
