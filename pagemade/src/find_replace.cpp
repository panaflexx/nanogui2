/*
 * pagemade/find_replace.cpp — see find_replace.h.
 */
#include "find_replace.h"

#include "composer/edit.h"

#include <utility>

namespace pagemade {

namespace {

/* A code point and where it starts in the text */
struct Cp {
    uint32_t c;
    uint32_t at;
};

std::vector<Cp> decode(const std::string &s) {
    std::vector<Cp> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b = (unsigned char) s[i];
        uint32_t c = b;
        size_t n = 1;
        if (b >= 0xF0 && b < 0xF8)
            n = 4, c = b & 0x07;
        else if (b >= 0xE0)
            n = 3, c = b & 0x0F;
        else if (b >= 0xC0)
            n = 2, c = b & 0x1F;
        if (n > 1) {
            if (i + n > s.size())
                n = 1, c = 0xFFFD;
            else
                for (size_t k = 1; k < n; ++k) {
                    const unsigned char t = (unsigned char) s[i + k];
                    if ((t & 0xC0) != 0x80) {
                        n = 1;
                        c = 0xFFFD;
                        break;
                    }
                    c = (c << 6) | (t & 0x3F);
                }
        }
        out.push_back({c, (uint32_t) i});
        i += n;
    }
    return out;
}

/* Lower case for Latin, Greek and Cyrillic; anything else is itself */
uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c < 0x80)
        return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130)
            return 'i';
        if (c == 0x178)
            return 0xFF;
        if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F)
            return c;
        /* Pairs start even up to U+0137, odd from U+0139 to U+0148, even again after */
        const bool odd_pairs = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        return odd_pairs ? (c % 2 == 1 ? c + 1 : c) : (c % 2 == 0 ? c + 1 : c);
    }
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    return c;
}

/* A letter or digit, for Whole Word: ASCII ones, and every non-ASCII
 * code point outside the symbol and punctuation blocks */
bool word_char(uint32_t c) {
    if (c < 0x80)
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               c == '_';
    if (c <= 0xBF || c == 0xD7 || c == 0xF7)
        return false;
    if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF0F) || c == 0xFFFC || c == 0xFFFD || (c >= 0x1F000 && c <= 0x1FAFF))
        return false;
    return true;
}

std::vector<StoryMatch> find_in_text(const std::string &text, const std::vector<uint32_t> &want,
                                     const FindOptions &opt, size_t para) {
    std::vector<StoryMatch> out;
    if (want.empty() || text.empty())
        return out;
    const std::vector<Cp> cps = decode(text);
    const size_t n = want.size();
    size_t i = 0;
    while (i + n <= cps.size()) {
        bool hit = true;
        for (size_t k = 0; k < n && hit; ++k) {
            const uint32_t c = opt.match_case ? cps[i + k].c : fold(cps[i + k].c);
            hit = c == want[k];
        }
        if (hit && opt.whole_word)
            hit = (i == 0 || !word_char(cps[i - 1].c)) &&
                  (i + n == cps.size() || !word_char(cps[i + n].c));
        if (!hit) {
            ++i;
            continue;
        }
        const uint32_t a = cps[i].at;
        const uint32_t b = i + n < cps.size() ? cps[i + n].at : (uint32_t) text.size();
        out.push_back({para, a, b - a});
        i += n;
    }
    return out;
}

/* Whole characters: a cut that lands inside one moves off it */
size_t back_to_char(const std::string &s, size_t i) {
    while (i > 0 && i < s.size() && ((unsigned char) s[i] & 0xC0) == 0x80)
        --i;
    return i;
}

std::string shown(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char) s[i];
        if (c < 0x20) {
            out += ' ';
        } else if (c == 0xE2 && i + 2 < s.size() && (unsigned char) s[i + 1] == 0x80 &&
                   ((unsigned char) s[i + 2] == 0xA8 || (unsigned char) s[i + 2] == 0xA9)) {
            out += ' ';                  // U+2028, U+2029
            i += 2;
        } else {
            out += (char) c;
        }
    }
    return out;
}

} // namespace

