/*
    nanogui/dataadapter.h -- Base adapter class with built-in sort/filter support

    Provides a filtered + sorted view over an underlying data store.
    Concrete adapters (SQLite, CSV, JSON, etc.) implement the storage_* methods.
*/

#pragma once

#include <nanogui/datagrid.h>
#include <functional>
#include <string>
#include <vector>

NAMESPACE_BEGIN(nanogui)

class NANOGUI_EXPORT DataAdapter : public DataGridModel {
public:
    DataAdapter();
    virtual ~DataAdapter() = default;

    // --- DataGridModel interface (implemented using the view mapping) ---
    size_t      row_count() const override;
    size_t      column_count() const override;
    std::string column_name(int column) const override;
    DataType    column_type(int column) const override;

    nanogui::any get_value(int row, int column) const override;
    CellStyle    get_cell_style(int row, int column) const override;

    bool is_cell_editable(int row, int column) const override;
    bool set_value(int row, int column, const nanogui::any& value) override;

    // Sorting / Filtering
    bool supports_sorting() const override { return true; }
    void sort(int column, bool ascending) override;

    bool supports_filtering() const override { return true; }
    void set_filter(const std::string& text) override;

    // Optional batch update (good for paste)
    struct CellChange {
        int          row;
        int          column;
        nanogui::any new_value;
    };
    virtual bool set_values(const std::vector<CellChange>& changes);

    // Row mutation (optional)
    virtual bool supports_insert() const { return false; }
    virtual bool supports_delete() const { return false; }
    virtual bool insert_row(int logical_row = -1) { return false; }
    virtual bool delete_row(int logical_row) { return false; }

    // Force view rebuild (call after external data change)
    void invalidate_view();

    // Convert an `any` value to its textual representation (for filtering / display).
    // Handles std::string, const char*, integral, floating-point, bool. Returns "" for unset.
    static std::string any_to_string(const nanogui::any& v);

protected:
    // --- Methods that concrete adapters must implement ---
    virtual size_t      storage_row_count() const = 0;
    virtual size_t      storage_column_count() const = 0;
    virtual std::string storage_column_name(int col) const = 0;
    virtual DataType    storage_column_type(int col) const = 0;

    virtual nanogui::any storage_get_value(int storage_row, int col) const = 0;
    virtual CellStyle    storage_get_cell_style(int storage_row, int col) const { return CellStyle{}; }

    virtual bool storage_is_editable(int storage_row, int col) const { return false; }
    virtual bool storage_set_value(int storage_row, int col, const nanogui::any& v) { return false; }

    // Optional: return true if the row matches the current filter
    virtual bool row_matches_filter(int storage_row, const std::string& filter) const;

    // Optional: called after insert/delete so adapter can sync (e.g., reload row count)
    virtual void on_rows_changed() {}

    // Return true if the underlying storage already returns rows in the order
    // requested by `sort()` (e.g. by issuing `ORDER BY` in SQL). When true, the
    // base class will NOT perform an in-memory sort of the view indices --
    // which avoids O(N log N) random `storage_get_value` calls thrashing any
    // window cache the backend keeps.
    virtual bool storage_handles_sorting() const { return false; }

private:
    void rebuild_view() const;
    void ensure_view() const {
        if (m_view_dirty) rebuild_view();
    }

    std::string m_filter_text;
    int         m_sort_column = -1;
    bool        m_sort_ascending = true;

    // Maps logical row -> storage row. Mutable: const accessors lazily build the view.
    mutable std::vector<int> m_view_indices;
    mutable bool             m_view_dirty = true;
};

NAMESPACE_END(nanogui)
