/*
 * pagemade/composer/hyphenator.h — Liang's pattern hyphenation, as in TeX.
 *
 * Loads hyph-utf8 pattern files (one pattern per line, e.g. ".ach4" or
 * "4ab.") and an optional exceptions file (one hyphenated word per line,
 * e.g. "ta-ble"). points() returns where a lowercase word may break, as
 * codepoint indices: i means a hyphen may go between word[i-1] and word[i].
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pagemade {

class Hyphenator {
public:
    bool load(const std::string &patterns_path, const std::string &exceptions_path = "");
    void add_pattern(const std::string &utf8);
    void add_exception(const std::string &utf8);
    bool empty() const { return m_patterns.empty(); }

    std::vector<size_t> points(const std::u32string &word) const;

    /* Shortest fragment allowed before / after a hyphen (en-US: 2 and 3). */
    int left_min = 2;
    int right_min = 3;

private:
    std::unordered_map<std::u32string, std::vector<uint8_t>> m_patterns;
    std::unordered_map<std::u32string, std::vector<size_t>> m_exceptions;
    size_t m_max_len = 0;
};

/* Lowercase for the scripts the patterns cover (Latin, Greek, Cyrillic). */
char32_t to_lower(char32_t c);
bool is_letter(char32_t c);

std::u32string utf8_to_u32(const std::string &s);

} // namespace pagemade
