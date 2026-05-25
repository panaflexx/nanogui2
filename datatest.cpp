/*
    datagrid.cpp -- Test application for the new DataGrid widget

    Shows:
      - FolderView on the left (sidebar)
      - Large virtual DataGrid (64k rows) on the right
      - Multiple data types including Currency with Excel-style formatting
*/

#include <nanogui/nanogui.h>
#include <nanogui/datagrid.h>
#include <random>
#include <chrono>
#include <iostream>
#include <unordered_map>

using namespace nanogui;


// ---------------------------------------------------------------------------
// Fake Data Model - 65536 rows with varied types per row
// ---------------------------------------------------------------------------
class FakeLargeDataModel : public DataGridModel {
public:
    FakeLargeDataModel() {
        m_row_count = 65536;
        m_rng.seed(42); // reproducible
    }

    size_t row_count() const override { return m_row_count; }
    size_t column_count() const override { return 255; }

    std::string column_name(int column) const override {
        static const char* names[] = {
            "ID", "Name", "Amount", "Qty", "Unit Price", "Date",
            "Active", "Category", "Score", "Notes", "Status", "Total"
        };
        return std::string(names[column % 12]) + "_" + std::to_string(column / 12);
    }

    DataType column_type(int column) const override {
        static DataType types[] = {
            DataType::Integer, DataType::String, DataType::Currency,
            DataType::Integer, DataType::Currency, DataType::DateTime,
            DataType::Boolean, DataType::String, DataType::Double,
            DataType::String, DataType::Integer, DataType::Currency
        };
        return types[column % 12];
    }

    std::any get_value(int row, int column) const override {
        // Honor any user-edited override first
        auto key = make_key(row, column);
        auto it = m_overrides.find(key);
        if (it != m_overrides.end())
            return it->second;

        int c = column % 12;
        switch (c) {
            case 0: return (int64_t)row;
            case 1: return std::string("Item_") + std::to_string(row % 10000);
            case 2: {
                double val = (row % 7 == 0) ? -(row * 1.37) : (row * 12.34);
                return val;
            }
            case 3: return (int64_t)((row % 200) + 1);
            case 4: return 9.99 + (row % 50) * 0.5;
            case 5: {
                // Generate a plausible timestamp
                auto tp = std::chrono::system_clock::now() - std::chrono::hours(row % 10000);
                return tp;
            }
            case 6: return (row % 3) != 0;
            case 7: {
                static const char* cats[] = {"Hardware", "Software", "Services", "Support", "Training"};
                return std::string(cats[row % 5]);
            }
            case 8: return 50.0 + (row % 50) * 0.8;
            case 9: return (row % 5 == 0) ? "Urgent - follow up" : "Standard order";
            case 10: return (int64_t)(row % 4); // 0-3 status codes
            case 11: {
                double qty = (row % 200) + 1;
                double price = 9.99 + (row % 50) * 0.5;
                return qty * price;
            }
            default: return std::any{};
        }
    }

    CellStyle get_cell_style(int row, int column) const override {
        CellStyle style;
        style.fontSize = 14.0f;
        int c = column % 12;
        if (c == 2 || c == 4 || c == 11) {
            // Currency columns - red for negative, right-aligned, grouped
            style.number_format.symbol = "$";
            style.number_format.decimal_places = 2;
            style.number_format.negative_red = true;
            style.number_format.use_grouping = true;
            style.h_align = Alignment::Maximum;
        }
        if (c == 0 || c == 3 || c == 10) {
            // Integer columns - right-aligned, grouped
            style.number_format.use_grouping = true;
            style.h_align = Alignment::Maximum;
        }
        if (c == 8) {
            // Score (Double)
            style.number_format.decimal_places = 1;
            style.h_align = Alignment::Maximum;
        }
        if (c == 6) {
            // Boolean centered
            style.h_align = Alignment::Middle;
        }
        if (c == 5) {
            // Date centered
            style.h_align = Alignment::Middle;
        }
        if (c == 10) {
            // Status code - colorize
            int64_t code = (int64_t)(row % 4);
            switch (code) {
                case 0: style.fgColor = nvgRGB(40, 110, 220);   break; // blue
                case 1: style.fgColor = nvgRGB(20, 140, 60);    break; // green
                case 2: style.fgColor = nvgRGB(210, 130, 30);   break; // amber
                case 3: style.fgColor = nvgRGB(200, 30, 30);    break; // red
                default: break;
            }
            style.bold = true;
        }
        return style;
    }

    bool is_cell_editable(int row, int column) const override {
        int c = column % 12;
        // Allow editing of name (string) and score (double)
        return c == 1 || c == 8;
    }

    // Persist the edited value so future get_value() calls (e.g. re-draws
    // and re-opening the editor) see it.
    bool set_value(int row, int column, const std::any& value) override {
        if (!is_cell_editable(row, column))
            return false;
        m_overrides[make_key(row, column)] = value;
        return true;
    }

private:
    static uint64_t make_key(int row, int column) {
        return ((uint64_t)(uint32_t)row << 32) | (uint32_t)column;
    }

    size_t m_row_count;
    mutable std::mt19937 m_rng;
    std::unordered_map<uint64_t, std::any> m_overrides;
}
;
class DataGridApp {
public:
    Screen* screen = nullptr;
    Window* window = nullptr;
    DataGrid* datagrid = nullptr;

