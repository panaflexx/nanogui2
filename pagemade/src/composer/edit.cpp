/*
 * pagemade/composer/edit.cpp — see edit.h.
 */
#include "composer/edit.h"

#include <graphemebreak.h>
#include <wordbreak.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace pagemade {

namespace {

uint32_t decode(const std::string &s, size_t i) {
    unsigned char c = (unsigned char) s[i];
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n && i + k < s.size(); ++k)
        cp = (cp << 6) | ((unsigned char) s[i + k] & 0x3F);
    return cp;
}

size_t char_len(const std::string &s, size_t i) {
    unsigned char c = (unsigned char) s[i];
    return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
}

void init_breaks() {
    static std::once_flag once;
    std::call_once(once, [] { init_graphemebreak(); init_wordbreak(); });
}

/* Byte offsets where a grapheme (or word) boundary falls, 0 and the end
 * included. */
std::vector<uint32_t> boundaries(const std::string &t, bool words) {
    init_breaks();
    std::vector<uint32_t> out{0};
    if (t.empty())
        return out;
    std::vector<char> brks(t.size());
    if (words)
        set_wordbreaks_utf8((const utf8_t *) t.data(), t.size(), "en", brks.data());
    else
        set_graphemebreaks_utf8((const utf8_t *) t.data(), t.size(), "en", brks.data());
    const char brk = words ? WORDBREAK_BREAK : GRAPHEMEBREAK_BREAK;
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if (brks[i] == brk)
            out.push_back((uint32_t) i + 1);
    out.push_back((uint32_t) t.size());
    return out;
}

bool is_word_char(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           cp == '_' || (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && cp != 0x2028 &&
           !(cp >= 0x2000 && cp <= 0x206F) && cp != 0xA0 && cp != 0xAD);
}

bool has_word_char(const std::string &t, uint32_t a, uint32_t b) {
    for (uint32_t i = a; i < b; i += (uint32_t) char_len(t, i))
        if (is_word_char(decode(t, i)))
            return true;
    return false;
}

/* Make sure a run boundary falls at `byte`; returns the index of the run
 * that starts there (runs.size() at the end). */
size_t split_at(Paragraph &p, uint32_t byte) {
    uint32_t acc = 0;
    for (size_t i = 0; i < p.runs.size(); ++i) {
        uint32_t len = (uint32_t) p.runs[i].text.size();
        if (byte == acc)
            return i;
        if (byte < acc + len) {
            Run tail{p.runs[i].style, p.runs[i].text.substr(byte - acc)};
            p.runs[i].text.resize(byte - acc);
            p.runs.insert(p.runs.begin() + (long) i + 1, tail);
            return i + 1;
        }
        acc += len;
    }
    return p.runs.size();
}

void erase_bytes(Paragraph &p, uint32_t a, uint32_t b) {
    if (a >= b)
        return;
    size_t i0 = split_at(p, a);
    size_t i1 = split_at(p, b);
    p.runs.erase(p.runs.begin() + (long) i0, p.runs.begin() + (long) i1);
}

void insert_plain(Paragraph &p, uint32_t byte, const std::string &text, const CharStyle &style) {
    if (text.empty())
        return;
    uint32_t acc = 0;
    for (size_t i = 0; i < p.runs.size(); ++i) {
        Run &r = p.runs[i];
        uint32_t len = (uint32_t) r.text.size();
        if (byte <= acc + len) {
            if (r.style == style) {
                r.text.insert(byte - acc, text);
                normalize(p);
                return;
            }
            if (byte == acc + len && i + 1 < p.runs.size() && p.runs[i + 1].style == style) {
                p.runs[i + 1].text.insert(0, text);
                normalize(p);
                return;
            }
            size_t at = split_at(p, byte);
            p.runs.insert(p.runs.begin() + (long) at, Run{style, text});
            normalize(p);
            return;
        }
        acc += len;
    }
    p.runs.push_back(Run{style, text});
    normalize(p);
}

} // namespace

/* ---- Edits -------------------------------------------------------------- */

uint32_t para_length(const Paragraph &p) {
    uint32_t n = 0;
    for (const Run &r : p.runs)
        n += (uint32_t) r.text.size();
    return n;
}

TextPos story_end(const Story &s) {
    if (s.paragraphs.empty())
        return {};
    return {s.paragraphs.size() - 1, para_length(s.paragraphs.back())};
}

TextPos clamp(const Story &s, TextPos p) {
    if (s.paragraphs.empty())
        return {};
    p.para = std::min(p.para, s.paragraphs.size() - 1);
    p.byte = std::min(p.byte, para_length(s.paragraphs[p.para]));
    return p;
}

