/*
    nanogui/sqliteadapter.cpp -- SQLite-backed DataAdapter.

    Hardened against the most common issues:

      * Identifiers (table / column names) are quoted with SQLite's `%w` format
        specifier, defeating SQL injection through unsanitized names.
      * `sqlite3_prepare_v2` failures are diagnosed and surfaced as exceptions.
      * Row reads use a windowed cache (k_window_size rows per fetch) so
        rendering N rows costs roughly N / k_window_size full-table scans
        instead of N independent OFFSET seeks.
      * UPDATE / DELETE are implemented using primary-key columns supplied via
        `set_primary_key`. `set_values` wraps batched edits in a single
        transaction for atomicity and speed.
      * Cache invalidation is centralized.

    Concurrency: instances are NOT thread-safe. Use one SQLiteDataAdapter per
    thread (and ideally per sqlite3* handle).
*/

#include <nanogui/sqliteadapter.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
//
// The adapter writes diagnostic messages to stderr by default. Set the
// environment variable NANOGUI_SQLITE_VERBOSE=0 to silence informational
// chatter (errors are always printed).

namespace {

bool sqlite_verbose() {
    static const bool v = []{
        const char* e = std::getenv("NANOGUI_SQLITE_VERBOSE");
        return !e || std::string(e) != "0";
    }();
    return v;
}

void log_info(const char* fmt, ...) {
    if (!sqlite_verbose()) return;
    std::fputs("[sqlite] ", stderr);
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

void log_err(sqlite3* db, const char* ctx) {
    std::fprintf(stderr, "[sqlite][error] %s: %s (code=%d)\n",
                 ctx,
                 db ? sqlite3_errmsg(db) : "(no db)",
                 db ? sqlite3_extended_errcode(db) : -1);
}

// Walltime in seconds (monotonic enough for progress reporting).
double now_s() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SQLiteDataAdapter::SQLiteDataAdapter(const std::string& db_path,
                                     const std::string& table_name)
    : m_table(table_name)
{
    open_database(db_path);
    discover_schema_from_table(table_name);
    prepare_statements();
}

SQLiteDataAdapter::SQLiteDataAdapter(const std::string& db_path,
                                     const std::string& query,
                                     const std::vector<std::string>& column_names)
    : m_base_query(query), m_column_names(column_names)
{
    open_database(db_path);
    m_column_types.resize(column_names.size(), DataType::String);
    prepare_statements();
}

SQLiteDataAdapter::~SQLiteDataAdapter() {
    if (m_count_stmt)  sqlite3_finalize(m_count_stmt);
    if (m_select_stmt) sqlite3_finalize(m_select_stmt);
    if (m_update_stmt) sqlite3_finalize(m_update_stmt);
    if (m_insert_stmt) sqlite3_finalize(m_insert_stmt);
    if (m_delete_stmt) sqlite3_finalize(m_delete_stmt);
    if (m_db)          sqlite3_close(m_db);
}

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

void SQLiteDataAdapter::open_database(const std::string& db_path) {
    log_info("opening database '%s'", db_path.c_str());

    // sqlite3_open() yields a handle even on failure so we can get errmsg.
    if (sqlite3_open(db_path.c_str(), &m_db) != SQLITE_OK) {
        std::string err = m_db ? sqlite3_errmsg(m_db) : "out of memory";
        log_err(m_db, "sqlite3_open");
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        throw std::runtime_error("Failed to open SQLite database: " + err);
    }

    // Tuning that helps large/read-heavy databases without changing semantics.
    char* err = nullptr;
    sqlite3_exec(m_db, "PRAGMA temp_store = MEMORY;",  nullptr, nullptr, &err);
    if (err) { sqlite3_free(err); err = nullptr; }
    sqlite3_exec(m_db, "PRAGMA cache_size  = -65536;", nullptr, nullptr, &err); // ~64 MiB
    if (err) { sqlite3_free(err); err = nullptr; }

    log_info("sqlite library version %s", sqlite3_libversion());
}

std::string SQLiteDataAdapter::quote_identifier(const std::string& ident) {
    // %w escapes embedded double quotes by doubling them; safe for identifiers.
    char* q = sqlite3_mprintf("\"%w\"", ident.c_str());
    if (!q) throw std::bad_alloc();
    std::string out(q);
    sqlite3_free(q);
    return out;
}

void SQLiteDataAdapter::discover_schema_from_table(const std::string& table) {
    // PRAGMA does not bind values, so we must quote the identifier ourselves.
    const std::string qtable = quote_identifier(table);
    const std::string sql    = "PRAGMA table_info(" + qtable + ");";

    log_info("discovering schema for table %s", qtable.c_str());

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        log_err(m_db, "prepare PRAGMA table_info");
        throw std::runtime_error(std::string("table_info: ") + sqlite3_errmsg(m_db));
    }

    constexpr int kColName = 1;
    constexpr int kColType = 2;
    constexpr int kColPk   = 5;

    std::vector<std::string> discovered_pk;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, kColName));
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, kColType));
        const int   pk   = sqlite3_column_int(stmt, kColPk);

        const std::string col_name = name ? name : "";
        m_column_names.emplace_back(col_name);
        m_column_types.push_back(map_sqlite_type(type));
        if (pk > 0) discovered_pk.push_back(col_name);
    }
    sqlite3_finalize(stmt);

    if (m_column_names.empty()) {
        log_err(m_db, ("table '" + table + "' missing").c_str());
        throw std::runtime_error("Table '" + table + "' has no columns or does not exist.");
    }

    m_base_query = "SELECT * FROM " + qtable;

    if (!discovered_pk.empty()) {
        std::string pk_join;
        for (size_t i = 0; i < discovered_pk.size(); ++i) {
            if (i) pk_join += ", ";
            pk_join += discovered_pk[i];
        }
        log_info("detected %zu column(s); primary key: %s",
                 m_column_names.size(), pk_join.c_str());
        set_primary_key(discovered_pk);
    } else {
        log_info("detected %zu column(s); no primary key (table will be read-only)",
                 m_column_names.size());
    }
}

