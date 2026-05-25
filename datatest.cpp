/*
    datatest.cpp -- Test driver for the DataGrid widget.

    Usage:
        datatest                              # built-in synthetic 64k-row demo
        datatest --csv file.csv [--no-header] # edit a CSV file in place
        datatest --db file.db --table NAME    # edit a SQLite table in place

    Edits made in the grid are committed to the underlying source:
      * SQLite: each cell edit issues an UPDATE through the prepared statement
                in SQLiteDataAdapter (requires a primary key, which is auto-
                detected from PRAGMA table_info).
      * CSV:    edits land in memory immediately. A "Save" button writes them
                back to disk; an "Auto-save" toggle persists after every change.
*/

#include <nanogui/nanogui.h>
#include <nanogui/datagrid.h>
#include <nanogui/dataadapter.h>
#include <nanogui/csvadapter.h>
#include <nanogui/sqliteadapter.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace nanogui;

// ---------------------------------------------------------------------------
// Built-in synthetic model (used when no --csv / --db is provided)
// ---------------------------------------------------------------------------
class FakeLargeDataModel : public DataGridModel {
public:
    FakeLargeDataModel() : m_row_count(65536) {}

    size_t row_count() const override    { return m_row_count; }
    size_t column_count() const override  { return 255; }

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

    nanogui::any get_value(int row, int column) const override {
        auto it = m_overrides.find(make_key(row, column));
        if (it != m_overrides.end()) return it->second;

        int c = column % 12;
        switch (c) {
            case 0:  return (int64_t)row;
            case 1:  return std::string("Item_") + std::to_string(row % 10000);
            case 2:  return (row % 7 == 0) ? -(row * 1.37) : (row * 12.34);
            case 3:  return (int64_t)((row % 200) + 1);
            case 4:  return 9.99 + (row % 50) * 0.5;
            case 5:  return std::chrono::system_clock::now() - std::chrono::hours(row % 10000);
            case 6:  return (row % 3) != 0;
            case 7:  { static const char* cats[] = {"Hardware","Software","Services","Support","Training"};
                       return std::string(cats[row % 5]); }
            case 8:  return 50.0 + (row % 50) * 0.8;
            case 9:  return (row % 5 == 0) ? "Urgent - follow up" : "Standard order";
            case 10: return (int64_t)(row % 4);
            case 11: return ((row % 200) + 1) * (9.99 + (row % 50) * 0.5);
            default: return nanogui::any{};
        }
    }

    CellStyle get_cell_style(int row, int column) const override {
        CellStyle style;
        style.fontSize = 14.0f;
        int c = column % 12;
        if (c == 2 || c == 4 || c == 11) {
            style.number_format.symbol = "$";
            style.number_format.decimal_places = 2;
            style.number_format.negative_red = true;
            style.number_format.use_grouping = true;
            style.h_align = Alignment::Maximum;
        }
        if (c == 0 || c == 3 || c == 10) {
            style.number_format.use_grouping = true;
            style.h_align = Alignment::Maximum;
        }
        if (c == 8) { style.number_format.decimal_places = 1; style.h_align = Alignment::Maximum; }
        if (c == 6 || c == 5) style.h_align = Alignment::Middle;
        if (c == 10) {
            switch ((int64_t)(row % 4)) {
                case 0: style.fgColor = nvgRGB(40, 110, 220);  break;
                case 1: style.fgColor = nvgRGB(20, 140,  60);  break;
                case 2: style.fgColor = nvgRGB(210,130,  30);  break;
                case 3: style.fgColor = nvgRGB(200, 30,  30);  break;
            }
            style.bold = true;
        }
        return style;
    }

    bool is_cell_editable(int row, int column) const override {
        int c = column % 12;
        return c == 1 || c == 8;
    }

    bool set_value(int row, int column, const nanogui::any& value) override {
        if (!is_cell_editable(row, column)) return false;
        m_overrides[make_key(row, column)] = value;
        return true;
    }

private:
    static uint64_t make_key(int row, int column) {
        return ((uint64_t)(uint32_t)row << 32) | (uint32_t)column;
    }
    size_t                                    m_row_count;
    std::unordered_map<uint64_t, nanogui::any> m_overrides;
};