void normalize(Paragraph &p) {
    if (p.runs.empty())
        return;
    CharStyle keep = p.runs.front().style;
    std::vector<Run> out;
    for (Run &r : p.runs) {
        if (r.text.empty())
            continue;
        if (!out.empty() && out.back().style == r.style)
            out.back().text += r.text;
        else
            out.push_back(std::move(r));
    }
    if (out.empty())
        out.push_back(Run{keep, ""});
    p.runs = std::move(out);
}

CharStyle style_at(const Story &s, TextPos p) {
    if (s.paragraphs.empty())
        return CharStyle();
    p = clamp(s, p);
    const Paragraph &para = s.paragraphs[p.para];
    if (para.runs.empty())
        return p.para > 0 ? style_at(s, {p.para - 1, para_length(s.paragraphs[p.para - 1])})
                          : CharStyle();
    uint32_t acc = 0;
    for (const Run &r : para.runs) {
        uint32_t len = (uint32_t) r.text.size();
        if (p.byte > acc && p.byte <= acc + len)
            return r.style;
        acc += len;
    }
    return para.runs.front().style;
}

TextPos split_paragraph(Story &s, TextPos p) {
    p = clamp(s, p);
    const CharStyle carry = style_at(s, p);
    Paragraph &a = s.paragraphs[p.para];
    Paragraph b;
    b.style = a.style;
    size_t at = split_at(a, p.byte);
    b.runs.assign(a.runs.begin() + (long) at, a.runs.end());
    a.runs.erase(a.runs.begin() + (long) at, a.runs.end());
    if (a.runs.empty() || para_length(a) == 0)
        a.runs = {Run{carry, ""}};
    if (b.runs.empty() || para_length(b) == 0)
        b.runs = {Run{carry, ""}};
    normalize(a);
    normalize(b);
    s.paragraphs.insert(s.paragraphs.begin() + (long) p.para + 1, std::move(b));
    return {p.para + 1, 0};
}

TextPos insert_text(Story &s, TextPos p, const std::string &utf8, const CharStyle &style) {
    if (s.paragraphs.empty())
        s.paragraphs.emplace_back();
    p = clamp(s, p);
    size_t i = 0;
    while (i <= utf8.size()) {
        size_t nl = utf8.find('\n', i);
        std::string line = utf8.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        insert_plain(s.paragraphs[p.para], p.byte, line, style);
        p.byte += (uint32_t) line.size();
        if (nl == std::string::npos)
            break;
        p = split_paragraph(s, p);
        i = nl + 1;
    }
    return p;
}

TextPos erase(Story &s, TextPos a, TextPos b) {
    a = clamp(s, a);
    b = clamp(s, b);
    if (b < a)
        std::swap(a, b);
    if (a == b)
        return a;
    if (a.para == b.para) {
        erase_bytes(s.paragraphs[a.para], a.byte, b.byte);
        normalize(s.paragraphs[a.para]);
        return a;
    }
    Paragraph &first = s.paragraphs[a.para];
    Paragraph &last = s.paragraphs[b.para];
    erase_bytes(first, a.byte, para_length(first));
    erase_bytes(last, 0, b.byte);
    first.runs.insert(first.runs.end(), last.runs.begin(), last.runs.end());
    s.paragraphs.erase(s.paragraphs.begin() + (long) a.para + 1,
                       s.paragraphs.begin() + (long) b.para + 1);
    if (first.runs.empty())
        first.runs = {Run{style_at(s, a), ""}};
    normalize(first);
    return a;
}

void restyle(Story &s, TextPos a, TextPos b, const std::function<void(CharStyle &)> &fn) {
    a = clamp(s, a);
    b = clamp(s, b);
    if (b < a)
        std::swap(a, b);
    for (size_t pi = a.para; pi <= b.para; ++pi) {
        Paragraph &p = s.paragraphs[pi];
        uint32_t from = pi == a.para ? a.byte : 0;
        uint32_t to = pi == b.para ? b.byte : para_length(p);
        if (from >= to && para_length(p) > 0)
            continue;
        size_t i0 = split_at(p, from);
        size_t i1 = split_at(p, to);
        for (size_t i = i0; i < i1; ++i)
            fn(p.runs[i].style);
        if (para_length(p) == 0)
            for (Run &r : p.runs) fn(r.style);
        normalize(p);
    }
}

std::string copy_text(const Story &s, TextPos a, TextPos b) {
    a = clamp(s, a);
    b = clamp(s, b);
    if (b < a)
        std::swap(a, b);
    std::string out;
    for (size_t pi = a.para; pi <= b.para; ++pi) {
        std::string t = paragraph_text(s.paragraphs[pi]);
        uint32_t from = pi == a.para ? a.byte : 0;
        uint32_t to = pi == b.para ? b.byte : (uint32_t) t.size();
        out += t.substr(from, to - from);
        if (pi != b.para)
            out += '\n';
    }
    return out;
}