DataType SQLiteDataAdapter::map_sqlite_type(const char* decl_type) const {
    if (!decl_type) return DataType::String;
    std::string t(decl_type);
    for (char& c : t)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (t.find("INT")  != std::string::npos) return DataType::Integer;
    if (t.find("REAL") != std::string::npos ||
        t.find("FLOA") != std::string::npos ||
        t.find("DOUB") != std::string::npos) return DataType::Double;
    if (t.find("BOOL") != std::string::npos) return DataType::Boolean;
    if (t.find("DATE") != std::string::npos ||
        t.find("TIME") != std::string::npos) return DataType::DateTime;
    if (t.find("BLOB") != std::string::npos) return DataType::Blob;
    return DataType::String;
}

void SQLiteDataAdapter::set_primary_key(const std::vector<std::string>& pk_columns) {
    m_primary_key_columns = pk_columns;
    m_pk_column_indices.clear();
    for (const auto& name : pk_columns) {
        auto it = std::find(m_column_names.begin(), m_column_names.end(), name);
        if (it != m_column_names.end())
            m_pk_column_indices.push_back((int)std::distance(m_column_names.begin(), it));
    }
    // Cached UPDATE statement is keyed on PK structure -- drop it.
    if (m_update_stmt) {
        sqlite3_finalize(m_update_stmt);
        m_update_stmt = nullptr;
        m_update_col  = -1;
    }
}

void SQLiteDataAdapter::set_primary_key(const std::string& single_pk_column) {
    set_primary_key(std::vector<std::string>{single_pk_column});
}

void SQLiteDataAdapter::prepare_statements() {
    // For a plain table (no custom query), count directly on the table; this
    // lets SQLite use its b-tree shortcut without materializing a derived
    // subquery, which matters enormously for multi-GB tables.
    const std::string count_sql = !m_table.empty()
        ? ("SELECT COUNT(*) FROM " + quote_identifier(m_table))
        : ("SELECT COUNT(*) FROM (" + m_base_query + ")");

    log_info("prepare count : %s", count_sql.c_str());

    if (sqlite3_prepare_v2(m_db, count_sql.c_str(), -1, &m_count_stmt, nullptr) != SQLITE_OK) {
        log_err(m_db, "prepare count");
        throw std::runtime_error(std::string("prepare count: ") + sqlite3_errmsg(m_db));
    }

    rebuild_select_statement();
}

void SQLiteDataAdapter::rebuild_select_statement() const {
    // Drop any previously prepared SELECT so we can rebuild around the new
    // ORDER BY clause (or lack of one).
    if (m_select_stmt) {
        sqlite3_finalize(m_select_stmt);
        m_select_stmt = nullptr;
    }

    std::string select_sql = m_base_query;
    if (m_sort_col >= 0 && m_sort_col < (int)m_column_names.size()) {
        select_sql += " ORDER BY " + quote_identifier(m_column_names[m_sort_col])
                   +  (m_sort_ascending ? " ASC" : " DESC");
    }
    select_sql += " LIMIT ? OFFSET ?";

    log_info("prepare select: %s", select_sql.c_str());

    if (sqlite3_prepare_v2(m_db, select_sql.c_str(), -1, &m_select_stmt, nullptr) != SQLITE_OK) {
        log_err(m_db, "prepare select");
        throw std::runtime_error(std::string("prepare select: ") + sqlite3_errmsg(m_db));
    }
}