// ---------------------------------------------------------------------------
// Generic editor factories driven off the column's DataType
// ---------------------------------------------------------------------------
namespace {

DataGridColumn::EditorFactory make_editor_factory(DataType type) {
    switch (type) {
        case DataType::Integer:
            return [](DataGrid* grid, int /*row*/, int /*col*/,
                      const nanogui::any& value) -> Widget* {
                int64_t v = 0;
                if (value.has_value()) {
                    if (value.type() == typeid(int64_t))      v = nanogui::any_cast<int64_t>(value);
                    else if (value.type() == typeid(int))     v = nanogui::any_cast<int>(value);
                    else if (value.type() == typeid(double))  v = (int64_t)nanogui::any_cast<double>(value);
                    else if (value.type() == typeid(std::string)) {
                        try { v = std::stoll(nanogui::any_cast<std::string>(value)); } catch (...) {}
                    }
                }
                auto* ib = new IntBox<int64_t>(grid, v);
                ib->set_editable(true);
                ib->set_alignment(TextBox::Alignment::Right);
                ib->set_callback([grid](int64_t) { grid->end_edit(true); });
                return ib;
            };

        case DataType::Double:
        case DataType::Currency:
            return [](DataGrid* grid, int /*row*/, int /*col*/,
                      const nanogui::any& value) -> Widget* {
                double v = 0.0;
                if (value.has_value()) {
                    if (value.type() == typeid(double))       v = nanogui::any_cast<double>(value);
                    else if (value.type() == typeid(int64_t)) v = (double)nanogui::any_cast<int64_t>(value);
                    else if (value.type() == typeid(int))     v = (double)nanogui::any_cast<int>(value);
                    else if (value.type() == typeid(std::string)) {
                        try { v = std::stod(nanogui::any_cast<std::string>(value)); } catch (...) {}
                    }
                }
                auto* fb = new FloatBox<double>(grid, v);
                fb->set_editable(true);
                fb->set_alignment(TextBox::Alignment::Right);
                fb->set_callback([grid](double) { grid->end_edit(true); });
                return fb;
            };

        case DataType::Boolean:
            return [](DataGrid* grid, int /*row*/, int /*col*/,
                      const nanogui::any& value) -> Widget* {
                bool v = false;
                if (value.has_value()) {
                    if (value.type() == typeid(bool))         v = nanogui::any_cast<bool>(value);
                    else if (value.type() == typeid(int64_t)) v = nanogui::any_cast<int64_t>(value) != 0;
                    else if (value.type() == typeid(std::string)) {
                        const auto& s = nanogui::any_cast<std::string>(value);
                        v = (s == "true" || s == "1" || s == "yes");
                    }
                }
                auto* cb = new CheckBox(grid, "");
                cb->set_checked(v);
                cb->set_callback([grid](bool) { grid->end_edit(true); });
                return cb;
            };

        case DataType::String:
        case DataType::DateTime:
        case DataType::Date:
        case DataType::Time:
        default:
            return [](DataGrid* grid, int /*row*/, int /*col*/,
                      const nanogui::any& value) -> Widget* {
                std::string s = DataAdapter::any_to_string(value);
                auto* tb = new TextBox(grid, s);
                tb->set_editable(true);
                tb->set_alignment(TextBox::Alignment::Left);
                tb->set_callback([grid](const std::string&) {
                    grid->end_edit(true);
                    return true;
                });
                return tb;
            };
    }
}

// Build a column list from the model's metadata. Picks sensible widths and
// alignment from the column type and wires up a default editor per column.
std::vector<DataGridColumn> build_columns_from_model(DataGridModel* model) {
    std::vector<DataGridColumn> cols;
    if (!model) return cols;
    const int ncols = (int)model->column_count();
    cols.reserve(ncols);

    for (int i = 0; i < ncols; ++i) {
        DataGridColumn c;
        c.title = model->column_name(i);
        c.type  = model->column_type(i);
        switch (c.type) {
            case DataType::Integer:   c.width = 100; break;
            case DataType::Double:
            case DataType::Currency:  c.width = 120; break;
            case DataType::Boolean:   c.width = 70;  break;
            case DataType::DateTime:
            case DataType::Date:
            case DataType::Time:      c.width = 150; break;
            default:                  c.width = 160; break;
        }
        c.min_width        = 40;
        c.max_width        = 600;
        c.sortable         = true;
        c.editor_factory   = make_editor_factory(c.type);
        cols.push_back(std::move(c));
    }
    return cols;
}

} // namespace

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------
struct CliOptions {
    enum class Mode { Synthetic, CSV, SQLite };
    Mode        mode      = Mode::Synthetic;
    std::string csv_path;
    bool        csv_header = true;
    std::string db_path;
    std::string table_name;
};

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s                              # synthetic 64k-row demo\n"
        "  %s --csv FILE [--no-header]     # load and edit a CSV file\n"
        "  %s --db FILE --table NAME       # load and edit a SQLite table\n",
        prog, prog, prog);
}

