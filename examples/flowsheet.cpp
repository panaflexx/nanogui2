#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/layout.h>
#include <nanogui/textbox.h>
#include <nanogui/button.h>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace nanogui;

class ChartingFormApp : public Screen {
public:
    Window *m_rootWindow = nullptr;

    ChartingFormApp() : Screen(Vector2i(300, 450), "Patient Charting Form") {
        inc_ref();

        // Custom theme for professional medical look
		/*
        Theme *theme = m_theme;
        theme->m_window_fill_unfocused = Color(248, 249, 250, 255);
        theme->m_window_fill_focused = Color(255, 255, 255, 255);
        theme->m_text_color = Color(33, 37, 41, 255);
        theme->m_button_gradient_top_unfocused = Color(40, 167, 69, 255);  // Green for submit
        theme->m_button_gradient_bot_unfocused = Color(28, 118, 48, 255);
		*/

        // Main window
        Window *window = new Window(this, "", true);
        m_rootWindow = window;
        window->set_position(Vector2i(0, 0));
        window->set_size(this->size());
        window->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 15, 15));

        // Add title
        Label *titleLabel = new Label(window, "Add To Chart", "sans-bold", 24);
        titleLabel->set_color(Color(40, 167, 69, 255));
        titleLabel->set_min_height(30);

        // Create a scroll panel to contain the form fields
        ScrollPanel *scrollPanel = new ScrollPanel(window);

        // Create content widget with GridLayout
        Widget *formContainer = new Widget(scrollPanel);

        // Configure the grid layout - 2 columns, middle alignment, with proper spacing
        GridLayout *layout = new GridLayout(
            Orientation::Horizontal, 2,
            Alignment::Middle,
            15, 5
        );

        // Set column alignment: labels right-aligned (Maximum), inputs fill space (Fill)
        layout->set_col_alignment({
            Alignment::Maximum,  // Labels will be right-aligned
            Alignment::Fill      // Textboxes will fill available space
        });

        // Set spacing between elements
        layout->set_spacing(Orientation::Horizontal, 10);
        formContainer->set_layout(layout);

        // Add form fields using the grid layout approach
        TextBox *activityBox = add_form_field(formContainer, "Activity:");
        TextBox *behaviorBox = add_form_field(formContainer, "Behavior:");
        //TextBox *respCheckBox = add_form_field(formContainer, "Resp Check:");
        //TextBox *restraintsBox = add_form_field(formContainer, "Restraints:");
        TextBox *interventionsBox = add_form_field(formContainer, "Interventions:");
        TextBox *atBedsideBox = add_form_field(formContainer, "At Bedside:", true);
        TextBox *patientCommentBox = add_form_field(formContainer, "Patient Comment:");
        TextBox *cameraBox = add_form_field(formContainer, "Camera:", true);
        CheckBox *waitCheck = add_form_checkbox(formContainer, "Waitlist:");
        CheckBox *dischargeCheck = add_form_checkbox(formContainer, "Discharge:");
        TextBox *timeBox = add_form_field(formContainer, "Observation Time:");

        // Get current time in HH:MM format
        std::time_t now = std::time(nullptr);
        std::tm* now_tm = std::localtime(&now);
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(2) << now_tm->tm_hour << ":"
            << std::setfill('0') << std::setw(2) << now_tm->tm_min;
        timeBox->set_value(oss.str());

        // Action buttons - using a similar grid approach for consistency
        Widget *buttonContainer = new Widget(window);
        GridLayout *buttonLayout = new GridLayout(
            Orientation::Horizontal, 2,
            Alignment::Middle,
            0, 5
        );
        buttonLayout->set_col_alignment({Alignment::Fill, Alignment::Fill});
        buttonLayout->set_spacing(Orientation::Horizontal, 0);

        buttonContainer->set_layout(buttonLayout);
        Button *cancelButton = new Button(buttonContainer, "Cancel");
        cancelButton->set_background_color(Color(220, 53, 69, 255));  // Red color for cancel
        cancelButton->set_callback([this]() {
            set_visible(false);
        });

        Button *submitButton = new Button(buttonContainer, "Submit");
        submitButton->set_callback([this]() {
            // Add your submission logic here
			printf("Submitted\n");

            // You would typically save the data here and then close the form
            set_visible(false);
        });


        perform_layout();
    }

private:
    TextBox* add_form_field(Widget *parent, const std::string &labelText, bool spin=false) {
        // Add the label to the grid
        new Label(parent, labelText, "sans");

        // Add the textbox to the grid (it will automatically go to the next cell)
        TextBox *textBox = new TextBox(parent);
        textBox->set_min_height(28);
        textBox->set_alignment(TextBox::Alignment::Left);
		if(spin)
			textBox->set_spinnable(spin);

        return textBox;
    }

	CheckBox* add_form_checkbox(Widget *parent, const std::string &labelText) {
        // Add the label to the grid
        new Label(parent, labelText, "sans");

        // Add the textbox to the grid (it will automatically go to the next cell)
        CheckBox *checkBox = new CheckBox(parent,"");
        //Box->set_fixed_height(28);

        return checkBox;
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

    virtual void draw(NVGcontext *ctx) override {
        // Gradient background
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, m_size.x(), m_size.y());
        NVGpaint bg = nvgLinearGradient(ctx, 0, 0, 0, m_size.y(),
                                       nvgRGBf(0.95f, 0.96f, 0.97f), nvgRGBf(0.90f, 0.91f, 0.92f));
        nvgFillPaint(ctx, bg);
        nvgFill(ctx);
        nvgRestore(ctx);

        Screen::draw(ctx);
    }
};

int main() {
    try {
        nanogui::init();

        {
            ref<ChartingFormApp> app = new ChartingFormApp();
            app->set_visible(true);
            app->dec_ref();
            app->draw_all();
            nanogui::mainloop();
        }

        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::string error_msg = std::string("Caught a fatal error: ") + std::string(e.what());
		/*
        std::cerr << error_msg << std::endl;
		*/
		printf("EXCEPTION: %s\n", error_msg.c_str());
        return -1;
    }

    return 0;
}