/* ---- Movement ----------------------------------------------------------- */

TextPos next_grapheme(const Story &s, TextPos p) {
    p = clamp(s, p);
    if (s.paragraphs.empty())
        return p;
    const std::string t = paragraph_text(s.paragraphs[p.para]);
    if (p.byte >= t.size())
        return p.para + 1 < s.paragraphs.size() ? TextPos{p.para + 1, 0} : p;
    for (uint32_t b : boundaries(t, false))
        if (b > p.byte)
            return {p.para, b};
    return {p.para, (uint32_t) t.size()};
}

TextPos prev_grapheme(const Story &s, TextPos p) {
    p = clamp(s, p);
    if (s.paragraphs.empty())
        return p;
    if (p.byte == 0)
        return p.para > 0 ? TextPos{p.para - 1, para_length(s.paragraphs[p.para - 1])} : p;
    const std::string t = paragraph_text(s.paragraphs[p.para]);
    uint32_t best = 0;
    for (uint32_t b : boundaries(t, false))
        if (b < p.byte)
            best = b;
    return {p.para, best};
}

TextPos next_word(const Story &s, TextPos p) {
    p = clamp(s, p);
    if (s.paragraphs.empty())
        return p;
    const std::string t = paragraph_text(s.paragraphs[p.para]);
    std::vector<uint32_t> b = boundaries(t, true);
    for (size_t i = 0; i + 1 < b.size(); ++i)
        if (b[i + 1] > p.byte && has_word_char(t, b[i], b[i + 1]))
            return {p.para, b[i + 1]};
    if (p.byte < t.size())
        return {p.para, (uint32_t) t.size()};
    return p.para + 1 < s.paragraphs.size() ? next_word(s, {p.para + 1, 0}) : p;
}

TextPos prev_word(const Story &s, TextPos p) {
    p = clamp(s, p);
    if (s.paragraphs.empty())
        return p;
    const std::string t = paragraph_text(s.paragraphs[p.para]);
    std::vector<uint32_t> b = boundaries(t, true);
    for (size_t i = b.size() - 1; i > 0; --i)
        if (b[i - 1] < p.byte && has_word_char(t, b[i - 1], b[i]))
            return {p.para, b[i - 1]};
    if (p.byte > 0)
        return {p.para, 0};
    return p.para > 0 ? prev_word(s, {p.para - 1, para_length(s.paragraphs[p.para - 1])}) : p;
}

std::pair<TextPos, TextPos> word_at(const Story &s, TextPos p) {
    p = clamp(s, p);
    if (s.paragraphs.empty())
        return {p, p};
    const std::string t = paragraph_text(s.paragraphs[p.para]);
    std::vector<uint32_t> b = boundaries(t, true);
    for (size_t i = 0; i + 1 < b.size(); ++i) {
        bool inside = p.byte >= b[i] && p.byte < b[i + 1];
        bool at_end = p.byte == b[i + 1] && i + 2 == b.size();
        if (inside || at_end) {
            /* Clicking just after a word (on the space) selects the word. */
            if (!has_word_char(t, b[i], b[i + 1]) && p.byte == b[i] && i > 0 &&
                has_word_char(t, b[i - 1], b[i]))
                return {{p.para, b[i - 1]}, {p.para, b[i]}};
            return {{p.para, b[i]}, {p.para, b[i + 1]}};
        }
    }
    return {p, p};
}

/* ---- Composition <-> positions ------------------------------------------ */

namespace {

/* A line's text glyphs grouped by cluster, in order. */
struct ClusterBox {
    uint32_t c0, c1;     // bytes
    float    x, adv;
};

std::vector<ClusterBox> clusters_of(const ComposedLine &l) {
    std::vector<ClusterBox> out;
    for (const GlyphRun &r : l.runs)
        for (const PlacedGlyph &g : r.glyphs) {
            if (g.flags & PlacedGlyph::Inserted)
                continue;
            if (!out.empty() && out.back().c0 == g.cluster) {
                out.back().adv += g.adv;
                continue;
            }
            out.push_back({g.cluster, 0, g.x, g.adv});
        }
    std::sort(out.begin(), out.end(),
              [](const ClusterBox &a, const ClusterBox &b) { return a.c0 < b.c0; });
    for (size_t i = 0; i < out.size(); ++i)
        out[i].c1 = i + 1 < out.size() ? out[i + 1].c0 : l.byte_end;
    return out;
}

} // namespace

