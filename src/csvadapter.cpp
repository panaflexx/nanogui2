/*
    nanogui/csvadapter.cpp -- In-memory CSV-backed DataAdapter.

    Improvements over the naive comma-split implementation:

      * RFC-4180-ish parser: handles quoted fields, escaped quotes (""),
        embedded commas, embedded newlines, and CRLF line endings.
      * `save()` quotes only when needed and escapes embedded quotes; it also
        reports stream-write errors instead of silently returning success.
      * Column types are inferred from a sample of the loaded rows so numeric
        columns sort numerically rather than lexicographically.
      * Ragged rows are padded to the header width so editing never indexes
        out of bounds.
*/

#include <nanogui/csvadapter.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// CSV parsing
// ---------------------------------------------------------------------------

namespace {

// Parse one logical CSV record starting at the current position in `in`.
// A logical record may span multiple physical lines if quoted.
// Returns false at clean EOF (no record produced).
bool read_csv_record(std::istream& in, std::vector<std::string>& out) {
    out.clear();
    std::string field;
    bool in_quotes = false;
    bool any_char  = false;

    int ch;
    while ((ch = in.get()) != EOF) {
        any_char = true;
        const char c = static_cast<char>(ch);

        if (in_quotes) {
            if (c == '"') {
                // Either an escaped quote ("") or end-of-field.
                const int next = in.peek();
                if (next == '"') {
                    in.get();
                    field.push_back('"');
                } else {
                    in_quotes = false;
                }
            } else {
                field.push_back(c);
            }
            continue;
        }

        switch (c) {
            case '"':
                in_quotes = true;
                break;
            case ',':
                out.push_back(std::move(field));
                field.clear();
                break;
            case '\r': {
                // Swallow optional LF (CRLF). Either way the record ends.
                if (in.peek() == '\n') in.get();
                out.push_back(std::move(field));
                return true;
            }
            case '\n':
                out.push_back(std::move(field));
                return true;
            default:
                field.push_back(c);
                break;
        }
    }

    if (!any_char) return false;            // clean EOF, no trailing record
    out.push_back(std::move(field));        // last record without newline
    return true;
}

// True if `s` looks like a base-10 signed integer (optionally with leading +/-).
bool looks_like_integer(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') {
        if (s.size() == 1) return false;
        ++i;
    }
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// True if `s` parses as a finite floating-point number consuming all of it.
bool looks_like_double(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

bool looks_like_bool(const std::string& s) {
    if (s.size() < 4 || s.size() > 5) return false;
    std::string l(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i)
        l[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return l == "true" || l == "false";
}

// Quote a cell for CSV output. Adds surrounding quotes and doubles embedded
// quotes only if the cell contains a quote, comma, or newline. Pure-ASCII
// fields without special characters are emitted unquoted to keep diffs small.
std::string csv_quote(const std::string& s) {
    const bool needs_quote = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs_quote) return s;

    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// CSVDataAdapter
// ---------------------------------------------------------------------------

CSVDataAdapter::CSVDataAdapter(const std::string& filename, bool has_header) {
    load_csv(filename, has_header);
}

void CSVDataAdapter::load_csv(const std::string& filename, bool has_header) {
    m_filename = filename;

    // Open in binary so we control \r\n handling ourselves.
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open CSV file: " + filename);

    std::vector<std::string> record;
    bool first = true;

    while (read_csv_record(file, record)) {
        if (first && has_header) {
            m_headers = record;
            first = false;
            continue;
        }
        m_rows.push_back(record);
        first = false;
    }

    // Determine column count: prefer header width, otherwise the widest row.
    size_t cols = m_headers.size();
    if (cols == 0) {
        for (const auto& r : m_rows) cols = std::max(cols, r.size());
    }

    if (m_headers.empty()) {
        m_headers.reserve(cols);
        for (size_t i = 0; i < cols; ++i)
            m_headers.push_back("Column_" + std::to_string(i));
    }

    // Pad ragged rows so set_value() / get_value() never index OOB.
    for (auto& r : m_rows)
        if (r.size() < cols) r.resize(cols);

    // ---- Type inference (sample up to N rows) ------------------------------
    m_column_types.assign(cols, DataType::String);
    constexpr size_t kSample = 200;
    const size_t n_sample = std::min(m_rows.size(), kSample);

    for (size_t c = 0; c < cols; ++c) {
        bool all_int = true, all_double = true, all_bool = true, any_value = false;
        for (size_t r = 0; r < n_sample; ++r) {
            const std::string& v = m_rows[r][c];
            if (v.empty()) continue;
            any_value = true;
            if (all_int    && !looks_like_integer(v)) all_int    = false;
            if (all_double && !looks_like_double(v))  all_double = false;
            if (all_bool   && !looks_like_bool(v))    all_bool   = false;
            if (!all_int && !all_double && !all_bool) break;
        }
        if (!any_value)      m_column_types[c] = DataType::String;
        else if (all_int)    m_column_types[c] = DataType::Integer;
        else if (all_double) m_column_types[c] = DataType::Double;
        else if (all_bool)   m_column_types[c] = DataType::Boolean;
    }
}

// ---------------------------------------------------------------------------
// Storage interface
// ---------------------------------------------------------------------------

size_t CSVDataAdapter::storage_row_count() const {
    return m_rows.size();
}

size_t CSVDataAdapter::storage_column_count() const {
    return m_headers.size();
}

std::string CSVDataAdapter::storage_column_name(int col) const {
    if (col < 0 || col >= (int)m_headers.size()) return {};
    return m_headers[col];
}

DataType CSVDataAdapter::storage_column_type(int col) const {
    if (col < 0 || col >= (int)m_column_types.size()) return DataType::String;
    return m_column_types[col];
}

nanogui::any CSVDataAdapter::storage_get_value(int storage_row, int col) const {
    if (storage_row < 0 || storage_row >= (int)m_rows.size()) return {};
    const auto& row = m_rows[storage_row];
    if (col < 0 || col >= (int)row.size()) return {};

    // Hand back a typed value when the column type permits a clean parse.
    const std::string& s = row[col];
    if (s.empty()) return std::string();

    switch (storage_column_type(col)) {
        case DataType::Integer:
            if (looks_like_integer(s)) {
                try { return (int64_t)std::stoll(s); } catch (...) {}
            }
            break;
        case DataType::Double:
            if (looks_like_double(s)) {
                try { return std::stod(s); } catch (...) {}
            }
            break;
        case DataType::Boolean: {
            std::string l(s.size(), '\0');
            for (size_t i = 0; i < s.size(); ++i)
                l[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
            if (l == "true")  return true;
            if (l == "false") return false;
            break;
        }
        default:
            break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

bool CSVDataAdapter::insert_row(int logical_row) {
    std::vector<std::string> new_row(m_headers.size());
    if (logical_row < 0 || logical_row >= (int)m_rows.size())
        m_rows.push_back(std::move(new_row));
    else
        m_rows.insert(m_rows.begin() + logical_row, std::move(new_row));
    invalidate_view();
    return true;
}

bool CSVDataAdapter::delete_row(int logical_row) {
    if (logical_row < 0 || logical_row >= (int)m_rows.size()) return false;
    m_rows.erase(m_rows.begin() + logical_row);
    invalidate_view();
    return true;
}

bool CSVDataAdapter::storage_set_value(int storage_row, int col, const nanogui::any& v) {
    if (storage_row < 0 || storage_row >= (int)m_rows.size()) return false;
    auto& row = m_rows[storage_row];
    if (col < 0 || col >= (int)row.size()) return false;

    // Reuse the central any -> string conversion (handles ints, doubles, bool).
    std::string s = DataAdapter::any_to_string(v);
    if (!v.has_value()) s.clear();
    else if (s.empty() && v.type() != typeid(std::string) && v.type() != typeid(const char*))
        return false;  // unsupported any payload

    row[col] = std::move(s);
    return true;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

bool CSVDataAdapter::save(const std::string& filename) const {
    const std::string out_path = filename.empty() ? m_filename : filename;
    std::ofstream file(out_path, std::ios::binary);
    if (!file.is_open()) return false;

    // Header
    for (size_t c = 0; c < m_headers.size(); ++c) {
        if (c) file.put(',');
        file << csv_quote(m_headers[c]);
    }
    file.put('\n');

    // Rows
    for (const auto& row : m_rows) {
        for (size_t c = 0; c < row.size(); ++c) {
            if (c) file.put(',');
            file << csv_quote(row[c]);
        }
        file.put('\n');
    }

    file.flush();
    return file.good();
}

NAMESPACE_END(nanogui)
