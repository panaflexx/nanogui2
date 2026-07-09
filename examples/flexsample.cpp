#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanogui/cachedwidget.h>
#include <nanogui/layout.h>
#include <nanogui/textbox.h>
#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/menu.h>
#include <nanogui/split.h>
#include <GLFW/glfw3.h>
#include <iostream>

// Add this after your includes to define the Split widget
// (This is the Split widget implementation we created earlier)
// [Insert the Split widget implementation here]

// If you've already added Split to NanoGUI, uncomment this instead:
// #include <nanogui/split.h>

using namespace nanogui;

class CarSalesFormApp : public Screen {
public:
    Window *m_rootWindow = nullptr;
	Label *m_loan_amount, *m_interest_rate, *m_trade_in_value, *m_total_loan, *m_monthly_payment;
	Label *m_term, *m_total_interest;
	CheckBox *m_has_trade_in;

    CarSalesFormApp() : Screen(Vector2i(1080, 800), "Car Sales Management System") {
        //inc_ref();

        // Custom theme for professional look
		/*
        Theme *theme = m_theme;
        theme->m_window_fill_unfocused = Color(248, 249, 250, 255);
        theme->m_window_fill_focused = Color(255, 255, 255, 255);
        theme->m_text_color = Color(33, 37, 41, 255);
        theme->m_button_gradient_top_unfocused = Color(52, 144, 220, 255);
        theme->m_button_gradient_bot_unfocused = Color(41, 128, 204, 255);
		*/

        // Main window
        Window *window = new Window(this, "", true);
        m_rootWindow = window;
        window->set_position(Vector2i(0, 0));
        //window->set_size(this->size());
        window->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

        // Create a Split widget as the main container
        Split *split = new Split(window, Split::Orientation::Horizontal);
		//split->set_max_size( this->size());
		split->set_max_size({2048,2048});
		split->set_min_size( 100 );
		split->set_grow_parent( true );

        // LEFT PANEL: Customer List
		ScrollPanel *leftPanel = new ScrollPanel(split);
		leftPanel->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);
		//leftPanel->set_min_width(100);

		// CREATE A SINGLE CONTAINER WIDGET FOR THE SCROLLPANEL
		Widget *leftContainer = new Widget(leftPanel);  // ScrollPanel has ONE child
		leftContainer->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 10));
		//leftContainer->set_min_size( Vector2i(100, 1800) );

		// Add header for left panel to the CONTAINER, not the ScrollPanel
		Label *leftHeader = new Label(leftContainer, "Customers", "sans-bold", 16);
		leftHeader->set_color(Color(52, 144, 220, 255));

		// Add some customer list items
		std::vector<std::string> customers = {
			"John Smith", "Jane Doe", "Robert Johnson", "Sarah Williams",
			"Michael Brown", "Emily Davis", "William Miller", "Olivia Wilson",
			"Michael Brown", "Emily Davis", "William Miller", "Roger Davenport",
			"Charles Vidal"
		};

		for (const auto& customer : customers) {
			Widget *item = new Widget(leftContainer);  // Add to CONTAINER, not ScrollPanel
			item->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Fill, 0, 5));
			item->set_size( Vector2i(50, 30) );
			item->set_min_height(14);

			// Add customer icon
			Label *icon = new Label(item, "👤", "emoji", 14);
			icon->set_width(24);

			// Add customer name
			Label *name = new Label(item, customer, "sans", 13);
			name->set_width(100);
		}
		Widget *padContainer = new Widget(leftContainer);
		padContainer->set_min_width( 10 );

		// RIGHT PANEL: Existing form content (now zoomable + cached)
		ZoomScrollPanel *rightPanel = new ZoomScrollPanel(split, ZoomScrollPanel::ScrollTypes::Both);
		rightPanel->set_zoom(1.0); // default zoom

		// Wrap the content in a CachedWidget for performance
		Widget *cachedContent = new Widget(rightPanel);
		FlexLayout *mainLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
									AlignItems::Stretch, 20, 15);
		cachedContent->set_layout(mainLayout);

		// Main content container
		Widget *contentContainer = cachedContent;

		// Rest remains the same...
		create_header(contentContainer, mainLayout);
		create_vehicle_section(contentContainer, mainLayout);
		create_customer_section(contentContainer, mainLayout);
		create_financing_section(contentContainer, mainLayout);
		create_options_section(contentContainer, mainLayout);
		create_loan_table(contentContainer);
		create_action_buttons(contentContainer, mainLayout);
		add_spacer(contentContainer, mainLayout, 30);

        // Set initial split position (25% for left panel, 75% for right)
        split->set_drag_position(0.15f);

		// Set the window size right before perform_layout
        window->set_size(this->size());
        perform_layout();
    }

    // Rest of the class remains the same...
    // [All your existing private methods below]

