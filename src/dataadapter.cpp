/*
    nanogui/dataadapter.cpp -- Sortable / filterable view over a storage backend.

    Notes:
      - View state is `mutable`; const accessors lazily rebuild the index map.
      - Sort comparator establishes a total order across types so std::sort
        never sees a non-strict-weak-ordering predicate (which would be UB).
      - Filter matching stringifies numeric / bool / text values uniformly.
*/

#include <nanogui/dataadapter.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace {

inline char ascii_tolower(char c) {
    // Cast through unsigned char to avoid UB on negative char values.
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline void to_lower_inplace(std::string& s) {
    for (char& c : s) c = ascii_tolower(c);
}

// Rank used to establish a total order between any-values of differing types.
// Lower rank sorts first when types differ.
enum class TypeRank : int {
    Null    = 0,
    Bool    = 1,
    Integer = 2,
    Double  = 3,
    String  = 4,
    Other   = 5
};

TypeRank rank_of(const nanogui::any& v) {
    if (!v.has_value())                     return TypeRank::Null;
    if (v.type() == typeid(bool))           return TypeRank::Bool;
    if (v.type() == typeid(int)   ||
        v.type() == typeid(int64_t) ||
        v.type() == typeid(long)  ||
        v.type() == typeid(long long))      return TypeRank::Integer;
    if (v.type() == typeid(float) ||
        v.type() == typeid(double))         return TypeRank::Double;
    if (v.type() == typeid(std::string) ||
        v.type() == typeid(const char*))    return TypeRank::String;
    return TypeRank::Other;
}

// Extract a numeric (double) view of int / double / bool if possible.
bool as_number(const nanogui::any& v, double& out) {
    if (v.type() == typeid(bool))     { out = nanogui::any_cast<bool>(v) ? 1.0 : 0.0;            return true; }
    if (v.type() == typeid(int))      { out = (double)nanogui::any_cast<int>(v);                 return true; }
    if (v.type() == typeid(long))     { out = (double)nanogui::any_cast<long>(v);                return true; }
    if (v.type() == typeid(long long)){ out = (double)nanogui::any_cast<long long>(v);           return true; }
    if (v.type() == typeid(int64_t))  { out = (double)nanogui::any_cast<int64_t>(v);             return true; }
    if (v.type() == typeid(float))    { out = (double)nanogui::any_cast<float>(v);               return true; }
    if (v.type() == typeid(double))   { out = nanogui::any_cast<double>(v);                      return true; }
    return false;
}

// Total-order less-than that respects type ranks. Guarantees strict weak ordering.
bool any_less(const nanogui::any& a, const nanogui::any& b) {
    TypeRank ra = rank_of(a), rb = rank_of(b);
    if (ra != rb) return (int)ra < (int)rb;

    switch (ra) {
        case TypeRank::Null:
            return false;
        case TypeRank::Bool:
            return (int)nanogui::any_cast<bool>(a) < (int)nanogui::any_cast<bool>(b);
        case TypeRank::Integer:
        case TypeRank::Double: {
            double da = 0, db = 0;
            as_number(a, da);
            as_number(b, db);
            return da < db;
        }
        case TypeRank::String: {
            const std::string sa = DataAdapter::any_to_string(a);
            const std::string sb = DataAdapter::any_to_string(b);
            return sa < sb;
        }
        case TypeRank::Other:
        default:
            // Fall back to comparing type_info names so the predicate is still
            // a total order even for unknown types.
            return std::strcmp(a.type().name(), b.type().name()) < 0;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// DataAdapter
// ---------------------------------------------------------------------------

DataAdapter::DataAdapter() = default;

std::string DataAdapter::any_to_string(const nanogui::any& v) {
    if (!v.has_value()) return {};

    if (v.type() == typeid(std::string))   return nanogui::any_cast<std::string>(v);
    if (v.type() == typeid(const char*)) {
        const char* s = nanogui::any_cast<const char*>(v);
        return s ? std::string(s) : std::string();
    }
    if (v.type() == typeid(bool))          return nanogui::any_cast<bool>(v) ? "true" : "false";

    double d = 0;
    if (as_number(v, d)) {
        // Integral types: format without a trailing ".0"
        if (v.type() == typeid(int)       ||
            v.type() == typeid(long)      ||
            v.type() == typeid(long long) ||
            v.type() == typeid(int64_t))
        {
            return std::to_string((long long)d);
        }
        std::ostringstream os;
        os << d;
        return os.str();
    }
    return {};
}

size_t DataAdapter::row_count() const {
    ensure_view();
    return m_view_indices.size();
}

size_t DataAdapter::column_count() const {
    return storage_column_count();
}

std::string DataAdapter::column_name(int column) const {
    return storage_column_name(column);
}

DataType DataAdapter::column_type(int column) const {
    return storage_column_type(column);
}

nanogui::any DataAdapter::get_value(int row, int column) const {
    ensure_view();
    if (row < 0 || row >= (int)m_view_indices.size()) return {};
    return storage_get_value(m_view_indices[row], column);
}

CellStyle DataAdapter::get_cell_style(int row, int column) const {
    ensure_view();
    if (row < 0 || row >= (int)m_view_indices.size()) return {};
    return storage_get_cell_style(m_view_indices[row], column);
}

bool DataAdapter::is_cell_editable(int row, int column) const {
    ensure_view();
    if (row < 0 || row >= (int)m_view_indices.size()) return false;
    return storage_is_editable(m_view_indices[row], column);
}

bool DataAdapter::set_value(int row, int column, const nanogui::any& value) {
    ensure_view();
    if (row < 0 || row >= (int)m_view_indices.size()) return false;
    const int storage_row = m_view_indices[row];
    if (!storage_set_value(storage_row, column, value))
        return false;

    // Edits may change sort position or filter membership.  We do an
    // incremental update only when the touched column isn't the active sort /
    // filter target -- otherwise a full rebuild is safer and not noticeably
    // slower on typical view sizes.
    const bool affects_sort   = (m_sort_column == column);
    const bool affects_filter = !m_filter_text.empty();
    if (affects_sort || affects_filter)
        invalidate_view();

    return true;
}

bool DataAdapter::set_values(const std::vector<CellChange>& changes) {
    bool all_ok = true;
    for (const auto& ch : changes) {
        if (!set_value(ch.row, ch.column, ch.new_value))
            all_ok = false;
    }
    return all_ok;
}

void DataAdapter::sort(int column, bool ascending) {
    m_sort_column = column;
    m_sort_ascending = ascending;
    invalidate_view();
}

void DataAdapter::set_filter(const std::string& text) {
    m_filter_text = text;
    invalidate_view();
}

void DataAdapter::invalidate_view() {
    m_view_dirty = true;
}

bool DataAdapter::row_matches_filter(int storage_row, const std::string& filter) const {
    if (filter.empty()) return true;

    std::string lower_filter = filter;
    to_lower_inplace(lower_filter);

    const int ncols = (int)storage_column_count();
    for (int c = 0; c < ncols; ++c) {
        std::string s = any_to_string(storage_get_value(storage_row, c));
        if (s.empty()) continue;
        to_lower_inplace(s);
        if (s.find(lower_filter) != std::string::npos)
            return true;
    }
    return false;
}

void DataAdapter::rebuild_view() const {
    m_view_indices.clear();
    const size_t n = storage_row_count();
    m_view_indices.reserve(n);

    if (m_filter_text.empty()) {
        for (int i = 0; i < (int)n; ++i)
            m_view_indices.push_back(i);
    } else {
        for (int i = 0; i < (int)n; ++i) {
            if (row_matches_filter(i, m_filter_text))
                m_view_indices.push_back(i);
        }
    }

    // Only do an in-memory sort if the storage backend doesn't already serve
    // rows in the requested order. For SQL backends this is critical -- letting
    // the database do the ORDER BY is many orders of magnitude faster than
    // round-tripping through storage_get_value() per comparison.
    if (m_sort_column >= 0 && !storage_handles_sorting()) {
        const int  col = m_sort_column;
        const bool asc = m_sort_ascending;
        std::sort(m_view_indices.begin(), m_view_indices.end(),
            [this, col, asc](int a, int b) {
                const nanogui::any va = storage_get_value(a, col);
                const nanogui::any vb = storage_get_value(b, col);
                return asc ? any_less(va, vb) : any_less(vb, va);
            });
    }

    m_view_dirty = false;
}

NAMESPACE_END(nanogui)
