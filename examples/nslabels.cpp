
#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanogui/layout.h>
#include <GLFW/glfw3.h>
#include <iostream>

using namespace nanogui;

class LabelExampleApp : public Screen {
public:
    Window *m_rootWindow = nullptr;

    LabelExampleApp() : Screen(Vector2i(800, 600), "Label Line Breaking Demo") {
        inc_ref();
        set_theme_mode(ThemeMode::Light);

        Window *window = new Window(this, "", true);
        m_rootWindow = window;

        window->set_position(Vector2i(0, 0));
        window->set_size(Vector2i(this->width(), this->height()));
        window->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

        ScrollPanel *scrollPanel = new ScrollPanel(window);
        scrollPanel->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Maximum, 0, 0));
		scrollPanel->set_grow_parent(true);

        // Main content container with vertical FlexLayout
        Widget *contentContainer = new Widget(scrollPanel);
        FlexLayout *mainLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::Stretch, 10, 10);
        contentContainer->set_layout(mainLayout);
		contentContainer->set_grow_parent(true);

        // Large header
        Label *header = new Label(contentContainer, "🐺💺💆🐡🐛", "emoji", 16);
        mainLayout->set_flex_item(header, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        add_spacer(contentContainer, mainLayout);

        // Demo line-break modes
        std::string longText = "This is a very long text that will demonstrate different line breaking modes in NanoGUI labels. It contains multiple words and should show how each mode handles text overflow.";

        create_label_section(contentContainer, mainLayout, "Word Wrapping:", longText, Label::LineBreakMode::LineBreakByWordWrapping);
        create_label_section(contentContainer, mainLayout, "Character Wrapping:", longText, Label::LineBreakMode::LineBreakByCharWrapping);
        create_label_section(contentContainer, mainLayout, "Clipping:", longText, Label::LineBreakMode::LineBreakByClipping);
        create_label_section(contentContainer, mainLayout, "Truncating Tail:", longText, Label::LineBreakMode::LineBreakByTruncatingTail);
        create_label_section(contentContainer, mainLayout, "Truncating Head:", longText, Label::LineBreakMode::LineBreakByTruncatingHead);
        create_label_section(contentContainer, mainLayout, "Truncating Middle:", longText, Label::LineBreakMode::LineBreakByTruncatingMiddle);

        add_spacer(contentContainer, mainLayout);

        // Short Text header
        Label *shortHeader = new Label(contentContainer, "🐺💺💆🐡🐛🕞🍰🐽🍣🍫🔂🏆🍩", "emoji", 16);
		//shortHeader->set_selectable(true);
        mainLayout->set_flex_item(shortHeader, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        create_label_section(contentContainer, mainLayout, "Short text (Word Wrap):", "Short text example", Label::LineBreakMode::LineBreakByWordWrapping);
        create_label_section(contentContainer, mainLayout, "Short text (Truncate Tail):", "Short text example", Label::LineBreakMode::LineBreakByTruncatingTail);

        add_spacer(contentContainer, mainLayout);

        // Width header
        Label *widthHeader = new Label(contentContainer, "Different Widths:", "sans-bold", 14);
        mainLayout->set_flex_item(widthHeader, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Width comparison with same text
        create_width_examples(contentContainer, mainLayout);

        // Bottom spacing
        add_spacer(contentContainer, mainLayout);
        add_spacer(contentContainer, mainLayout);

        perform_layout();
    }

private:
    void create_label_section(
        Widget *parent, FlexLayout *parentLayout,
        const std::string &title, const std::string &text, Label::LineBreakMode mode)
    {
        // Create a horizontal container for this section
        Widget *sectionContainer = new Widget(parent);
        FlexLayout *sectionLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 0, 15);
        sectionContainer->set_layout(sectionLayout);

        // Set this section to not grow/shrink in the main vertical layout
        parentLayout->set_flex_item(sectionContainer, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Title label - fixed width
        Label *titleLabel = new Label(sectionContainer, title, "sans-bold", 13);
        titleLabel->set_color(Color(0.6f, 0.6f, 0.6f, 1.0f));
        //titleLabel->set_fixed_width(160); // Fixed width for consistency
        sectionLayout->set_flex_item(titleLabel, FlexLayout::FlexItem(0.0f, 0.0f, 160));

        // Content label - flexible width
        Label *contentLabel = new Label(sectionContainer, text, "sans", 24);
        contentLabel->set_line_break_mode(mode);
        contentLabel->set_color(Color(0.1f, 0.1f, 0.1f, 1.0f));
        contentLabel->set_min_height(80);
        contentLabel->set_selectable( true );
        contentLabel->set_min_width(200);
        sectionLayout->set_flex_item(contentLabel, FlexLayout::FlexItem(1.0f, 1.0f, 200)); // Allow growing
    }

    void create_width_examples(Widget *parent, FlexLayout *parentLayout)
    {
        std::string sampleText = "This text will be shown at different widths to demonstrate responsive behavior.";

        // Create horizontal container for width examples
        Widget *examplesContainer = new Widget(parent);
        FlexLayout *examplesLayout = new FlexLayout(FlexDirection::Row, JustifyContent::SpaceEvenly, AlignItems::FlexStart, 0, 20);
        examplesContainer->set_layout(examplesLayout);

        // Set this container to not grow in the main vertical layout
        parentLayout->set_flex_item(examplesContainer, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        int widths[] = {120, 150, 180};
        for (int i = 0; i < 3; ++i) {
            Widget *column = new Widget(examplesContainer);
            FlexLayout *columnLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::FlexStart, 5, 5);
            column->set_layout(columnLayout);

            // Each column should not grow/shrink, maintain fixed width
            examplesLayout->set_flex_item(column, FlexLayout::FlexItem(0.0f, 0.0f, widths[i] + 10));

            // Width label
            Label *widthLabel = new Label(column, std::string("Width: ") + std::to_string(widths[i]), "sans-bold", 11);
            columnLayout->set_flex_item(widthLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));

            // Sample text label
            Label *sampleLabel = new Label(column, sampleText);
			//sampleLabel->set_selectable( true );
            sampleLabel->set_line_break_mode(Label::LineBreakMode::LineBreakByWordWrapping);
            sampleLabel->set_min_width(widths[i]);
            sampleLabel->set_min_height(150);
            sampleLabel->set_color(Color(0.2f, 0.2f, 0.2f, 1.0f));
            columnLayout->set_flex_item(sampleLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        }
    }

    void add_spacer(Widget *parent, FlexLayout *parentLayout) {
        Widget *spacer = new Widget(parent);
        spacer->set_min_height(15);
        parentLayout->set_flex_item(spacer, FlexLayout::FlexItem(0.0f, 0.0f, 15));
    }

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            set_visible(false);
            return true;
        }
        if (key == GLFW_KEY_T && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            set_theme_mode(theme_mode() == ThemeMode::Light
                               ? ThemeMode::Dark : ThemeMode::Light);
            perform_layout();
            return true;
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, m_size.x(), m_size.y());
        nvgFillColor(ctx, nvgRGBA(0, 64, 100, 255));
        nvgFill(ctx);
        nvgRestore(ctx);
        Screen::draw(ctx);
    }

    virtual bool resize_event(const Vector2i &size) override {
        if (m_rootWindow) {
            m_rootWindow->set_size(size);
            perform_layout();
        }
        Screen::resize_event(size);
        return true;
    }
};

int main() {
    try {
        nanogui::init();

        {
            ref<LabelExampleApp> app = new LabelExampleApp();
            app->dec_ref();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop(1/60.f * 1000);
        }

        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::string error_msg = std::string("Caught a fatal error: ") + std::string(e.what());
        std::cerr << error_msg << std::endl;
        return -1;
    }

    return 0;
}