std::vector<StoryMatch> find_matches(const Story &s, const std::string &needle,
                                     const FindOptions &opt) {
    std::vector<StoryMatch> out;
    std::vector<uint32_t> want;
    for (const Cp &cp : decode(needle))
        want.push_back(opt.match_case ? cp.c : fold(cp.c));
    if (want.empty())
        return out;
    for (size_t pi = 0; pi < s.paragraphs.size(); ++pi) {
        std::vector<StoryMatch> m = find_in_text(paragraph_text(s.paragraphs[pi]), want, opt, pi);
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

std::vector<FoundText> find_all(const PageDoc &doc, const std::string &needle,
                                const std::string &replacement, const FindOptions &opt) {
    std::vector<FoundText> out;
    for (const StoryEntry &se : doc.stories) {
        size_t para = SIZE_MAX;
        uint32_t end = 0;
        std::string text;
        for (const StoryMatch &m : find_matches(se.story, needle, opt)) {
            if (m.para != para) {
                para = m.para;
                end = 0;
                text = paragraph_text(se.story.paragraphs[para]);
            }
            FoundText f;
            f.story = se.id;
            f.para = m.para;
            f.gap = m.byte - end;
            f.found = text.substr(m.byte, m.len);
            f.replacement = replacement;
            f.byte = m.byte;
            out.push_back(std::move(f));
            end = m.byte + m.len;
        }
    }
    return out;
}

void resolve_found(const PageDoc &doc, std::vector<FoundText> &finds) {
    StoryId story = 0;
    size_t para = SIZE_MAX;
    const StoryEntry *se = nullptr;
    std::string text;
    bool have = false;                   // the paragraph is there
    uint32_t end = 0;
    for (FoundText &f : finds) {
        if (f.story != story || f.para != para) {
            story = f.story;
            para = f.para;
            se = doc.find_story(story);
            have = se && para < se->story.paragraphs.size();
            text = have ? paragraph_text(se->story.paragraphs[para]) : std::string();
            end = 0;
        }
        const uint32_t at = end + f.gap;
        f.byte = at;
        auto holds = [&](const std::string &s) {
            return have && at <= text.size() && text.compare(at, s.size(), s) == 0;
        };
        /* What it was last seen holding wins a tie (a replacement that
         * starts with the original text, say) */
        const std::string &was = f.current();
        const std::string &other = f.state == FoundText::State::Replaced ? f.found : f.replacement;
        if (f.state != FoundText::State::Stale && holds(was)) {
            // unchanged
        } else if (holds(other) && f.state != FoundText::State::Stale) {
            f.state = f.state == FoundText::State::Replaced ? FoundText::State::Found
                                                            : FoundText::State::Replaced;
        } else if (holds(f.found)) {
            f.state = FoundText::State::Found;
        } else if (holds(f.replacement)) {
            f.state = FoundText::State::Replaced;
        } else {
            f.state = FoundText::State::Stale;
        }
        end = at + (uint32_t) f.current().size();
    }
}

bool apply_found(PageDoc &doc, FoundText &f, bool replace) {
    if (f.state == FoundText::State::Stale)
        return false;
    if ((f.state == FoundText::State::Replaced) == replace)
        return false;
    StoryEntry *se = doc.find_story(f.story);
    if (!se || f.para >= se->story.paragraphs.size())
        return false;
    Story &s = se->story;
    const std::string &now = f.current();
    const TextPos a{f.para, f.byte}, b{f.para, f.byte + (uint32_t) now.size()};
    if (paragraph_text(s.paragraphs[f.para]).compare(f.byte, now.size(), now) != 0)
        return false;
    /* The style of its first character: the style before the position
     * just after that character */
    TextPos first = a;
    if (a != b)
        first = next_grapheme(s, a);
    if (first.para != a.para || b < first)
        first = b;
    const CharStyle style = style_at(s, a == b ? a : first);
    const std::string &with = replace ? f.replacement : f.found;
    const TextPos at = erase(s, a, b);
    if (!with.empty())
        insert_text(s, at, with, style);
    f.state = replace ? FoundText::State::Replaced : FoundText::State::Found;
    return true;
}

void found_context(const PageDoc &doc, const FoundText &f, size_t before, size_t after,
                   std::string &head, std::string &tail) {
    head.clear();
    tail.clear();
    const StoryEntry *se = doc.find_story(f.story);
    if (!se || f.para >= se->story.paragraphs.size())
        return;
    const std::string text = paragraph_text(se->story.paragraphs[f.para]);
    const size_t at = std::min<size_t>(f.byte, text.size());
    const size_t end = std::min(text.size(), at + f.current().size());
    size_t from = at > before ? at - before : 0;
    if (from > 0) {
        while (from < at && ((unsigned char) text[from] & 0xC0) == 0x80)
            ++from;                      // forward to a whole character
    }
    size_t to = back_to_char(text, std::min(text.size(), end + after));
    head = (from > 0 ? "\xE2\x80\xA6" : "") + shown(text.substr(from, at - from));
    tail = shown(text.substr(end, to - end)) + (to < text.size() ? "\xE2\x80\xA6" : "");
}

} // namespace pagemade