void SQLiteDataAdapter::sort(int column, bool ascending) {
    if (column == m_sort_col && ascending == m_sort_ascending) {
        // No actual change -- still let the base class refresh its view.
        DataAdapter::sort(column, ascending);
        return;
    }

    m_sort_col       = column;
    m_sort_ascending = ascending;

    if (column >= 0 && column < (int)m_column_names.size()) {
        log_info("sort: ORDER BY %s %s (native)",
                 m_column_names[column].c_str(),
                 ascending ? "ASC" : "DESC");
    } else {
        log_info("sort: cleared (natural order)");
    }

    // Re-prepare the SELECT with the new ORDER BY and drop any cached rows --
    // their indices no longer match the sorted order.
    rebuild_select_statement();
    invalidate_cache();

    // Propagate to the base so it invalidates its view-index map. Note: the
    // base will NOT do its own std::sort, because storage_handles_sorting()
    // returns true.
    DataAdapter::sort(column, ascending);
}

// ---------------------------------------------------------------------------
// Row count
// ---------------------------------------------------------------------------

size_t SQLiteDataAdapter::storage_row_count() const {
    if (m_cached_row_count >= 0) return (size_t)m_cached_row_count;
    if (m_count_unknown)         return (size_t)m_row_count_estimate;

    if (!m_count_stmt) {
        log_err(m_db, "row_count: no prepared count statement");
        return 0;
    }

    log_info("counting rows (this may take a while on very large tables)...");
    const double t0 = now_s();

    sqlite3_reset(m_count_stmt);
    const int rc = sqlite3_step(m_count_stmt);
    if (rc == SQLITE_ROW) {
        m_cached_row_count = sqlite3_column_int64(m_count_stmt, 0);
        log_info("row count = %lld  (%.2fs)",
                 (long long)m_cached_row_count, now_s() - t0);
        return (size_t)m_cached_row_count;
    }

    // COUNT(*) failed -- typically SQLITE_CORRUPT on a damaged database.
    // We fall back to "unknown": the grid will scroll through whatever rows
    // SELECT can actually return, and the estimate grows as windows load.
    log_err(m_db, "row_count: sqlite3_step failed -- falling back to incremental discovery");
    m_count_unknown      = true;
    m_row_count_estimate = k_window_size * 4;  // initial guess to allow scrolling
    return (size_t)m_row_count_estimate;
}

size_t SQLiteDataAdapter::storage_column_count() const {
    return m_column_names.size();
}

std::string SQLiteDataAdapter::storage_column_name(int col) const {
    if (col < 0 || col >= (int)m_column_names.size()) return {};
    return m_column_names[col];
}

DataType SQLiteDataAdapter::storage_column_type(int col) const {
    if (col < 0 || col >= (int)m_column_types.size()) return DataType::String;
    return m_column_types[col];
}

// ---------------------------------------------------------------------------
// Row window cache
// ---------------------------------------------------------------------------

void SQLiteDataAdapter::invalidate_cache() const {
    m_window_offset = -1;
    m_window_rows.clear();
    m_cached_row_count = -1;
}

