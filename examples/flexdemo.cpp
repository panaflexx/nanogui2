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
        // Professional light defaults from Theme (switchable at runtime).
        set_theme_mode(ThemeMode::Light);
        Theme* theme = m_theme.get();

        Window* window = new Window(this, WindowConfig{
            .title = "", // Content panel: no title chrome
            .position = Vector2i(0, 0),
            .size = Vector2i(300, 420),
            .resizable = true,
            .layout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::Stretch, 10, 10)
        });
		window->set_size(this->size());
		m_rootWindow = window;

        // Menu bar — derives colors from the active ThemeMode
        MenuBar* menuBar = new MenuBar(window, "");
        Dropdown* fileMenu = menuBar->add_menu("File");
        MenuItem* newSaleMenuItem = new MenuItem(fileMenu->popup(), "New Sale", FA_PLUS);
        newSaleMenuItem->set_callback([this]() { resetForm(); });
        MenuItem* saveMenuItem = new MenuItem(fileMenu->popup(), "Save", FA_SAVE);
        saveMenuItem->set_callback([this]() { saveForm(); });
        MenuItem* exitMenuItem = new MenuItem(fileMenu->popup(), "Exit", FA_CROSS);
        exitMenuItem->set_callback([this]() { requestClose(); });

        Dropdown* viewMenu = menuBar->add_menu("View");
        MenuItem* lightItem = new MenuItem(viewMenu->popup(), "Light Theme");
        lightItem->set_callback([this]() { set_theme_mode(ThemeMode::Light); perform_layout(); });
        MenuItem* darkItem = new MenuItem(viewMenu->popup(), "Dark Theme");
        darkItem->set_callback([this]() { set_theme_mode(ThemeMode::Dark); perform_layout(); });

        Dropdown* helpMenu = menuBar->add_menu("Help");
        MenuItem* aboutItem = new MenuItem(helpMenu->popup(), "About", FA_INFO);
        aboutItem->set_callback([]() {
            std::cout << "Car Sales Demo v1.0\n";
        });

        Widget* formContainer = new Widget(window);
        formContainer->set_layout(new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart, AlignItems::Stretch, 10, 10));

        Widget* custRow = new Widget(formContainer);
        custRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Stretch, 5, 5));
        new Label(custRow, "Customer Information", Label::Style::Title);
        Label* customerLabel2 = new Label(custRow, "🧑", "emoji", 24);
        (void)customerLabel2;

        Widget* nameRow = new Widget(formContainer);
        nameRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(nameRow, "Name:", Label::Style::Caption);
        customerName = new TextBox(nameRow, "", "Enter customer name");

        Widget* contactRow = new Widget(formContainer);
        contactRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(contactRow, "Contact:", Label::Style::Caption);
        customerContact = new TextBox(contactRow, "", "Enter phone or email");

        new Label(formContainer, "Vehicle Information", Label::Style::Heading);

        Widget* makeRow = new Widget(formContainer);
        makeRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(makeRow, "Make:", Label::Style::Caption);
        Dropdown* makeDropdown = new Dropdown(makeRow, Dropdown::ComboBox, "Select Make");
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
        new Label(modelRow, "Model:", Label::Style::Caption);
        new TextBox(modelRow, "", "Enter model");

        Widget* yearRow = new Widget(formContainer);
        yearRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(yearRow, "Year:", Label::Style::Caption);
        Dropdown* yearDropdown = new Dropdown(yearRow, Dropdown::ComboBox, "Select Year");
        for (int i = 2025; i >= 2010; --i) {
            std::string year = std::to_string(i);
            yearDropdown->add_item(
                {year, "year_" + year}, FA_CALENDAR,
                [this, year] { std::cout << "Selected year: " << year << "\n"; },
                std::vector<Shortcut>{{GLFW_MOD_SUPER, '0' + (i % 10)}},
                true
            );
        }
        yearDropdown->set_selected_callback([this, yearDropdown](int idx) {
            if (auto item = yearDropdown->popup()->item(idx))
                std::cout << "Dropdown callback - Selected year: " << item->caption() << "\n";
        });

        new Label(formContainer, "Sale Details", Label::Style::Heading);

        Widget* priceRow = new Widget(formContainer);
        priceRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(priceRow, "Price ($):", Label::Style::Caption);
        salePrice = new TextBox(priceRow, "", "Enter sale price");
        salePrice->set_units("$");

        Widget* statusRow = new Widget(formContainer);
        statusRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 5, 5));
        new Label(statusRow, "Status:", Label::Style::Caption);
        Dropdown* statusDropdown = new Dropdown(statusRow, Dropdown::ComboBox, "Select Status");
        std::vector<std::string> statuses = {"Pending", "Completed", "Cancelled"};
        for (const auto& status : statuses) {
            statusDropdown->add_item(
                {status, status + "_item"}, FA_FLAG,
                [this, status] { std::cout << "Selected status: " << status << "\n"; },
                std::vector<Shortcut>{{GLFW_MOD_SUPER, status[0]}},
                true
            );
        }
        statusDropdown->set_selected_callback([this, statusDropdown](int idx) {
            if (auto item = statusDropdown->popup()->item(idx))
                std::cout << "Dropdown callback - Selected status: " << item->caption() << "\n";
        });

        Widget* buttonRow = new Widget(formContainer);
        buttonRow->set_layout(new FlexLayout(FlexDirection::Row, JustifyContent::FlexEnd, AlignItems::Center, 5, 10));
        Button* submitButton = new Button(buttonRow, "Submit Sale", theme->m_message_primary_button_icon);
        submitButton->set_callback([this]() { submitForm(); });
        submitButton->set_background_color(theme->m_success_color);
        Button* clearButton = new Button(buttonRow, "Clear Form", FA_TRASH);
        clearButton->set_callback([this]() { resetForm(); });
        clearButton->set_background_color(theme->m_danger_color);

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
