/*
    nanogui/csvadapter.h -- CSV file DataAdapter
*/

#pragma once

#include <nanogui/dataadapter.h>
#include <string>
#include <vector>

NAMESPACE_BEGIN(nanogui)

class NANOGUI_EXPORT CSVDataAdapter : public DataAdapter {
public:
    explicit CSVDataAdapter(const std::string& filename, bool has_header = true);
    ~CSVDataAdapter() override = default;

    // Mutation support (in-memory only unless you add save())
    bool supports_insert() const override { return true; }
    bool supports_delete() const override { return true; }

    bool insert_row(int logical_row = -1) override;
    bool delete_row(int logical_row) override;

    bool storage_set_value(int storage_row, int col, const nanogui::any& v) override;

    // Optional: persist changes back to disk
    bool save(const std::string& filename = "") const;

protected:
    size_t      storage_row_count() const override;
    size_t      storage_column_count() const override;
    std::string storage_column_name(int col) const override;
    DataType    storage_column_type(int col) const override;

    nanogui::any storage_get_value(int storage_row, int col) const override;
    bool         storage_is_editable(int storage_row, int col) const override { return true; }

private:
    void load_csv(const std::string& filename, bool has_header);

    std::vector<std::string>              m_headers;
    std::vector<std::vector<std::string>> m_rows;       // raw string storage
    std::vector<DataType>                 m_column_types;
    std::string                           m_filename;
};

NAMESPACE_END(nanogui)