    DataGridApp() {
        screen = new Screen(Vector2i(1400, 900), "DataGrid Test - 64k Rows");

        window = new Window(screen, "Enterprise DataGrid Demo");
        window->set_position(Vector2i(20, 20));
        window->set_size(Vector2i(1360, 860));
        window->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Fill, 8, 8));

        // Left sidebar - FolderView style navigation
        auto* sidebar = new Widget(window);
        sidebar->set_min_width(220);
        sidebar->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 4, 4));

        auto* sidebar_label = new Label(sidebar, "Data Sources", "sans-bold");
        sidebar_label->set_font_size(15);

        // Simple folder-like items
        const char* folders[] = {
            "Sales Data", "Customer DB", "Inventory", "Financials",
            "HR Records", "Audit Logs", "Archived"
        };

        for (const char* name : folders) {
            auto* item = new Button(sidebar, name);
            item->set_font_size(14);
            item->set_min_height(28);
            item->set_callback([this, name]() {
                if (datagrid && datagrid->model()) {
                    // In a real app we would swap models here
                    screen->redraw();
                }
            });
        }

        // Main content area with the DataGrid
        auto* main_area = new Widget(window);
        main_area->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 4, 4));

        auto* header = new Label(main_area, "Virtual DataGrid — 65536 rows × 255 columns (horiz scroll test)", "sans-bold");
        header->set_font_size(16);

        // Create the DataGrid
        datagrid = new DataGrid(main_area);
        datagrid->set_font_size(14);
        datagrid->set_min_height(400);

        // Create a model that generates 64k rows of varied types
        auto model = std::make_shared<FakeLargeDataModel>();
        datagrid->set_model(model);

        // Configure columns with rich formatting
        std::vector<DataGridColumn> cols(255);

        // Configure columns with rich formatting (cycle every 12 types, repeat widths)
        static const char* titles[] = {
            "ID", "Name", "Amount", "Qty", "Unit Price", "Date",
            "Active", "Category", "Score", "Notes", "Status", "Total"
        };
        static const DataType col_types[] = {
            DataType::Integer, DataType::String, DataType::Currency,
            DataType::Integer, DataType::Currency, DataType::DateTime,
            DataType::Boolean, DataType::String, DataType::Double,
            DataType::String, DataType::Integer, DataType::Currency
        };
        static const int widths[] = {70,160,110,70,100,130,70,120,80,200,80,110};

        for (int i = 0; i < 255; ++i) {
            int t = i % 12;
            cols[i].title = std::string(titles[t]) + "_" + std::to_string(i / 12);
            cols[i].width = widths[t];
            cols[i].min_width = 40;
            cols[i].max_width = 400;
            cols[i].type = col_types[t];
            // Mark several columns as sortable to demonstrate the indicator
            cols[i].sortable = (t == 0 || t == 1 || t == 2 || t == 5 || t == 8 || t == 11);
        }

        datagrid->set_columns(cols);

        // Debug: print initial column widths (first 20)
        printf("Initial column widths (first 20): ");
        for (int i = 0; i < 20 && i < (int)cols.size(); ++i) {
            printf("%d ", cols[i].width);
        }
        printf("...\n");

        // Editor for Name column (string) - writes the new string back
        // into the model so the change persists when the editor closes.
        cols[1].editor_factory = [](DataGrid* grid, int row, int col, const std::any& value) -> Widget* {
            auto* tb = new TextBox(grid, value.has_value() ? std::any_cast<std::string>(value) : "");
            tb->set_editable(true);
            tb->set_alignment(TextBox::Alignment::Left);
            tb->set_callback([grid, row, col](const std::string& text) {
                if (auto* m = grid->model())
                    m->set_value(row, col, std::any(text));
                grid->end_edit(true);
                return true;
            });
            return tb;
        };

        // Editor for Score column (double) - same pattern: store the
        // typed value via set_value() then close the editor.
        cols[8].editor_factory = [](DataGrid* grid, int row, int col, const std::any& value) -> Widget* {
            double v = value.has_value() ? std::any_cast<double>(value) : 0.0;
            auto* fb = new FloatBox<double>(grid, v);
            fb->set_editable(true);
            fb->set_alignment(TextBox::Alignment::Right);
            fb->set_callback([grid, row, col](double newv) {
                if (auto* m = grid->model())
                    m->set_value(row, col, std::any(newv));
                grid->end_edit(true);
            });
            return fb;
        };

        datagrid->set_columns(cols); // re-apply with editors

        // Status bar
        auto* status = new Label(main_area,
            "Rows: 65,536  |  Click a cell to focus  |  Click headers to select column + cycle sort  |  "
            "Double-click cell or press Enter to edit  |  Double-click column edge to autosize  |  Esc cancels edit");
        status->set_font_size(12);

        // Keyboard help
        screen->set_resize_callback([this](Vector2i) {
            window->set_size(screen->size() - Vector2i(40, 40));
        });
    }

    void run() {
        window->perform_layout(screen->nvg_context());
        screen->set_visible(true);
        screen->draw_all();
        mainloop();
    }
};


int main(int argc, char** argv) {
    try {
        nanogui::init();
        DataGridApp app;
        app.run();
        nanogui::shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}