private:
    void create_header(Widget *parent, FlexLayout *parentLayout) {
        // Header container with centered alignment
        Widget *headerContainer = new Widget(parent);
        FlexLayout *headerLayout = new FlexLayout(FlexDirection::Column, JustifyContent::Center,
                                                  AlignItems::Center, 0, 10);
        headerContainer->set_layout(headerLayout);
        parentLayout->set_flex_item(headerContainer, FlexLayout::FlexItem(0.0f, 0.0f, 80));

        Label *title = new Label(headerContainer, "Car Sales Management System", "sans-bold", 24);
        title->set_color(Color(52, 144, 220, 255));
        title->set_min_size(Vector2i(300, 30)); // Minimum width for title, reasonable height
        title->set_max_size(Vector2i(600, 40)); // Maximum width to prevent excessive stretching
        headerLayout->set_flex_item(title, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        Label *subtitle = new Label(headerContainer, "Complete Vehicle Sales Form", "sans", 14);
        subtitle->set_color(Color(108, 117, 125, 255));
        subtitle->set_min_size(Vector2i(250, 20)); // Minimum width, reasonable height
        subtitle->set_max_size(Vector2i(500, 25)); // Maximum width
        headerLayout->set_flex_item(subtitle, FlexLayout::FlexItem(0.0f, 0.0f, -1));
    }

    Widget* create_section(Widget *parent, FlexLayout *parentLayout, const std::string &title) {
        // Section container
        Widget *sectionContainer = new Widget(parent);
        FlexLayout *sectionLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                   AlignItems::Stretch, 15, 12);
        sectionContainer->set_layout(sectionLayout);
        parentLayout->set_flex_item(sectionContainer, FlexLayout::FlexItem(0.0f, 0.0f, -1));
		sectionContainer->set_max_size(Vector2i(800, 1000));

        // Section header with background
        Widget *headerWidget = new Widget(sectionContainer);
        //headerWidget->set_height(80);
        //headerWidget->set_size(Vector2i(600, 40)); // Minimum width and height
        //headerWidget->set_max_size(Vector2i(800, 80)); // Maximum width
        FlexLayout *headerLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                                  AlignItems::Center, 15, 0);
        headerWidget->set_layout(headerLayout);
        sectionLayout->set_flex_item(headerWidget, FlexLayout::FlexItem(0.0f, 0.0f, 60));

        Label *sectionTitle = new Label(headerWidget, title, "sans-bold", 30);
        sectionTitle->set_color(Color(52, 144, 220, 255));
        //sectionTitle->set_min_width(800);
        sectionTitle->set_min_size(Vector2i(450, 55)); // Minimum width based on text
        //sectionTitle->set_max_size(Vector2i(800, 55)); // Maximum width
        headerLayout->set_flex_item(sectionTitle, FlexLayout::FlexItem(0.0f, 0.0f, 55));

        return sectionContainer;
    }

    void create_form_row(Widget *parent, FlexLayout *parentLayout, const std::string &labelText,
                        Widget *inputWidget, bool fullWidth = false) {
        Widget *rowContainer = new Widget(parent);
        FlexLayout *rowLayout;

        if (fullWidth) {
            rowLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                     AlignItems::Stretch, 0, 5);
        } else {
            rowLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                     AlignItems::Center, 0, 15);
        }

        rowContainer->set_layout(rowLayout);
        parentLayout->set_flex_item(rowContainer, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Label
        Label *label = new Label(rowContainer, labelText, "sans", 13);
        label->set_color(Color(73, 80, 87, 255));
        if (!fullWidth) {
            label->set_width(180);
            rowLayout->set_flex_item(label, FlexLayout::FlexItem(0.0f, 0.0f, 180));
            rowLayout->set_flex_item(inputWidget, FlexLayout::FlexItem(1.0f, 1.0f, -1));
        } else {
            rowLayout->set_flex_item(label, FlexLayout::FlexItem(0.0f, 0.0f, -1));
            rowLayout->set_flex_item(inputWidget, FlexLayout::FlexItem(1.0f, 1.0f, -1));
        }
    }

    void create_vehicle_section(Widget *parent, FlexLayout *parentLayout) {
        Widget *section = create_section(parent, parentLayout, "🚗 Vehicle Information");
        FlexLayout *sectionLayout = static_cast<FlexLayout*>(section->layout());

        // Row 1: Make and Model
        Widget *row1 = new Widget(section);
        FlexLayout *row1Layout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                               AlignItems::Center, 0, 20);
        row1->set_layout(row1Layout);
        sectionLayout->set_flex_item(row1, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Make
        Widget *makeContainer = new Widget(row1);
        FlexLayout *makeLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                               AlignItems::Stretch, 0, 5);
        makeContainer->set_layout(makeLayout);
        row1Layout->set_flex_item(makeContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *makeLabel = new Label(makeContainer, "Make:", "sans", 13);
        makeLabel->set_color(Color(73, 80, 87, 255));
        makeLabel->set_min_size(Vector2i(50, 20)); // Minimum width based on text
        makeLabel->set_max_size(Vector2i(100, 25)); // Maximum width

        Dropdown *makeDropdown = new Dropdown(makeContainer, {"Toyota", "Honda", "Ford", "Chevrolet",
                                                             "Nissan", "BMW", "Mercedes-Benz", "Audi",
                                                             "Volkswagen", "Hyundai"},
                                             {}, Dropdown::ComboBox, "Select Make");
        makeDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected make index: " << idx << std::endl;
        });
        makeDropdown->set_min_size(Vector2i(150, 30)); // Minimum width for longest option, reasonable height
        makeDropdown->set_max_size(Vector2i(300, 40)); // Maximum width to prevent excessive stretching
        makeLayout->set_flex_item(makeLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        makeLayout->set_flex_item(makeDropdown, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Model
        Widget *modelContainer = new Widget(row1);
        FlexLayout *modelLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                AlignItems::Stretch, 0, 5);
        modelContainer->set_layout(modelLayout);
        row1Layout->set_flex_item(modelContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *modelLabel = new Label(modelContainer, "Model:", "sans", 13);
        modelLabel->set_color(Color(73, 80, 87, 255));
        modelLabel->set_min_size(Vector2i(60, 20)); // Minimum width based on text
        modelLabel->set_max_size(Vector2i(120, 25)); // Maximum width

        TextBox *modelBox = new TextBox(modelContainer);
        modelBox->set_placeholder("Enter model");
        modelBox->set_min_size(Vector2i(150, 30)); // Minimum width for placeholder, reasonable height
        modelBox->set_max_size(Vector2i(300, 40)); // Maximum width
        modelLayout->set_flex_item(modelLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        modelLayout->set_flex_item(modelBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Row 2: Year and Trim
        Widget *row2 = new Widget(section);
        FlexLayout *row2Layout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                               AlignItems::Center, 0, 20);
        row2->set_layout(row2Layout);
        sectionLayout->set_flex_item(row2, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Year
        Widget *yearContainer = new Widget(row2);
        FlexLayout *yearLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                               AlignItems::Stretch, 0, 5);
        yearContainer->set_layout(yearLayout);
        row2Layout->set_flex_item(yearContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *yearLabel = new Label(yearContainer, "Year:", "sans", 13);
        yearLabel->set_color(Color(73, 80, 87, 255));
        yearLabel->set_min_size(Vector2i(50, 20)); // Minimum width based on text
        yearLabel->set_max_size(Vector2i(100, 25)); // Maximum width

        Dropdown *yearDropdown = new Dropdown(yearContainer, {"2024", "2023", "2022", "2021", "2020",
                                                             "2019", "2018", "2017", "2016", "2015"},
                                             {}, Dropdown::ComboBox, "Select Year");
        yearDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected year index: " << idx << std::endl;
        });
        yearDropdown->set_min_size(Vector2i(100, 30)); // Minimum width for longest option, reasonable height
        yearDropdown->set_max_size(Vector2i(150, 40)); // Maximum width
        yearLayout->set_flex_item(yearLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        yearLayout->set_flex_item(yearDropdown, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Trim
        Widget *trimContainer = new Widget(row2);
        FlexLayout *trimLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                               AlignItems::Stretch, 0, 5);
        trimContainer->set_layout(trimLayout);
        row2Layout->set_flex_item(trimContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *trimLabel = new Label(trimContainer, "Trim Level:", "sans", 13);
        trimLabel->set_color(Color(73, 80, 87, 255));
        trimLabel->set_min_size(Vector2i(80, 20)); // Minimum width based on text
        trimLabel->set_max_size(Vector2i(150, 25)); // Maximum width

        Dropdown *trimDropdown = new Dropdown(trimContainer, {"Base", "Sport", "Limited", "Premium", "Luxury"},
                                             {}, Dropdown::ComboBox, "Select Trim");
        trimDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected trim index: " << idx << std::endl;
        });
        trimDropdown->set_min_size(Vector2i(150, 30)); // Minimum width for longest option, reasonable height
        trimDropdown->set_max_size(Vector2i(250, 40)); // Maximum width
        trimLayout->set_flex_item(trimLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        trimLayout->set_flex_item(trimDropdown, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Row 3: Color and Mileage
        Widget *row3 = new Widget(section);
        FlexLayout *row3Layout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                               AlignItems::Center, 0, 20);
        row3->set_layout(row3Layout);
        sectionLayout->set_flex_item(row3, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Color
        Widget *colorContainer = new Widget(row3);
        FlexLayout *colorLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                AlignItems::Stretch, 0, 5);
        colorContainer->set_layout(colorLayout);
        row3Layout->set_flex_item(colorContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *colorLabel = new Label(colorContainer, "Color:", "sans", 13);
        colorLabel->set_color(Color(73, 80, 87, 255));
        colorLabel->set_min_size(Vector2i(50, 20)); // Minimum width based on text
        colorLabel->set_max_size(Vector2i(100, 25)); // Maximum width

        Dropdown *colorDropdown = new Dropdown(colorContainer, {"White", "Black", "Silver", "Red", "Blue",
                                                               "Gray", "Green", "Gold", "Brown"},
                                              {}, Dropdown::ComboBox, "Select Color");
        colorDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected color index: " << idx << std::endl;
        });
        colorDropdown->set_min_size(Vector2i(150, 30)); // Minimum width for longest option, reasonable height
        colorDropdown->set_max_size(Vector2i(250, 40)); // Maximum width
        colorLayout->set_flex_item(colorLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        colorLayout->set_flex_item(colorDropdown, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Mileage
        Widget *mileageContainer = new Widget(row3);
        FlexLayout *mileageLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                  AlignItems::Stretch, 0, 5);
        mileageContainer->set_layout(mileageLayout);
        row3Layout->set_flex_item(mileageContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *mileageLabel = new Label(mileageContainer, "Mileage:", "sans", 13);
        mileageLabel->set_color(Color(73, 80, 87, 255));
        mileageLabel->set_min_size(Vector2i(70, 20)); // Minimum width based on text
        mileageLabel->set_max_size(Vector2i(120, 25)); // Maximum width

        TextBox *mileageBox = new TextBox(mileageContainer);
        mileageBox->set_placeholder("Enter mileage");
        mileageBox->set_units("miles");
        mileageBox->set_min_size(Vector2i(150, 30)); // Minimum width for placeholder, reasonable height
        mileageBox->set_max_size(Vector2i(200, 40)); // Maximum width
        mileageLayout->set_flex_item(mileageLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        mileageLayout->set_flex_item(mileageBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // VIN
        TextBox *vinBox = new TextBox(section);
        vinBox->set_placeholder("Vehicle Identification Number (VIN)");
        vinBox->set_min_size(Vector2i(300, 30)); // Minimum width for VIN placeholder, reasonable height
        vinBox->set_max_size(Vector2i(500, 40)); // Maximum width
        create_form_row(section, sectionLayout, "VIN:", vinBox, true);
    }

    void create_customer_section(Widget *parent, FlexLayout *parentLayout) {
        Widget *section = create_section(parent, parentLayout, "👤 Customer Information");
        FlexLayout *sectionLayout = static_cast<FlexLayout*>(section->layout());

        // Row 1: Name fields
        Widget *nameRow = new Widget(section);
        FlexLayout *nameLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                               AlignItems::Center, 0, 15);
        nameRow->set_layout(nameLayout);
        sectionLayout->set_flex_item(nameRow, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // First Name
        Widget *firstNameContainer = new Widget(nameRow);
        FlexLayout *firstNameLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                    AlignItems::Stretch, 0, 5);
        firstNameContainer->set_layout(firstNameLayout);
        nameLayout->set_flex_item(firstNameContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *firstNameLabel = new Label(firstNameContainer, "First Name:", "sans", 13);
        firstNameLabel->set_color(Color(73, 80, 87, 255));
        TextBox *firstNameBox = new TextBox(firstNameContainer,"");
        firstNameBox->set_placeholder("Enter first name");
        firstNameLayout->set_flex_item(firstNameLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        firstNameLayout->set_flex_item(firstNameBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Last Name
        Widget *lastNameContainer = new Widget(nameRow);
        FlexLayout *lastNameLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                   AlignItems::Stretch, 0, 5);
        lastNameContainer->set_layout(lastNameLayout);
        nameLayout->set_flex_item(lastNameContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *lastNameLabel = new Label(lastNameContainer, "Last Name:", "sans", 13);
        lastNameLabel->set_color(Color(73, 80, 87, 255));
        TextBox *lastNameBox = new TextBox(lastNameContainer);
        lastNameBox->set_placeholder("Enter last name");
        lastNameLayout->set_flex_item(lastNameLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        lastNameLayout->set_flex_item(lastNameBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Contact info
        TextBox *emailBox = new TextBox(section);
        emailBox->set_placeholder("customer@email.com");
        create_form_row(section, sectionLayout, "Email:", emailBox);

        TextBox *phoneBox = new TextBox(section);
        phoneBox->set_placeholder("(555) 123-4567");
        create_form_row(section, sectionLayout, "Phone:", phoneBox);

        // Address
        TextBox *addressBox = new TextBox(section);
        addressBox->set_placeholder("Street address");
        create_form_row(section, sectionLayout, "Address:", addressBox, true);

        // City, State, ZIP row
        Widget *locationRow = new Widget(section);
        FlexLayout *locationLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                                   AlignItems::Center, 0, 15);
        locationRow->set_layout(locationLayout);
        sectionLayout->set_flex_item(locationRow, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // City
        Widget *cityContainer = new Widget(locationRow);
        FlexLayout *cityLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                               AlignItems::Stretch, 0, 5);
        cityContainer->set_layout(cityLayout);
        locationLayout->set_flex_item(cityContainer, FlexLayout::FlexItem(2.0f, 2.0f, -1));

        Label *cityLabel = new Label(cityContainer, "City:", "sans", 13);
        cityLabel->set_color(Color(73, 80, 87, 255));
        TextBox *cityBox = new TextBox(cityContainer);
        cityBox->set_placeholder("City");
        cityLayout->set_flex_item(cityLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        cityLayout->set_flex_item(cityBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // State
        Widget *stateContainer = new Widget(locationRow);
        FlexLayout *stateLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                AlignItems::Stretch, 0, 5);
        stateContainer->set_layout(stateLayout);
        locationLayout->set_flex_item(stateContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *stateLabel = new Label(stateContainer, "State:", "sans", 13);
        stateLabel->set_color(Color(73, 80, 87, 255));

        Dropdown *stateDropdown = new Dropdown(stateContainer, {"CA", "NY", "TX", "FL", "IL", "PA",
                                                               "OH", "GA", "NC", "MI"},
                                              {}, Dropdown::ComboBox, "State");
        stateDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected state index: " << idx << std::endl;
        });

        stateLayout->set_flex_item(stateLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        stateLayout->set_flex_item(stateDropdown, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // ZIP
        Widget *zipContainer = new Widget(locationRow);
        FlexLayout *zipLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                              AlignItems::Stretch, 0, 5);
        zipContainer->set_layout(zipLayout);
        locationLayout->set_flex_item(zipContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *zipLabel = new Label(zipContainer, "ZIP:", "sans", 13);
        zipLabel->set_color(Color(73, 80, 87, 255));
        TextBox *zipBox = new TextBox(zipContainer);
        zipBox->set_placeholder("12345");
        zipLayout->set_flex_item(zipLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        zipLayout->set_flex_item(zipBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));
    }

    void create_financing_section(Widget *parent, FlexLayout *parentLayout) {
        Widget *section = create_section(parent, parentLayout, "💰 Financing Information");
        FlexLayout *sectionLayout = static_cast<FlexLayout*>(section->layout());

        // Purchase type
        Dropdown *purchaseTypeDropdown = new Dropdown(section, {"Cash Purchase", "Finance", "Lease"},
                                                     {}, Dropdown::ComboBox, "Select Type");
        purchaseTypeDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected purchase type index: " << idx << std::endl;
        });
        create_form_row(section, sectionLayout, "Purchase Type:", purchaseTypeDropdown);

        // Price row
        Widget *priceRow = new Widget(section);
        FlexLayout *priceLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                                AlignItems::Center, 0, 20);
        priceRow->set_layout(priceLayout);
        sectionLayout->set_flex_item(priceRow, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Sale Price
        Widget *salePriceContainer = new Widget(priceRow);
        FlexLayout *salePriceLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                    AlignItems::Stretch, 0, 5);
        salePriceContainer->set_layout(salePriceLayout);
        priceLayout->set_flex_item(salePriceContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *salePriceLabel = new Label(salePriceContainer, "Sale Price:", "sans", 13);
        salePriceLabel->set_color(Color(73, 80, 87, 255));
        TextBox *salePriceBox = new TextBox(salePriceContainer);
        salePriceBox->set_placeholder("0.00");
        salePriceBox->set_units("$");
        salePriceLayout->set_flex_item(salePriceLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        salePriceLayout->set_flex_item(salePriceBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Down Payment
        Widget *downPaymentContainer = new Widget(priceRow);
        FlexLayout *downPaymentLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                      AlignItems::Stretch, 0, 5);
        downPaymentContainer->set_layout(downPaymentLayout);
        priceLayout->set_flex_item(downPaymentContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *downPaymentLabel = new Label(downPaymentContainer, "Down Payment:", "sans", 13);
        downPaymentLabel->set_color(Color(73, 80, 87, 255));
        TextBox *downPaymentBox = new TextBox(downPaymentContainer);
        downPaymentBox->set_placeholder("0.00");
        downPaymentBox->set_units("$");
        downPaymentLayout->set_flex_item(downPaymentLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        downPaymentLayout->set_flex_item(downPaymentBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Financing terms
        Widget *termsRow = new Widget(section);
        FlexLayout *termsLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                                AlignItems::Center, 0, 20);
        termsRow->set_layout(termsLayout);
        sectionLayout->set_flex_item(termsRow, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Interest Rate
        Widget *interestContainer = new Widget(termsRow);
        FlexLayout *interestLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                   AlignItems::Stretch, 0, 5);
        interestContainer->set_layout(interestLayout);
        termsLayout->set_flex_item(interestContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *interestLabel = new Label(interestContainer, "Interest Rate:", "sans", 13);
        interestLabel->set_color(Color(73, 80, 87, 255));
        TextBox *interestBox = new TextBox(interestContainer);
        interestBox->set_placeholder("4.50");
        interestBox->set_units("%");
        interestLayout->set_flex_item(interestLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        interestLayout->set_flex_item(interestBox, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Loan Term
        Widget *termContainer = new Widget(termsRow);
        FlexLayout *termLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                               AlignItems::Stretch, 0, 5);
        termContainer->set_layout(termLayout);
        termsLayout->set_flex_item(termContainer, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        Label *termLabel = new Label(termContainer, "Loan Term:", "sans", 13);
        termLabel->set_color(Color(73, 80, 87, 255));

        Dropdown *termDropdown = new Dropdown(termContainer, {"36 months", "48 months", "60 months",
                                                             "72 months", "84 months"},
                                             {}, Dropdown::ComboBox, "Select Term");
        termDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected loan term index: " << idx << std::endl;
        });

        termLayout->set_flex_item(termLabel, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        termLayout->set_flex_item(termDropdown, FlexLayout::FlexItem(0.0f, 0.0f, -1));
    }

    void create_options_section(Widget *parent, FlexLayout *parentLayout) {
        Widget *section = create_section(parent, parentLayout, "🎯 Additional Options");
        FlexLayout *sectionLayout = static_cast<FlexLayout*>(section->layout());

        // Warranty options
        Widget *warrantyContainer = new Widget(section);
        FlexLayout *warrantyLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                                   AlignItems::Center, 0, 15);
        warrantyContainer->set_layout(warrantyLayout);
        sectionLayout->set_flex_item(warrantyContainer, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        Label *warrantyLabel = new Label(warrantyContainer, "Warranty:", "sans", 18);
        warrantyLabel->set_color(Color(73, 80, 187, 255));
        warrantyLabel->set_width(180);
        warrantyLayout->set_flex_item(warrantyLabel, FlexLayout::FlexItem(0.0f, 0.0f, 180));

        CheckBox *extendedWarranty = new CheckBox(warrantyContainer, "Extended Warranty (+$2,500)");
        warrantyLayout->set_flex_item(extendedWarranty, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        // Insurance
        Widget *insuranceContainer = new Widget(section);
        FlexLayout *insuranceLayout = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                                    AlignItems::FlexStart, 0, 15);
        insuranceContainer->set_layout(insuranceLayout);
        sectionLayout->set_flex_item(insuranceContainer, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        Label *insuranceLabel = new Label(insuranceContainer, "Insurance:", "sans", 18);
        insuranceLabel->set_color(Color(73, 80, 187, 255));
        insuranceLabel->set_min_width(180);
        insuranceLayout->set_flex_item(insuranceLabel, FlexLayout::FlexItem(0.0f, 0.0f, 180));

        Widget *insuranceOptions = new Widget(insuranceContainer);
        FlexLayout *optionsLayout = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                                  AlignItems::FlexStart, 0, 8);
        insuranceOptions->set_layout(optionsLayout);
        insuranceLayout->set_flex_item(insuranceOptions, FlexLayout::FlexItem(1.0f, 1.0f, -1));

        CheckBox *gapInsurance = new CheckBox(insuranceOptions, "GAP Insurance (+$800)");
        CheckBox *paintProtection = new CheckBox(insuranceOptions, "Paint Protection (+$1,200)");
        CheckBox *fabricProtection = new CheckBox(insuranceOptions, "Fabric Protection (+$600)");

        optionsLayout->set_flex_item(gapInsurance, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        optionsLayout->set_flex_item(paintProtection, FlexLayout::FlexItem(0.0f, 0.0f, -1));
        optionsLayout->set_flex_item(fabricProtection, FlexLayout::FlexItem(0.0f, 0.0f, -1));

        // Trade-in
        Dropdown *tradeInDropdown = new Dropdown(section, {"No Trade-In", "Has Trade-In Vehicle"},
                                                {}, Dropdown::ComboBox, "Trade-In Status");
        tradeInDropdown->set_selected_callback([](int idx) {
            std::cout << "Selected trade-in option index: " << idx << std::endl;
        });
        create_form_row(section, sectionLayout, "Trade-In:", tradeInDropdown);
    }

    void create_action_buttons(Widget *parent, FlexLayout *parentLayout) {
        add_spacer(parent, parentLayout, 20);

        Widget *buttonContainer = new Widget(parent);
        FlexLayout *buttonLayout = new FlexLayout(FlexDirection::Row, JustifyContent::SpaceEvenly,
                                                 AlignItems::Center, 0, 15);
        buttonContainer->set_layout(buttonLayout);
        parentLayout->set_flex_item(buttonContainer, FlexLayout::FlexItem(0.0f, 0.0f, 60));

        Button *saveButton = new Button(buttonContainer, "💾 Save Draft");
        saveButton->set_min_width(150);
        saveButton->set_callback([]() {
            std::cout << "Form saved as draft!" << std::endl;
        });
        buttonLayout->set_flex_item(saveButton, FlexLayout::FlexItem(0.0f, 0.0f, 150));

        Button *submitButton = new Button(buttonContainer, "✅ Submit Sale");
        submitButton->set_min_width(150);
        submitButton->set_callback([]() {
            std::cout << "Sale submitted for processing!" << std::endl;
        });
        buttonLayout->set_flex_item(submitButton, FlexLayout::FlexItem(0.0f, 0.0f, 150));

        Button *clearButton = new Button(buttonContainer, "🔄 Clear Form");
        clearButton->set_min_width(150);
		clearButton->set_id("CLEAR");
        clearButton->set_callback([clearButton]() {
            std::cout << "Form cleared!" << std::endl;
			clearButton->set_animation_type(Widget::AnimationType::SlideOpen);
			clearButton->set_animation_duration(1.0);
			clearButton->set_visible(true);
			clearButton->start_animation();
        });
        buttonLayout->set_flex_item(clearButton, FlexLayout::FlexItem(0.0f, 0.0f, 150));
    }

	void create_loan_table(Widget *parent) {
		Widget *table = new Widget(parent);
		// Use GridLayout with 2 columns and table drawing enabled
		GridLayout *layout = new GridLayout(Orientation::Horizontal, 2, Alignment::Middle, 15, 5);
		layout->set_col_alignment( Alignment::Fill);
		layout->set_row_alignment( Alignment::Middle );
		table->set_max_width(400);

		TableTheme theme;
		theme.header_background = Color(100, 100, 100, 255);
		theme.even_row_background = Color(150, 140, 140, 255);
		theme.odd_row_background = Color(175, 155, 155, 255);
		theme.border_color = Color(50, 50, 50, 128);
		theme.border_width = 1.0f;
		theme.shade_rows = true;
		theme.first_row_is_header = true;
		layout->enable_draw_table(theme);

		table->set_layout(layout);
		//table->set_min_height( 200 );
		table->set_id("table");

		// Header
		new Label(table, "Description ");
		new Label(table, "Value ");
		// Form inputs
		new Label(table, "Loan Amount ($)");
		m_loan_amount = new Label(table, "$0.00");

		new Label(table, "Interest Rate (%)");
		m_interest_rate = new Label(table, "5.00%");

		new Label(table, "Loan Term (years)");
		m_trade_in_value = new Label(table, "5");

		new Label(table, "Has Trade-In");
		m_has_trade_in = new CheckBox(table, "");
		m_has_trade_in->set_checked(false);
		m_has_trade_in->set_min_height(16);

		new Label(table, "Trade-In Value ($)");
		m_trade_in_value = new Label(table, "50");

		// Loan calculation results
		new Label(table, "Total Loan Amount");
		m_total_loan = new Label(table, "---");

		new Label(table, "Monthly Payment");
		m_monthly_payment = new Label(table, "---");

		new Label(table, "Total Interest");
		m_total_interest = new Label(table, "---");
	}

    void add_spacer(Widget *parent, FlexLayout *parentLayout, int height) {
        Widget *spacer = new Widget(parent);
        spacer->set_min_size( Vector2i(1, height) );
        //spacer->set_max_size( Vector2i(1, height) );
        parentLayout->set_flex_item(spacer, FlexLayout::FlexItem(0.0f, 0.0f, height));
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
                                       nvgRGB(240, 242, 247), nvgRGB(220, 225, 235));
        nvgFillPaint(ctx, bg);
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
            ref<CarSalesFormApp> app = new CarSalesFormApp();
            //app->dec_ref();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop();
        }

        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::string error_msg = std::string("Caught a fatal error: ") + std::string(e.what());
        std::cerr << error_msg << std::endl;
        return -1;
    }

    return 0;
}