void SQLiteDataAdapter::load_window(int64_t offset) const {
    m_window_rows.clear();
    m_window_offset = offset;
    if (!m_select_stmt) {
        log_err(m_db, "load_window: no prepared select statement");
        return;
    }

    const double t0 = now_s();

    if (sqlite3_reset(m_select_stmt) != SQLITE_OK)
        log_err(m_db, "load_window: reset");
    if (sqlite3_bind_int64(m_select_stmt, 1, k_window_size) != SQLITE_OK)
        log_err(m_db, "load_window: bind LIMIT");
    if (sqlite3_bind_int64(m_select_stmt, 2, offset) != SQLITE_OK)
        log_err(m_db, "load_window: bind OFFSET");

    const int ncols = (int)m_column_names.size();
    int rc;
    while ((rc = sqlite3_step(m_select_stmt)) == SQLITE_ROW) {
        std::vector<nanogui::any> row;
        row.reserve(ncols);
        for (int c = 0; c < ncols; ++c) {
            switch (sqlite3_column_type(m_select_stmt, c)) {
                case SQLITE_INTEGER:
                    row.emplace_back((int64_t)sqlite3_column_int64(m_select_stmt, c));
                    break;
                case SQLITE_FLOAT:
                    row.emplace_back(sqlite3_column_double(m_select_stmt, c));
                    break;
                case SQLITE_TEXT: {
                    const unsigned char* txt = sqlite3_column_text(m_select_stmt, c);
                    row.emplace_back(std::string(reinterpret_cast<const char*>(txt ? txt : (const unsigned char*)"")));
                    break;
                }
                case SQLITE_BLOB:
                case SQLITE_NULL:
                default:
                    row.emplace_back(nanogui::any{});
                    break;
            }
        }
        m_window_rows.push_back(std::move(row));
    }

    if (rc != SQLITE_DONE) {
        log_err(m_db, "load_window: sqlite3_step");
    }

    const double dt = now_s() - t0;
    if (dt > 0.25 || m_window_rows.empty()) {
        log_info("window load: offset=%lld rows=%zu (%.2fs)",
                 (long long)offset, m_window_rows.size(), dt);
    }

    // Incremental row-count discovery when COUNT(*) is unreliable.
    if (m_count_unknown) {
        const int64_t loaded_end = offset + (int64_t)m_window_rows.size();
        if ((int)m_window_rows.size() < k_window_size) {
            // Hit the end (or unreadable region) -- pin the count.
            if (m_cached_row_count < 0) {
                m_cached_row_count = loaded_end;
                log_info("end of readable data: row count pinned to %lld",
                         (long long)m_cached_row_count);
            }
        } else if (loaded_end > m_row_count_estimate - k_window_size) {
            // Approaching the current estimate -- grow it so the grid
            // remains scrollable past where we've already verified data.
            m_row_count_estimate = loaded_end + k_window_size * 4;
        }
    }
}

const std::vector<nanogui::any>*
SQLiteDataAdapter::row_in_window(int storage_row) const {
    if (storage_row < 0) return nullptr;
    if (m_window_offset < 0 ||
        storage_row < m_window_offset ||
        storage_row >= m_window_offset + (int64_t)m_window_rows.size())
    {
        // Align the window to a multiple of k_window_size for predictable reuse.
        const int64_t aligned = (storage_row / k_window_size) * k_window_size;
        load_window(aligned);
    }
    const size_t idx = (size_t)(storage_row - m_window_offset);
    if (idx >= m_window_rows.size()) return nullptr;
    return &m_window_rows[idx];
}

nanogui::any SQLiteDataAdapter::storage_get_value(int storage_row, int col) const {
    if (col < 0 || col >= (int)m_column_names.size()) return {};
    const auto* row = row_in_window(storage_row);
    if (!row || col >= (int)row->size()) return {};
    return (*row)[col];
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

bool SQLiteDataAdapter::storage_is_editable(int, int) const {
    return !m_primary_key_columns.empty() && !m_table.empty();
}

namespace {

// Bind a nanogui::any value to a sqlite3_stmt parameter.
// Returns true on success; on failure returns false and leaves stmt errored.
bool bind_any(sqlite3_stmt* stmt, int idx, const nanogui::any& v) {
    if (!v.has_value())
        return sqlite3_bind_null(stmt, idx) == SQLITE_OK;

    if (v.type() == typeid(std::string)) {
        const auto& s = nanogui::any_cast<std::string>(v);
        return sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT) == SQLITE_OK;
    }
    if (v.type() == typeid(const char*)) {
        const char* s = nanogui::any_cast<const char*>(v);
        return sqlite3_bind_text(stmt, idx, s ? s : "", -1, SQLITE_TRANSIENT) == SQLITE_OK;
    }
    if (v.type() == typeid(int))
        return sqlite3_bind_int(stmt, idx, nanogui::any_cast<int>(v)) == SQLITE_OK;
    if (v.type() == typeid(int64_t))
        return sqlite3_bind_int64(stmt, idx, nanogui::any_cast<int64_t>(v)) == SQLITE_OK;
    if (v.type() == typeid(long))
        return sqlite3_bind_int64(stmt, idx, (int64_t)nanogui::any_cast<long>(v)) == SQLITE_OK;
    if (v.type() == typeid(long long))
        return sqlite3_bind_int64(stmt, idx, (int64_t)nanogui::any_cast<long long>(v)) == SQLITE_OK;
    if (v.type() == typeid(double))
        return sqlite3_bind_double(stmt, idx, nanogui::any_cast<double>(v)) == SQLITE_OK;
    if (v.type() == typeid(float))
        return sqlite3_bind_double(stmt, idx, (double)nanogui::any_cast<float>(v)) == SQLITE_OK;
    if (v.type() == typeid(bool))
        return sqlite3_bind_int(stmt, idx, nanogui::any_cast<bool>(v) ? 1 : 0) == SQLITE_OK;

    return false;
}

} // namespace