static bool parse_cli(int argc, char** argv, CliOptions& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need_val = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", opt);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--csv") {
            const char* v = need_val("--csv"); if (!v) return false;
            out.mode = CliOptions::Mode::CSV;
            out.csv_path = v;
        } else if (a == "--no-header") {
            out.csv_header = false;
        } else if (a == "--db") {
            const char* v = need_val("--db"); if (!v) return false;
            out.mode = CliOptions::Mode::SQLite;
            out.db_path = v;
        } else if (a == "--table") {
            const char* v = need_val("--table"); if (!v) return false;
            out.table_name = v;
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return false;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", a.c_str());
            print_usage(argv[0]);
            return false;
        }
    }

    if (out.mode == CliOptions::Mode::SQLite && out.table_name.empty()) {
        std::fprintf(stderr, "--db requires --table NAME\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------
class DataGridApp {
public:
    Screen*    screen   = nullptr;
    Window*    window   = nullptr;
    DataGrid*  datagrid = nullptr;
    Label*     status   = nullptr;

    // Keep a strong ref to the model and (when applicable) a typed pointer
    // to the CSV adapter so the Save button can call save() on it.
    std::shared_ptr<DataGridModel> model;
    CSVDataAdapter*                csv_adapter = nullptr;
    bool                           auto_save   = false;

    CliOptions opts;

    explicit DataGridApp(const CliOptions& options) : opts(options) {
        const std::string title = title_for_mode();
        screen = new Screen(Vector2i(1400, 900), title);

        window = new Window(screen, title);
        window->set_position(Vector2i(20, 20));
        window->set_size(Vector2i(1360, 860));
        window->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 8, 8));

        // Toolbar
        auto* toolbar = new Widget(window);
        toolbar->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));

        auto* src_label = new Label(toolbar, source_description(), "sans-bold");
        src_label->set_font_size(14);

        // Spacer
        auto* spacer = new Widget(toolbar);
        spacer->set_fixed_size(Vector2i(20, 1));
        (void)spacer;

        if (opts.mode == CliOptions::Mode::CSV) {
            auto* save_btn = new Button(toolbar, "Save");
            save_btn->set_font_size(13);
            save_btn->set_callback([this]() { save_csv(); });

            auto* autosave_cb = new CheckBox(toolbar, "Auto-save");
            autosave_cb->set_font_size(13);
            autosave_cb->set_callback([this](bool v) { auto_save = v; });
        }

        // Grid
        datagrid = new DataGrid(window);
        datagrid->set_font_size(14);
        datagrid->set_min_height(600);

        // Model
        model = build_model();
        datagrid->set_model(model);
        datagrid->set_columns(build_columns_from_model(model.get()));
        datagrid->set_sorting_enabled(true);
        datagrid->set_filtering_enabled(true);
        datagrid->set_editing_enabled(true);

        // Persist every edit. For SQLite, set_value() already issues an UPDATE
        // inside the adapter, so we only need to handle CSV auto-save here.
        datagrid->set_cell_edited_callback(
            [this](int row, int col, const nanogui::any& new_value) {
                (void)row; (void)col; (void)new_value;
                if (auto_save && opts.mode == CliOptions::Mode::CSV)
                    save_csv();
                update_status();
            });

        // Status bar
        status = new Label(window, "");
        status->set_font_size(12);
        update_status();

        screen->set_resize_callback([this](Vector2i) {
            window->set_size(screen->size() - Vector2i(40, 40));
        });
    }

    void run() {
        window->perform_layout(screen->nvg_context());
        screen->set_visible(true);
        screen->draw_all();
        mainloop(-1);
    }

private:
    std::string title_for_mode() const {
        switch (opts.mode) {
            case CliOptions::Mode::CSV:    return "DataGrid - CSV: " + opts.csv_path;
            case CliOptions::Mode::SQLite: return "DataGrid - SQLite: " + opts.db_path + " [" + opts.table_name + "]";
            default:                       return "DataGrid - Synthetic Demo";
        }
    }

    std::string source_description() const {
        switch (opts.mode) {
            case CliOptions::Mode::CSV:    return "CSV file: " + opts.csv_path;
            case CliOptions::Mode::SQLite: return "SQLite: " + opts.db_path + "  table=" + opts.table_name;
            default:                       return "Synthetic in-memory model (65,536 rows)";
        }
    }

    std::shared_ptr<DataGridModel> build_model() {
        switch (opts.mode) {
            case CliOptions::Mode::CSV: {
                auto adapter = std::make_shared<CSVDataAdapter>(opts.csv_path, opts.csv_header);
                csv_adapter = adapter.get();
                return adapter;
            }
            case CliOptions::Mode::SQLite: {
                auto adapter = std::make_shared<SQLiteDataAdapter>(opts.db_path, opts.table_name);
                return adapter;
            }
            case CliOptions::Mode::Synthetic:
            default:
                return std::make_shared<FakeLargeDataModel>();
        }
    }

    void save_csv() {
        if (!csv_adapter) return;
        const bool ok = csv_adapter->save();
        if (status) {
            status->set_caption(ok ? ("Saved " + opts.csv_path)
                                    : ("Failed to save " + opts.csv_path));
        }
        if (screen) screen->redraw();
    }

    void update_status() {
        if (!status || !model) return;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "Rows: %zu  |  Columns: %zu  |  Double-click cell or press Enter to edit  |  "
            "Click headers to sort  |  Esc cancels edit%s",
            model->row_count(), model->column_count(),
            (opts.mode == CliOptions::Mode::CSV && auto_save) ? "  |  Auto-save ON" : "");
        status->set_caption(buf);
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    CliOptions opts;
    if (!parse_cli(argc, argv, opts))
        return 1;

    try {
        nanogui::init();
        {
            DataGridApp app(opts);
            app.run();
        }
        nanogui::shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}
