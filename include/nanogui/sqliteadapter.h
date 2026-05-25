/*
    nanogui/sqliteadapter.h -- SQLite-backed DataAdapter
*/

#pragma once

#include <nanogui/dataadapter.h>
#include <sqlite3.h>
#include <string>
#include <vector>
#include <cstdint>

NAMESPACE_BEGIN(nanogui)

class NANOGUI_EXPORT SQLiteDataAdapter : public DataAdapter {
public:
    // Open a table directly (SELECT * FROM table_name)
    SQLiteDataAdapter(const std::string& db_path, const std::string& table_name);

    // Use a custom query (must not contain ORDER BY / LIMIT)
    SQLiteDataAdapter(const std::string& db_path,
                      const std::string& query,
                      const std::vector<std::string>& column_names);

    ~SQLiteDataAdapter() override;

    sqlite3* handle() const { return m_db; }

    // Tell the adapter which column(s) form the primary key (required for UPDATE/DELETE)
    void set_primary_key(const std::vector<std::string>& pk_columns);
    void set_primary_key(const std::string& single_pk_column);

    // Mutation support
    bool supports_insert() const override { return true; }
    bool supports_delete() const override { return true; }

    bool insert_row(int logical_row = -1) override;
    bool delete_row(int logical_row) override;

    bool set_value(int row, int column, const nanogui::any& value) override;
    bool set_values(const std::vector<CellChange>& changes) override;

    // SQLite handles the ORDER BY natively; the base class then skips its
    // (catastrophically slow on large datasets) in-memory sort.
    void sort(int column, bool ascending) override;

protected:
    bool storage_handles_sorting() const override { return true; }
    size_t      storage_row_count() const override;
    size_t      storage_column_count() const override;
    std::string storage_column_name(int col) const override;
    DataType    storage_column_type(int col) const override;

    nanogui::any storage_get_value(int storage_row, int col) const override;
    bool         storage_is_editable(int storage_row, int col) const override;
    bool         storage_set_value(int storage_row, int col, const nanogui::any& v) override;

    void on_rows_changed() override;

private:
    void open_database(const std::string& db_path);
    void discover_schema_from_table(const std::string& table);
    void prepare_statements();
    void rebuild_select_statement() const;  // (re)prepares m_select_stmt using current sort

    DataType map_sqlite_type(const char* decl_type) const;

    sqlite3*     m_db = nullptr;
    std::string  m_table;
    std::string  m_base_query;

    std::vector<std::string> m_column_names;
    std::vector<DataType>    m_column_types;

    std::vector<std::string> m_primary_key_columns;
    std::vector<int>         m_pk_column_indices;

    // Current sort state -- folded into the SELECT via ORDER BY.
    mutable int   m_sort_col       = -1;     // -1 => no ORDER BY (natural order)
    mutable bool  m_sort_ascending = true;

    // Prepared statements
    mutable sqlite3_stmt* m_count_stmt  = nullptr;
    mutable sqlite3_stmt* m_select_stmt = nullptr;
    mutable sqlite3_stmt* m_insert_stmt = nullptr;
    mutable sqlite3_stmt* m_delete_stmt = nullptr;

    // Update statement is cached lazily, keyed by the column being modified.
    mutable int           m_update_col  = -1;
    mutable sqlite3_stmt* m_update_stmt = nullptr;

    mutable int64_t m_cached_row_count = -1;
    mutable bool    m_count_unknown    = false;  // true when COUNT(*) failed; we then
                                                 // grow the estimate as windows succeed
    mutable int64_t m_row_count_estimate = 0;    // best lower bound while unknown

    // ---- Row window cache --------------------------------------------------
    // To avoid the O(N) `LIMIT 1 OFFSET row` penalty on every cell access, we
    // fetch a contiguous block of rows on demand and cache it in memory.
    static constexpr int k_window_size = 256;

    mutable int64_t                              m_window_offset = -1;  // -1 => empty
    mutable std::vector<std::vector<nanogui::any>> m_window_rows;       // [row][col]

    void  invalidate_cache() const;
    void  load_window(int64_t offset) const;
    const std::vector<nanogui::any>* row_in_window(int storage_row) const;

    // Helper: safely quote an SQL identifier (table / column name).
    static std::string quote_identifier(const std::string& ident);
};

NAMESPACE_END(nanogui)
