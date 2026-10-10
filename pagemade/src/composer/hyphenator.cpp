/*
 * pagemade/composer/hyphenator.cpp — see hyphenator.h.
 */
#include "composer/hyphenator.h"
#include "composer/utf8.h"

#include <algorithm>
#include <fstream>

namespace pagemade {

std::u32string utf8_to_u32(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        out.push_back(utf8_decode(s, i));
        i += utf8_len(s, i);
    }
    return out;
}

char32_t to_lower(char32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;          // Latin-1
    if (c >= 0x100 && c <= 0x17F && !(c & 1)) return c + 1;             // Latin Extended-A (mostly)
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;         // Greek
    if (c >= 0x410 && c <= 0x42F) return c + 32;                        // Cyrillic
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

bool is_letter(char32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x4FF);
}

void Hyphenator::add_pattern(const std::string &utf8) {
    std::u32string letters;
    std::vector<uint8_t> values(1, 0);
    for (char32_t c : utf8_to_u32(utf8)) {
        if (c >= '0' && c <= '9') {
            values.back() = (uint8_t) (c - '0');
        } else {
            letters.push_back(c);
            values.push_back(0);
        }
    }
    if (letters.empty())
        return;
    m_max_len = std::max(m_max_len, letters.size());
    m_patterns[letters] = std::move(values);
}

void Hyphenator::add_exception(const std::string &utf8) {
    std::u32string word;
    std::vector<size_t> pts;
    for (char32_t c : utf8_to_u32(utf8)) {
        if (c == '-')
            pts.push_back(word.size());
        else
            word.push_back(to_lower(c));
    }
    if (!word.empty())
        m_exceptions[word] = std::move(pts);
}

bool Hyphenator::load(const std::string &patterns_path, const std::string &exceptions_path) {
    std::ifstream in(patterns_path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty() && line[0] != '%')
            add_pattern(line);
    }
    if (!exceptions_path.empty()) {
        std::ifstream ex(exceptions_path);
        if (!ex)
            return false;
        while (std::getline(ex, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty() && line[0] != '%')
                add_exception(line);
        }
    }
    return !m_patterns.empty();
}

std::vector<size_t> Hyphenator::points(const std::u32string &word) const {
    std::vector<size_t> out;
    const size_t n = word.size();
    if (n < (size_t) (left_min + right_min))
        return out;

    auto keep = [&](size_t i) { return i >= (size_t) left_min && i <= n - right_min; };
    auto ex = m_exceptions.find(word);
    if (ex != m_exceptions.end()) {
        for (size_t i : ex->second)
            if (keep(i))
                out.push_back(i);
        return out;
    }

    /* Every substring of ".word." that is a pattern raises the values
     * between its letters; odd values mark allowed breaks. */
    const std::u32string w = U"." + word + U".";
    std::vector<uint8_t> vals(w.size() + 1, 0);
    for (size_t i = 0; i < w.size(); ++i) {
        for (size_t len = 1; len <= m_max_len && i + len <= w.size(); ++len) {
            auto it = m_patterns.find(std::u32string_view(w.data() + i, len));
            if (it == m_patterns.end())
                continue;
            for (size_t k = 0; k < it->second.size(); ++k)
                vals[i + k] = std::max(vals[i + k], it->second[k]);
        }
    }
    /* vals[j] sits before w[j]; w[j] is word[j - 1]. */
    for (size_t i = 1; i < n; ++i)
        if ((vals[i + 1] & 1) && keep(i))
            out.push_back(i);
    return out;
}

} // namespace pagemade
