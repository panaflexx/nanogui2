#include <nanogui/nanogui.h>
#include <nanogui/layout.h>
#include <nanogui/window.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/textbox.h>
#include <nanogui/checkbox.h>
#include <nanogui/theme.h>
#include <iostream>

using namespace nanogui;

class FlexLayoutTest : public Screen {
public:
    FlexLayoutTest() : Screen(Vector2i(1200, 800), "FlexLayout Test", true) {
        // Initialize theme
        Theme* theme = new Theme(m_nvg_context);
        theme->m_standard_font_size = 16;
        theme->m_button_font_size = 18;
        theme->m_text_box_font_size = 16;
        theme->m_window_corner_radius = 4;
        theme->m_button_corner_radius = 4;
        theme->m_window_fill_unfocused = Color(230, 230, 230, 230);
        theme->m_window_fill_focused = Color(245, 245, 245, 230);
        theme->m_button_gradient_top_focused = Color(64, 164, 232, 255);
        theme->m_button_gradient_bot_focused = Color(48, 140, 200, 255);
        theme->m_text_color = Color(0, 0, 0, 255);
        theme->m_border_light = Color(150, 150, 150, 255);
        theme->m_border_dark = Color(50, 50, 50, 255);
        set_theme(theme);

        // Main window with ScrollPanel
        Window* window = new Window(this, WindowConfig{
            .title = "FlexLayout Test Window",
            .position = Vector2i(50, 50),
            .size = Vector2i(1100, 700),
            .resizable = true,
            .layout = new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 10)
        });

        // ScrollPanel for scrolling content
        ScrollPanel* scrollPanel = new ScrollPanel(window);
        //scrollPanel->set_fixed_size(Vector2i(1080, 650));
		scrollPanel->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);

        // Content widget for test cases
        Widget* content = new Widget(scrollPanel);
        content->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 10));

        // Define all combinations
        std::vector<FlexDirection> directions = { FlexDirection::Row, FlexDirection::Column };
        std::vector<JustifyContent> justifies = {
            JustifyContent::FlexStart, JustifyContent::FlexEnd, JustifyContent::Center,
            JustifyContent::SpaceBetween, JustifyContent::SpaceAround, JustifyContent::SpaceEvenly
        };
        std::vector<AlignItems> aligns = {
            AlignItems::FlexStart, AlignItems::FlexEnd, AlignItems::Center,
            AlignItems::Stretch, AlignItems::Baseline
        };

        // Iterate through all combinations
        int test_index = 1;
        for (const auto& direction : directions) {
            for (const auto& justify : justifies) {
                for (const auto& align : aligns) {
                    // Create a test case container
                    Widget* testCase = new Widget(content);
                    testCase->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 5, 5));
                    testCase->set_min_width(1050);

                    // Label the test case
                    std::string config = "Test " + std::to_string(test_index++) + ": " +
                        (direction == FlexDirection::Row ? "Row" : "Column") + ", " +
                        justifyContentToString(justify) + ", " +
                        alignItemsToString(align);
                    Label* testLabel = new Label(testCase, config);
                    testLabel->set_font_size(18);
                    testLabel->set_font("sans-bold");

                    // Test with 1 to 4 widgets in both row and column sub-containers
                    for (int widget_count = 1; widget_count <= 4; ++widget_count) {
                        // Row container
                        Widget* rowContainer = new Widget(testCase);
                        rowContainer->set_layout(new FlexLayout(direction, justify, align, 5, 5));
                        rowContainer->set_min_height(80);
                        addWidgets(rowContainer, widget_count);

                        // Column container (if direction is Row, test orthogonal direction)
                        Widget* colContainer = new Widget(testCase);
                        colContainer->set_layout(new FlexLayout(
                            direction == FlexDirection::Row ? FlexDirection::Column : FlexDirection::Row,
                            justify, align, 5, 5));
                        colContainer->set_min_height(80);
                        addWidgets(colContainer, widget_count);
                    }
                }
            }
        }

        window->center();
        perform_layout(m_nvg_context);
    }

private:
    void addWidgets(Widget* container, int count) {
        // Widget types to cycle through
        std::vector<std::function<Widget*()>> widgetFactories = {
            [&]() -> Widget* {
                Label* label = new Label(container, "Label " + std::to_string(count));
                label->set_font_size(16);
                return label;
            },
            [&]() -> Widget* {
                Button* button = new Button(container, "Button " + std::to_string(count));
                button->set_callback([]() { std::cout << "Button clicked\n"; });
                return button;
            },
            [&]() -> Widget* {
                TextBox* textbox = new TextBox(container);
                textbox->set_placeholder("TextBox " + std::to_string(count));
                textbox->set_width(150);
                return textbox;
            },
            [&]() -> Widget* {
                CheckBox* checkbox = new CheckBox(container, "CheckBox " + std::to_string(count));
                checkbox->set_callback([](bool state) { std::cout << "CheckBox state: " << state << "\n"; });
                return checkbox;
            }
        };

        // Add specified number of widgets, cycling through types
        for (int i = 0; i < count; ++i) {
            Widget* widget = widgetFactories[i % widgetFactories.size()]();
            // Vary widget properties to test FlexItem
            if (i % 2 == 0) {
                widget->set_min_size(Vector2i(100, 30)); // Fixed size for some widgets
            } else {
                FlexLayout::FlexItem item;
                item.flex_grow = 1.0f;
                item.flex_shrink = 1.0f;
                dynamic_cast<FlexLayout*>(container->layout())->set_flex_item(widget, item);
            }
        }
    }

    std::string justifyContentToString(JustifyContent jc) const {
        switch (jc) {
            case JustifyContent::FlexStart: return "FlexStart";
            case JustifyContent::FlexEnd: return "FlexEnd";
            case JustifyContent::Center: return "Center";
            case JustifyContent::SpaceBetween: return "SpaceBetween";
            case JustifyContent::SpaceAround: return "SpaceAround";
            case JustifyContent::SpaceEvenly: return "SpaceEvenly";
            default: return "Unknown";
        }
    }

    std::string alignItemsToString(AlignItems ai) const {
        switch (ai) {
            case AlignItems::FlexStart: return "FlexStart";
            case AlignItems::FlexEnd: return "FlexEnd";
            case AlignItems::Center: return "Center";
            case AlignItems::Stretch: return "Stretch";
            case AlignItems::Baseline: return "Baseline";
            default: return "Unknown";
        }
    }
};

int main(int argc, char** argv) {
    try {
        nanogui::init();
        {
            nanogui::ref<FlexLayoutTest> app = new FlexLayoutTest();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop();
        }
        nanogui::shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}