float x_at(const ComposedLine &l, const std::string &text, uint32_t byte) {
    std::vector<ClusterBox> cb = clusters_of(l);
    if (cb.empty())
        return l.x_start;
    if (byte <= cb.front().c0)
        return cb.front().x;
    for (const ClusterBox &b : cb) {
        if (byte == b.c0)
            return b.x;
        if (byte > b.c0 && byte < b.c1) {
            /* Inside a ligature: share its advance between its characters. */
            int n = 0, k = 0;
            for (uint32_t i = b.c0; i < b.c1 && i < text.size(); i += (uint32_t) char_len(text, i)) {
                if (i < byte) ++k;
                ++n;
            }
            return b.x + b.adv * (n ? (float) k / (float) n : 0.f);
        }
    }
    return cb.back().x + cb.back().adv;
}

Caret caret_at(const Composition &c, const Story &s, TextPos p) {
    Caret out;
    if (s.paragraphs.empty())
        return out;
    p = clamp(s, p);
    for (size_t i = 0; i < c.lines.size(); ++i) {
        const ComposedLine &l = c.lines[i];
        if (l.para != p.para || p.byte < l.byte_start)
            continue;
        if (p.byte < l.byte_end || (p.byte == l.byte_end && l.para_end)) {
            out.line = i;
            out.x = x_at(l, paragraph_text(s.paragraphs[p.para]), p.byte);
            out.top = l.top;
            out.bottom = l.top + l.leading;
            return out;
        }
    }
    return out;
}

TextPos line_start(const Composition &c, size_t line) {
    const ComposedLine &l = c.lines[line];
    return {l.para, l.byte_start};
}

TextPos line_end(const Composition &c, const Story &s, size_t line) {
    const ComposedLine &l = c.lines[line];
    if (l.para_end)
        return {l.para, l.byte_end};
    const std::string t = paragraph_text(s.paragraphs[l.para]);
    uint32_t e = l.byte_end;
    while (e > l.byte_start) {
        size_t i = e - 1;
        while (i > l.byte_start && ((unsigned char) t[i] & 0xC0) == 0x80)
            --i;
        uint32_t cp = decode(t, i);
        if (cp != ' ' && cp != 0xA0 && cp != '\n' && cp != '\r' && cp != 0x2028 && cp != 0x2029)
            break;
        e = (uint32_t) i;
    }
    return {l.para, e};
}

TextPos pos_at_x(const Composition &c, const Story &s, size_t line, float x) {
    const ComposedLine &l = c.lines[line];
    const std::string t = paragraph_text(s.paragraphs[l.para]);
    TextPos out{l.para, l.byte_end};
    for (const ClusterBox &b : clusters_of(l)) {
        std::vector<uint32_t> starts;
        for (uint32_t i = b.c0; i < b.c1 && i < t.size(); i += (uint32_t) char_len(t, i))
            starts.push_back(i);
        if (starts.empty())
            starts.push_back(b.c0);
        const float sub = b.adv / (float) starts.size();
        bool found = false;
        for (size_t k = 0; k < starts.size(); ++k)
            if (x < b.x + sub * ((float) k + 0.5f)) {
                out.byte = starts[k];
                found = true;
                break;
            }
        if (found)
            break;
    }
    if (out.byte >= l.byte_end && !l.para_end)
        out = line_end(c, s, line);
    if (out.byte < l.byte_start)
        out.byte = l.byte_start;
    return out;
}

TextPos hit_test(const Composition &c, const Story &s, const std::vector<Frame> &frames,
                 float x, float y) {
    if (c.lines.empty() || frames.empty())
        return {};
    /* The frame under the point, else the nearest one. */
    size_t fi = 0;
    float best = INFINITY;
    for (size_t i = 0; i < frames.size(); ++i) {
        const Frame &f = frames[i];
        float dx = std::max({f.x - x, 0.f, x - (f.x + f.w)});
        float dy = std::max({f.y - y, 0.f, y - (f.y + f.h)});
        float d = dx * dx + dy * dy;
        if (d < best) { best = d; fi = i; }
    }
    size_t first = SIZE_MAX, last = SIZE_MAX;
    for (size_t i = 0; i < c.lines.size(); ++i)
        if (c.lines[i].frame == fi) {
            if (first == SIZE_MAX) first = i;
            last = i;
        }
    if (first == SIZE_MAX) {
        /* An empty block: before the text if it comes first, else the end. */
        if (fi < c.lines.front().frame)
            return line_start(c, 0);
        const ComposedLine &l = c.lines.back();
        return {l.para, l.byte_end};
    }
    size_t line = last;
    for (size_t i = first; i <= last; ++i)
        if (y < c.lines[i].top + c.lines[i].leading) {
            line = i;
            break;
        }
    return pos_at_x(c, s, line, x);
}

} // namespace pagemade
