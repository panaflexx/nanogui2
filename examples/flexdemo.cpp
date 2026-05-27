#include <GLFW/glfw3.h> // Include GLFW for modifier definitions
#include <nanogui/nanogui.h>
#include <nanogui/menu.h>
#include <iostream>

using namespace nanogui;

class CarSalesApp : public Screen {
public:
	Window *m_rootWindow = nullptr;

    CarSalesApp() : Screen(Vector2i(800, 600), "Car Dealership Sales Entry", true) {
		inc_ref();
        // Initialize theme with m_nvg_context
        Theme* theme = new Theme(m_nvg_context);
        theme->m_standard_font_size = 18;
        theme->m_button_font_size = 20;
        theme->m_text_box_font_size = 18;
        theme->m_window_corner_radius = 4;
        theme->m_button_corner_radius = 4;
        theme->m_window_fill_unfocused = Color(230, 230, 230, 230);
        theme->m_window_fill_focused = Color(245, 245, 245, 230);
        theme->m_button_gradient_top_focused = Color(64, 164, 232, 255);
        theme->m_button_gradient_bot_focused = Color(48, 140, 200, 255);
        theme->m_button_gradient_top_unfocused = Color(100, 100, 100, 255);
        theme->m_button_gradient_bot_unfocused = Color(80, 80, 80, 255);
        theme->m_text_color = Color(0, 0, 0, 255); // Black text for readability
        theme->m_success_color = Color(34, 139, 34, 255); // Forest green for submit button
        theme->m_border_light = Color(150, 150, 150, 255); // Light gray border
        theme->m_border_dark = Color(50, 50, 50, 255); // Dark gray border
        theme->m_window_header_gradient_top = Color(100, 100, 100, 255);
        theme->m_window_header_gradient_bot = Color(80, 80, 80, 255);
        theme->m_window_title_focused = Color(0, 0, 0, 255); // Black title when focused
        theme->m_window_title_unfocused = Color(100, 100, 100, 255); // Gray title when unfocused
        set_theme(theme);

        // Main window with FlexLayout
/*
        Window* window = new Window(this, "");
        window->set_position(Vector2i(0, 0));
        window->set_size(this->size());
		window->set_resizable(true);

        // Use FlexLayout with vertical direction and spacing
        FlexLayout* layout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::Stretch, 10, 10);
        window->set_layout(layout);

		*/

		// Main window with designated initializers (Using WindowConfig struct... nice!)
        Window* window = new Window(this, WindowConfig{
            .title = "", // No title so no window decoration
            .position = Vector2i(0, 0),
            .size = Vector2i(300,420),
            .resizable = true,
            .layout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::Stretch, 10, 10)
        });
		// Set a window size (min), perform layout, then set a new size (regular) to 
		// set the window minimum size.	
		//perform_layout();
		window->set_size( this->size() );
		m_rootWindow = window; // For resizing 

        // Menu bar
        Widget* menuBar = new Widget(window);
        menuBar->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        
        // File Menu using Dropdown and PopupMenu
        Dropdown* fileMenu = new Dropdown(menuBar, Dropdown::Menu, "File 📁");
        fileMenu->set_icon(theme->m_popup_chevron_right_icon);
        fileMenu->set_flags(Button::NormalButton); // Menu items are not mutually exclusive
        PopupMenu* filePopup = fileMenu->popup();
        filePopup->set_layout(new GroupLayout(10));
        MenuItem* newSaleMenuItem = new MenuItem(filePopup, "New Sale 🆕", FA_PLUS);
        newSaleMenuItem->set_callback([this]() { resetForm(); });
        MenuItem* saveMenuItem = new MenuItem(filePopup, "Save 💾", FA_SAVE);
        saveMenuItem->set_callback([this]() { saveForm(); });
        MenuItem* exitMenuItem = new MenuItem(filePopup, "Exit 🚪", FA_CROSS);
        exitMenuItem->set_callback([this]() { requestClose(); });

        // Help Menu using Dropdown and PopupMenu
        Dropdown* helpMenu = new Dropdown(menuBar, Dropdown::Menu, "Help ❓");
        helpMenu->set_icon(theme->m_popup_chevron_right_icon);
        helpMenu->set_flags(Button::NormalButton); // Menu items are not mutually exclusive
        PopupMenu* helpPopup = helpMenu->popup();
        helpPopup->set_layout(new GroupLayout(10));
        Button* aboutButton = new Button(helpPopup, "About ℹ️", FA_INFO);
        aboutButton->set_callback([]() { 
            std::cout << "Car Sales Demo v1.0\n"; 
        });