bool SQLiteDataAdapter::storage_set_value(int storage_row, int col, const nanogui::any& v) {
    if (m_table.empty() || m_primary_key_columns.empty()) return false;
    if (col < 0 || col >= (int)m_column_names.size())     return false;

    // Need the current row contents (specifically PK values) to target the
    // correct database row. Pull them from the window cache.
    const auto* row = row_in_window(storage_row);
    if (!row) return false;

    // (Re)prepare an UPDATE statement keyed by this column.
    if (m_update_col != col || !m_update_stmt) {
        if (m_update_stmt) { sqlite3_finalize(m_update_stmt); m_update_stmt = nullptr; }

        std::string sql = "UPDATE " + quote_identifier(m_table)
                        + " SET "   + quote_identifier(m_column_names[col]) + " = ?";
        sql += " WHERE ";
        for (size_t i = 0; i < m_primary_key_columns.size(); ++i) {
            if (i) sql += " AND ";
            sql += quote_identifier(m_primary_key_columns[i]) + " = ?";
        }
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &m_update_stmt, nullptr) != SQLITE_OK)
            return false;
        m_update_col = col;
    }

    sqlite3_reset(m_update_stmt);
    sqlite3_clear_bindings(m_update_stmt);

    if (!bind_any(m_update_stmt, 1, v))
        return false;

    int param = 2;
    for (int pk_idx : m_pk_column_indices) {
        if (pk_idx < 0 || pk_idx >= (int)row->size())
            return false;
        if (!bind_any(m_update_stmt, param++, (*row)[pk_idx]))
            return false;
    }

    if (sqlite3_step(m_update_stmt) != SQLITE_DONE)
        return false;

    invalidate_cache();
    return true;
}

bool SQLiteDataAdapter::insert_row(int /*logical_row*/) {
    if (m_table.empty()) return false;
    const std::string sql = "INSERT INTO " + quote_identifier(m_table) + " DEFAULT VALUES";

    char* err = nullptr;
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    invalidate_cache();
    invalidate_view();
    return true;
}

bool SQLiteDataAdapter::delete_row(int logical_row) {
    if (m_table.empty() || m_primary_key_columns.empty()) return false;

    // logical_row is post sort/filter -- but DataAdapter doesn't expose the
    // mapping here. Delete via storage row directly: callers should pass a
    // storage_row, or we just refuse if no consistent mapping is available.
    const auto* row = row_in_window(logical_row);
    if (!row) return false;

    std::string sql = "DELETE FROM " + quote_identifier(m_table) + " WHERE ";
    for (size_t i = 0; i < m_primary_key_columns.size(); ++i) {
        if (i) sql += " AND ";
        sql += quote_identifier(m_primary_key_columns[i]) + " = ?";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool ok = true;
    int param = 1;
    for (int pk_idx : m_pk_column_indices) {
        if (pk_idx < 0 || pk_idx >= (int)row->size()) { ok = false; break; }
        if (!bind_any(stmt, param++, (*row)[pk_idx])) { ok = false; break; }
    }
    if (ok && sqlite3_step(stmt) != SQLITE_DONE) ok = false;

    sqlite3_finalize(stmt);
    if (ok) {
        invalidate_cache();
        invalidate_view();
    }
    return ok;
}

bool SQLiteDataAdapter::set_value(int row, int column, const nanogui::any& value) {
    return DataAdapter::set_value(row, column, value);
}

bool SQLiteDataAdapter::set_values(const std::vector<CellChange>& changes) {
    if (changes.empty()) return true;

    // Wrap the whole batch in a transaction for atomicity + speed.
    char* err = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }

    bool all_ok = true;
    for (const auto& ch : changes) {
        if (!DataAdapter::set_value(ch.row, ch.column, ch.new_value)) {
            all_ok = false;
            break;
        }
    }

    const char* finish = all_ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(m_db, finish, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        all_ok = false;
    }

    invalidate_cache();
    invalidate_view();
    return all_ok;
}

void SQLiteDataAdapter::on_rows_changed() {
    invalidate_cache();
    invalidate_view();
}

NAMESPACE_END(nanogui)
