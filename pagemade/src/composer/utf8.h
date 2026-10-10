/*
 * pagemade/composer/utf8.h — one UTF-8 decoder for shaping, carets,
 * hyphenation and PDF. Valid text matches; a continuation byte is one
 * byte, not a two-byte lead.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pagemade {

inline uint32_t utf8_decode(const std::string &s, size_t i) {
    if (i >= s.size())
        return 0;
    unsigned char c = (unsigned char) s[i];
    if (c < 0x80)
        return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n && i + k < s.size(); ++k)
        cp = (cp << 6) | ((unsigned char) s[i + k] & 0x3F);
    return cp;
}

/* Byte length of the character at i. At least 1, so a scan always advances. */
inline size_t utf8_len(const std::string &s, size_t i) {
    if (i >= s.size())
        return 1;
    unsigned char c = (unsigned char) s[i];
    int n = c < 0x80 ? 0 : (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    return (size_t) n + 1;
}

/* Start byte of the character that ends just before `end`. */
inline size_t utf8_prev(const std::string &s, size_t end) {
    if (end == 0 || s.empty())
        return 0;
    size_t i = end - 1;
    if (i >= s.size())
        i = s.size() - 1;
    while (i > 0 && ((unsigned char) s[i] & 0xC0) == 0x80)
        --i;
    return i;
}

/* Line breaks the composer treats as their own (invisible) glyph. */
inline bool is_break(uint32_t cp) {
    return cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C || cp == 0x85 ||
           cp == 0x2028 || cp == 0x2029;
}

} // namespace pagemade