        // Form container with FlexLayout
        Widget* formContainer = new Widget(window);
        formContainer->set_layout(new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::Stretch, 10, 10));

        // Customer Information Section
        Widget* custRow = new Widget(formContainer);
        custRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Stretch, 5, 5));
        Label* customerLabel = new Label(custRow, "Customer Information 🧑");
        customerLabel->set_font_size(30);
        customerLabel->set_font("sans-bold");
        Label* customerLabel2 = new Label(custRow, "🧑");
        customerLabel2->set_font_size(30);
        customerLabel2->set_font("emoji");
        
        Widget* nameRow = new Widget(formContainer);
        nameRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Stretch, 5, 5));
        new Label(nameRow, "Name:");
        customerName = new TextBox(nameRow,"");
        customerName->set_width(300);
        customerName->set_placeholder("Enter customer name");

        Widget* contactRow = new Widget(formContainer);
        contactRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(contactRow, "Contact:");
        customerContact = new TextBox(contactRow,"");
        customerContact->set_width(300);
        customerContact->set_placeholder("Enter phone or email");

        // Vehicle Information Section
        Label* vehicleLabel = new Label(formContainer, "Vehicle Information 🚘");
        vehicleLabel->set_font_size(20);
        vehicleLabel->set_font("sans-bold");
        
        Widget* makeRow = new Widget(formContainer);
        makeRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(makeRow, "Make:");
        Dropdown* makeDropdown = new Dropdown(makeRow, Dropdown::ComboBox, "Select Make");
        makeDropdown->set_width(200);
		makeDropdown->set_text_color( Color(255,255,255,255));
        std::vector<std::string> makes = {"Toyota", "Honda", "Ford", "Chevrolet", "BMW", "Mercedes"};
        for (const auto& make : makes) {
            makeDropdown->add_item(
                {make, make + "_item"}, FA_CAR,
                [this, make] { std::cout << "Selected make: " << make << "\n"; },
                std::vector<Shortcut>{{GLFW_MOD_SUPER, make[0]}}, // Shortcut: Cmd + first letter of make
                true
            );
        }
        makeDropdown->set_selected_callback([this, makeDropdown](int idx) {
            if (auto item = makeDropdown->popup()->item(idx))
                std::cout << "Dropdown callback - Selected make: " << item->caption() << "\n";
        });

        Widget* modelRow = new Widget(formContainer);
        modelRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(modelRow, "Model:");
        TextBox* modelText = new TextBox(modelRow,"");
        modelText->set_width(200);
        modelText->set_placeholder("Enter model");

        Widget* yearRow = new Widget(formContainer);
        yearRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(yearRow, "Year:");
        Dropdown* yearDropdown = new Dropdown(yearRow, Dropdown::ComboBox, "Select Year");
        yearDropdown->set_width(200);
		yearDropdown->set_text_color( Color(255,255,255,255));
        for (int i = 2025; i >= 2010; --i) {
            std::string year = std::to_string(i);
            yearDropdown->add_item(
                {year, "year_" + year}, FA_CALENDAR,
                [this, year] { std::cout << "Selected year: " << year << "\n"; },
                std::vector<Shortcut>{{GLFW_MOD_SUPER, '0' + (i % 10)}}, // Shortcut: Cmd + last digit
                true
            );
        }
        yearDropdown->set_selected_callback([this, yearDropdown](int idx) {
            if (auto item = yearDropdown->popup()->item(idx))
                std::cout << "Dropdown callback - Selected year: " << item->caption() << "\n";
        });

        // Sale Details Section
        Label* saleLabel = new Label(formContainer, "Sale Details 💰");
        saleLabel->set_font_size(20);
        saleLabel->set_font("sans-bold");
        
        Widget* priceRow = new Widget(formContainer);
        priceRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(priceRow, "Price ($):");
        salePrice = new TextBox(priceRow,"");
        salePrice->set_min_size( Vector2i(200,24) );
        salePrice->set_placeholder("Enter sale price");
        salePrice->set_units("$");

        Widget* statusRow = new Widget(formContainer);
        statusRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(statusRow, "Status:");
        Dropdown* statusDropdown = new Dropdown(statusRow, Dropdown::ComboBox, "Select Status");
        statusDropdown->set_min_size( Vector2i(200,24) );
		statusDropdown->set_text_color( Color(255,255,255,255));
        std::vector<std::string> statuses = {"Pending", "Completed", "Cancelled"};
        for (const auto& status : statuses) {
            statusDropdown->add_item(
                {status, status + "_item"}, FA_FLAG,
                [this, status] { std::cout << "Selected status: " << status << "\n"; },
                std::vector<Shortcut>{{GLFW_MOD_SUPER, status[0]}}, // Shortcut: Cmd + first letter of status
                true
            );
        }
        statusDropdown->set_selected_callback([this, statusDropdown](int idx) {
            if (auto item = statusDropdown->popup()->item(idx))
                std::cout << "Dropdown callback - Selected status: " << item->caption() << "\n";
        });

        // Action Buttons
        Widget* buttonRow = new Widget(formContainer);
        buttonRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexEnd, AlignItems::Center, 5, 10));
        Button* submitButton = new Button(buttonRow, "Submit Sale ✅", theme->m_message_primary_button_icon);
        submitButton->set_callback([this]() { submitForm(); });
        submitButton->set_background_color(theme->m_success_color); // Use success color for submit
        Button* clearButton = new Button(buttonRow, "Clear Form 🗑️", FA_TRASH);
        clearButton->set_callback([this]() { resetForm(); });

        // Center the window
        perform_layout(m_nvg_context);
        window->center();
    }

	// Makes background window resize with system window (screen)
    virtual bool resize_event(const Vector2i &size) override {
        if (m_rootWindow) {
            m_rootWindow->set_size(size);
            perform_layout(); 
        }
        Screen::resize_event(size);
        return true;
    }


private:
    TextBox* customerName;
    TextBox* customerContact;
    TextBox* salePrice;

    void resetForm() {
        customerName->set_value("");
        customerContact->set_value("");
        salePrice->set_value("");
        std::cout << "Form cleared\n";
    }

    void saveForm() {
        std::cout << "Saving form data...\n";
        std::cout << "Customer: " << customerName->value() << "\n";
        std::cout << "Contact: " << customerContact->value() << "\n";
        std::cout << "Price: " << salePrice->value() << "\n";
    }

    void submitForm() {
        std::cout << "Submitting sale...\n";
        std::cout << "Customer: " << customerName->value() << "\n";
        std::cout << "Contact: " << customerContact->value() << "\n";
        std::cout << "Price: " << salePrice->value() << "\n";
    }

    void requestClose() {
        set_visible(false);
    }
};

int main(int argc, char** argv) {
    try {
        nanogui::init();
        {
            nanogui::ref<CarSalesApp> app = new CarSalesApp();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop(); // Powersaver
        }
        nanogui::shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}
