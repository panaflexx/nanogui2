/*
 * pagemade/plugin/pdf_parse.cpp — see pdf_parse.h.
 *
 * The newest cross-reference section is read first and wins: an entry or a
 * trailer key already set is never overwritten by an older section (/Prev).
 * Objects are parsed when first asked for and kept.
 */
#include "plugin/pdf_parse.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace pagemade {
namespace pdf {

namespace {

bool is_space(uint8_t c) {
    return c == 0 || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

bool is_delim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '/' || c == '%';
}

int hex_value(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

Object keyword(std::string s) {
    Object o;
    o.type = Type::Keyword;
    o.str = std::move(s);
    return o;
}

} // namespace

const Object &null_object() {
    static const Object null;
    return null;
}

const Object *Object::get(const char *key) const {
    if (!dict)
        return nullptr;
    auto it = dict->find(key);
    return it == dict->end() ? nullptr : &it->second;
}

const Object &Object::operator[](size_t i) const {
    return array && i < array->size() ? (*array)[i] : null_object();
}

/* ---- Lexer ----------------------------------------------------------------- */

void Lexer::skip_space() {
    while (m_pos < m_n) {
        const uint8_t c = m_d[m_pos];
        if (is_space(c)) {
            ++m_pos;
        } else if (c == '%') {
            while (m_pos < m_n && m_d[m_pos] != '\n' && m_d[m_pos] != '\r')
                ++m_pos;
        } else {
            break;
        }
    }
}

bool Lexer::token(Object &out) {
    out = Object();
    skip_space();
    if (m_pos >= m_n)
        return false;
    const uint8_t c = m_d[m_pos];
    if (c == '(') {                      // a literal string, parentheses balanced
        ++m_pos;
        int depth = 1;
        std::string s;
        while (m_pos < m_n) {
            uint8_t ch = m_d[m_pos++];
            if (ch == '\\' && m_pos < m_n) {
                const uint8_t e = m_d[m_pos++];
                switch (e) {
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case '\r':
                    if (m_pos < m_n && m_d[m_pos] == '\n')
                        ++m_pos;
                    break;               // a line continuation
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        for (int k = 0; k < 2 && m_pos < m_n && m_d[m_pos] >= '0' && m_d[m_pos] <= '7'; ++k)
                            v = v * 8 + (m_d[m_pos++] - '0');
                        s += (char) (v & 0xFF);
                    } else {
                        s += (char) e;   // \( \) \\ and anything else as itself
                    }
                }
                continue;
            }
            if (ch == '(')
                ++depth;
            else if (ch == ')' && --depth == 0)
                break;
            s += (char) ch;
        }
        out.type = Type::String;
        out.str = std::move(s);
        return true;
    }
    if (c == '<') {
        if (m_pos + 1 < m_n && m_d[m_pos + 1] == '<') {
            m_pos += 2;
            out = keyword("<<");
            return true;
        }
        ++m_pos;                         // a hex string
        std::string s;
        int hi = -1;
        while (m_pos < m_n && m_d[m_pos] != '>') {
            const int v = hex_value(m_d[m_pos++]);
            if (v < 0)
                continue;
            if (hi < 0) {
                hi = v;
            } else {
                s += (char) (hi * 16 + v);
                hi = -1;
            }
        }
        if (hi >= 0)
            s += (char) (hi * 16);
        if (m_pos < m_n)
            ++m_pos;
        out.type = Type::String;
        out.str = std::move(s);
        return true;
    }
    if (c == '>') {
        m_pos += (m_pos + 1 < m_n && m_d[m_pos + 1] == '>') ? 2 : 1;
        out = keyword(">>");
        return true;
    }
    if (c == '[' || c == ']' || c == '{' || c == '}') {
        ++m_pos;
        out = keyword(std::string(1, (char) c));
        return true;
    }
    if (c == '/') {
        ++m_pos;
        std::string s;
        while (m_pos < m_n && !is_space(m_d[m_pos]) && !is_delim(m_d[m_pos])) {
            if (m_d[m_pos] == '#' && m_pos + 2 < m_n && hex_value(m_d[m_pos + 1]) >= 0 &&
                hex_value(m_d[m_pos + 2]) >= 0) {
                s += (char) (hex_value(m_d[m_pos + 1]) * 16 + hex_value(m_d[m_pos + 2]));
                m_pos += 3;
            } else {
                s += (char) m_d[m_pos++];
            }
        }
        out.type = Type::Name;
        out.str = std::move(s);
        return true;
    }
    if (c == ')') {                      // stray: skip it
        ++m_pos;
        return token(out);
    }
    /* A number or a keyword: a run of regular characters */
    const size_t start = m_pos;
    while (m_pos < m_n && !is_space(m_d[m_pos]) && !is_delim(m_d[m_pos]))
        ++m_pos;
    std::string s((const char *) m_d + start, m_pos - start);
    bool numeric = !s.empty();
    bool dot = false;
    for (size_t i = 0; i < s.size() && numeric; ++i) {
        const char ch = s[i];
        if (ch == '.')
            dot = true;
        else if (!(std::isdigit((unsigned char) ch) || ((ch == '-' || ch == '+') && i == 0)))
            numeric = false;
    }
    if (numeric && s != "-" && s != "+" && s != ".") {
        out.type = dot ? Type::Real : Type::Int;
        out.number = std::strtod(s.c_str(), nullptr);
        return true;
    }
    if (s == "true" || s == "false") {
        out.type = Type::Bool;
        out.boolean = s == "true";
        return true;
    }
    if (s == "null") {
        out.type = Type::Null;
        return true;
    }
    out = keyword(std::move(s));
    return true;
}

bool Lexer::next(Object &out) {
    auto raw = [&](Object &o) {
        if (!m_ahead.empty()) {
            o = std::move(m_ahead.front());
            m_ahead.erase(m_ahead.begin());
            return true;
        }
        return token(o);
    };
    if (!raw(out))
        return false;
    if (out.type == Type::Keyword) {
        if (out.str == "[") {
            auto a = std::make_shared<Array>();
            Object e;
            while (next(e)) {
                if (e.type == Type::Keyword && e.str == "]")
                    break;
                a->push_back(std::move(e));
            }
            out = Object();
            out.type = Type::Array;
            out.array = std::move(a);
        } else if (out.str == "<<") {
            auto d = std::make_shared<Dict>();
            Object k;
            while (next(k)) {
                if (k.type == Type::Keyword && k.str == ">>")
                    break;
                if (k.type != Type::Name)
                    continue;            // junk where a key should be
                Object v;
                if (!next(v) || (v.type == Type::Keyword && v.str == ">>")) {
                    (*d)[k.str] = Object();
                    break;
                }
                (*d)[k.str] = std::move(v);
            }
            out = Object();
            out.type = Type::Dict;
            out.dict = std::move(d);
        }
        return true;
    }
    if (out.type == Type::Int && out.number >= 0) {
        /* "n g R" is one reference */
        Object a;
        if (raw(a)) {
            if (a.type == Type::Int) {
                Object b;
                if (raw(b)) {
                    if (b.type == Type::Keyword && b.str == "R") {
                        Object r;
                        r.type = Type::Ref;
                        r.num = (int) out.number;
                        r.gen = (int) a.number;
                        out = r;
                        return true;
                    }
                    m_ahead.push_back(std::move(a));
                    m_ahead.push_back(std::move(b));
                    return true;
                }
            }
            m_ahead.push_back(std::move(a));
        }
    }
    return true;
}

/* ---- Filters --------------------------------------------------------------- */

bool flate_decode(const std::string &in, std::string &out) {
    out.clear();
    if (in.empty())
        return true;
    /* zlib, else a raw deflate stream (some writers leave the header off).
     * A damaged stream keeps what came out before the damage. */
    for (int bits : {MZ_DEFAULT_WINDOW_BITS, -MZ_DEFAULT_WINDOW_BITS}) {
        mz_stream zs;
        std::memset(&zs, 0, sizeof zs);
        if (mz_inflateInit2(&zs, bits) != MZ_OK)
            continue;
        zs.next_in = (const unsigned char *) in.data();
        zs.avail_in = (unsigned int) in.size();
        std::string got;
        unsigned char buf[65536];
        int rc = MZ_OK;
        do {
            zs.next_out = buf;
            zs.avail_out = sizeof buf;
            rc = mz_inflate(&zs, MZ_SYNC_FLUSH);
            got.append((const char *) buf, sizeof buf - zs.avail_out);
        } while (rc == MZ_OK && (zs.avail_in > 0 || zs.avail_out == 0));
        mz_inflateEnd(&zs);
        if (rc == MZ_STREAM_END || !got.empty()) {
            out = std::move(got);
            return true;
        }
    }
    return false;
}

bool lzw_decode(const std::string &in, std::string &out, bool early_change) {
    out.clear();
    std::vector<std::string> table;
    auto reset = [&] {
        table.clear();
        for (int i = 0; i < 256; ++i)
            table.emplace_back(1, (char) i);
        table.emplace_back();            // 256 clear
        table.emplace_back();            // 257 end
    };
    reset();
    int width = 9;
    uint32_t buffer = 0;
    int bits = 0;
    std::string prev;
    bool have_prev = false;
    for (unsigned char c : in) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= width) {
            const int code = (int) ((buffer >> (bits - width)) & ((1u << width) - 1));
            bits -= width;
            if (code == 256) {
                reset();
                width = 9;
                have_prev = false;
                continue;
            }
            if (code == 257)
                return true;
            std::string entry;
            if (code < (int) table.size()) {
                entry = table[(size_t) code];
                if (have_prev)
                    table.push_back(prev + entry[0]);
            } else if (have_prev && code == (int) table.size()) {
                entry = prev + prev[0];
                table.push_back(entry);
            } else {
                return !out.empty();     // damaged
            }
            out += entry;
            prev = entry;
            have_prev = true;
            const int limit = (1 << width) - (early_change ? 1 : 0);
            if ((int) table.size() >= limit && width < 12)
                ++width;
        }
    }
    return true;
}

bool ascii_hex_decode(const std::string &in, std::string &out) {
    out.clear();
    int hi = -1;
    for (unsigned char c : in) {
        if (c == '>')
            break;
        const int v = hex_value(c);
        if (v < 0)
            continue;
        if (hi < 0) {
            hi = v;
        } else {
            out += (char) (hi * 16 + v);
            hi = -1;
        }
    }
    if (hi >= 0)
        out += (char) (hi * 16);
    return true;
}

bool ascii85_decode(const std::string &in, std::string &out) {
    out.clear();
    uint32_t group = 0;
    int n = 0;
    size_t i = 0;
    if (in.size() >= 2 && in[0] == '<' && in[1] == '~')
        i = 2;
    for (; i < in.size(); ++i) {
        const unsigned char c = (unsigned char) in[i];
        if (c == '~')
            break;
        if (is_space(c))
            continue;
        if (c == 'z' && n == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u')
            return false;
        group = group * 85 + (c - '!');
        if (++n == 5) {
            for (int k = 3; k >= 0; --k)
                out += (char) ((group >> (8 * k)) & 0xFF);
            group = 0;
            n = 0;
        }
    }
    if (n > 1) {
        for (int k = n; k < 5; ++k)
            group = group * 85 + 84;
        for (int k = 0; k < n - 1; ++k)
            out += (char) ((group >> (24 - 8 * k)) & 0xFF);
    }
    return true;
}

bool run_length_decode(const std::string &in, std::string &out) {
    out.clear();
    for (size_t i = 0; i < in.size();) {
        const int len = (unsigned char) in[i++];
        if (len == 128)
            break;
        if (len < 128) {
            const size_t n = std::min<size_t>((size_t) len + 1, in.size() - i);
            out.append(in, i, n);
            i += n;
        } else if (i < in.size()) {
            out.append((size_t) (257 - len), in[i++]);
        }
    }
    return true;
}

bool unpredict(std::string &data, int predictor, int colors, int bpc, int columns) {
    if (predictor < 2)
        return true;
    colors = std::max(1, colors);
    bpc = std::max(1, bpc);
    columns = std::max(1, columns);
    const size_t row = ((size_t) colors * bpc * columns + 7) / 8;
    const size_t bpp = std::max<size_t>(1, ((size_t) colors * bpc + 7) / 8);
    if (predictor == 2) {                // TIFF: 8-bit components only
        if (bpc != 8)
            return true;
        for (size_t r = 0; r + row <= data.size(); r += row)
            for (size_t i = bpp; i < row; ++i)
                data[r + i] = (char) ((unsigned char) data[r + i] + (unsigned char) data[r + i - bpp]);
        return true;
    }
    /* PNG: a filter byte before each row */
    std::string out;
    out.reserve(data.size());
    std::vector<unsigned char> prev(row, 0), cur(row, 0);
    size_t at = 0;
    while (at + 1 <= data.size()) {
        const int filter = (unsigned char) data[at++];
        const size_t take = std::min(row, data.size() - at);
        if (take == 0)
            break;
        std::fill(cur.begin(), cur.end(), 0);
        std::memcpy(cur.data(), data.data() + at, take);
        at += take;
        for (size_t i = 0; i < row; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0;
            const int b = prev[i];
            const int c = i >= bpp ? prev[i - bpp] : 0;
            int v = cur[i];
            switch (filter) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            cur[i] = (unsigned char) v;
        }
        out.append((const char *) cur.data(), take);
        std::swap(prev, cur);
    }
    data = std::move(out);
    return true;
}

/* ---- Document -------------------------------------------------------------- */

const Object &Document::resolve(const Object &o) const {
    return o.type == Type::Ref ? object(o.num) : o;
}

const Object &Document::get(const Object &dict, const char *key) const {
    const Object *v = dict.get(key);
    return v ? resolve(*v) : null_object();
}

const Object &Document::page(int i) const {
    return i >= 0 && i < (int) m_pages.size() ? m_pages[(size_t) i] : null_object();
}

bool Document::open(const uint8_t *data, size_t n, std::string *error) {
    m_bytes = View{data, n};
    m_xref.clear();
    m_cache.clear();
    m_pages.clear();
    m_trailer = Object();
    const size_t head = std::min<size_t>(n, 1024);
    bool pdf = false;
    for (size_t i = 0; i + 5 <= head && !pdf; ++i)
        pdf = std::memcmp(data + i, "%PDF-", 5) == 0;
    if (!pdf) {
        if (error)
            *error = "not a PDF";
        return false;
    }
    /* startxref, near the end */
    bool ok = false;
    const size_t tail = n > 4096 ? n - 4096 : 0;
    const std::string end((const char *) data + tail, n - tail);
    const size_t sx = end.rfind("startxref");
    if (sx != std::string::npos) {
        Lexer lx(data, n, tail + sx + 9);
        Object off;
        if (lx.next(off) && off.is(Type::Int) && off.number > 0 && off.number < (double) n)
            ok = read_xref((size_t) off.number, nullptr, 0);
    }
    if (!ok || !m_trailer.get("Root") || resolve(*m_trailer.get("Root")).type != Type::Dict)
        if (!rebuild()) {
            if (error)
                *error = "the PDF's cross-reference couldn't be read";
            return false;
        }
    if (m_trailer.get("Encrypt") && resolve(*m_trailer.get("Encrypt")).type != Type::Null) {
        if (error)
            *error = "the PDF is encrypted";
        return false;
    }
    const Object &root = get(m_trailer, "Root");
    const Object &pages = get(root, "Pages");
    collect_pages(pages, Object(), 0);
    if (m_pages.empty()) {
        if (error)
            *error = "the PDF has no pages";
        return false;
    }
    return true;
}

void Document::merge_trailer(const Object &t) {
    if (!t.dict)
        return;
    if (!m_trailer.dict) {
        m_trailer.type = Type::Dict;
        m_trailer.dict = std::make_shared<Dict>();
    }
    for (const auto &kv : *t.dict)
        if (kv.first != "Prev" && kv.first != "XRefStm" && !m_trailer.dict->count(kv.first))
            (*m_trailer.dict)[kv.first] = kv.second;
}

bool Document::read_xref(size_t at, std::string *error, int depth) {
    if (depth > 64 || at >= m_bytes.size())
        return false;
    Lexer lx(m_bytes.data(), m_bytes.size(), at);
    lx.skip_space();
    const size_t p = lx.pos();
    if (p + 4 <= m_bytes.size() && std::memcmp(m_bytes.data() + p, "xref", 4) == 0)
        return read_xref_table(p + 4, error, depth);
    return read_xref_stream(p, error, depth);
}

bool Document::read_xref_table(size_t at, std::string *error, int depth) {
    Lexer lx(m_bytes.data(), m_bytes.size(), at);
    Object a;
    for (;;) {
        if (!lx.next(a))
            return false;
        if (a.is(Type::Keyword) && a.str == "trailer")
            break;
        Object count;
        if (!a.is(Type::Int) || !lx.next(count) || !count.is(Type::Int))
            return false;
        const int first = a.as_int(), n = count.as_int();
        for (int i = 0; i < n; ++i) {
            Object off, gen, kind;
            if (!lx.next(off) || !lx.next(gen) || !lx.next(kind))
                return false;
            if (!m_xref.count(first + i)) {
                Entry e;
                if (kind.is(Type::Keyword) && kind.str == "n" && off.number > 0) {
                    e.kind = Entry::Plain;
                    e.offset = (size_t) off.number;
                }
                m_xref[first + i] = e;
            }
        }
    }
    Object trailer;
    if (!lx.next(trailer) || !trailer.is(Type::Dict))
        return false;
    merge_trailer(trailer);
    if (const Object *stm = trailer.get("XRefStm"))
        if (stm->is_number())
            read_xref_stream((size_t) stm->number, error, depth + 1);
    if (const Object *prev = trailer.get("Prev"))
        if (prev->is_number())
            read_xref((size_t) prev->number, error, depth + 1);
    return true;
}

bool Document::read_xref_stream(size_t at, std::string *error, int depth) {
    Object xs;
    if (!parse_at(at, -1, xs) || !xs.is(Type::Stream) || !xs.get("W"))
        return false;
    const Object &w = *xs.get("W");
    if (w.size() < 3)
        return false;
    const int w1 = w[0].as_int(), w2 = w[1].as_int(), w3 = w[2].as_int();
    std::string bytes;
    if (!stream_bytes(xs, bytes))
        return false;
    std::vector<std::pair<int, int>> ranges;
    if (const Object *index = xs.get("Index")) {
        for (size_t i = 0; i + 1 < index->size(); i += 2)
            ranges.emplace_back((*index)[i].as_int(), (*index)[i + 1].as_int());
    } else if (const Object *size = xs.get("Size")) {
        ranges.emplace_back(0, size->as_int());
    }
    const size_t row = (size_t) (w1 + w2 + w3);
    if (row == 0)
        return false;
    auto field = [&](size_t at_, int width, uint64_t fallback) {
        if (width == 0)
            return fallback;
        uint64_t v = 0;
        for (int i = 0; i < width && at_ + i < bytes.size(); ++i)
            v = (v << 8) | (unsigned char) bytes[at_ + (size_t) i];
        return v;
    };
    size_t pos = 0;
    for (const auto &r : ranges)
        for (int i = 0; i < r.second && pos + row <= bytes.size(); ++i, pos += row) {
            const int num = r.first + i;
            const uint64_t type = field(pos, w1, 1);
            const uint64_t f2 = field(pos + (size_t) w1, w2, 0);
            const uint64_t f3 = field(pos + (size_t) w1 + (size_t) w2, w3, 0);
            if (m_xref.count(num))
                continue;
            Entry e;
            if (type == 1) {
                e.kind = Entry::Plain;
                e.offset = (size_t) f2;
            } else if (type == 2) {
                e.kind = Entry::InStream;
                e.stream = (int) f2;
                e.index = (int) f3;
            }
            m_xref[num] = e;
        }
    merge_trailer(xs);
    if (const Object *prev = xs.get("Prev"))
        if (prev->is_number())
            read_xref((size_t) prev->number, error, depth + 1);
    return true;
}

bool Document::parse_at(size_t offset, int num, Object &out) const {
    if (offset >= m_bytes.size())
        return false;
    Lexer lx(m_bytes.data(), m_bytes.size(), offset);
    Object n, g, kw;
    if (!lx.next(n) || !lx.next(g) || !lx.next(kw) || !n.is(Type::Int) || !kw.is(Type::Keyword) ||
        kw.str != "obj")
        return false;
    if (num >= 0 && n.as_int() != num)
        return false;
    if (!lx.next(out))
        return false;
    if (out.type != Type::Dict)
        return true;
    /* A stream follows its dictionary */
    const size_t after_dict = lx.pos();
    Object s;
    if (!lx.next(s) || !s.is(Type::Keyword) || s.str != "stream") {
        lx.seek(after_dict);
        return true;
    }
    size_t start = lx.pos();
    if (start < m_bytes.size() && m_bytes[start] == '\r')
        ++start;
    if (start < m_bytes.size() && m_bytes[start] == '\n')
        ++start;
    size_t len = 0;
    bool have = false;
    if (const Object *l = out.get("Length")) {
        const Object &lv = l->is(Type::Ref) && l->num != num ? resolve(*l) : *l;
        if (lv.is_number() && lv.number >= 0 && start + (size_t) lv.number <= m_bytes.size()) {
            len = (size_t) lv.number;
            /* The length is right only if endstream follows it */
            Lexer check(m_bytes.data(), m_bytes.size(), start + len);
            Object e;
            have = check.next(e) && e.is(Type::Keyword) && e.str == "endstream";
        }
    }
    if (!have) {
        const std::string_view hay((const char *) m_bytes.data() + start, m_bytes.size() - start);
        const size_t e = hay.find("endstream");
        len = e == std::string::npos ? hay.size() : e;
        while (len > 0 && (hay[len - 1] == '\n' || hay[len - 1] == '\r'))
            --len;
    }
    out.type = Type::Stream;
    out.data = std::make_shared<const std::string>((const char *) m_bytes.data() + start, len);
    return true;
}

bool Document::load_object_stream(int num) const {
    const Object &stm = object(num);
    if (!stm.is(Type::Stream))
        return false;
    std::string data;
    if (!stream_bytes(stm, data))
        return false;
    const int n = get(stm, "N").as_int(), first = get(stm, "First").as_int();
    if (n <= 0 || first < 0 || (size_t) first > data.size())
        return false;
    Lexer head((const uint8_t *) data.data(), data.size());
    std::vector<std::pair<int, size_t>> objs;
    for (int i = 0; i < n; ++i) {
        Object on, off;
        if (!head.next(on) || !head.next(off) || !on.is(Type::Int) || !off.is(Type::Int))
            break;
        objs.emplace_back(on.as_int(), (size_t) first + (size_t) off.number);
    }
    for (const auto &o : objs) {
        auto e = m_xref.find(o.first);
        const bool here = e == m_xref.end() || (e->second.kind == Entry::InStream && e->second.stream == num);
        if (!here || m_cache.count(o.first) || o.second >= data.size())
            continue;
        Lexer lx((const uint8_t *) data.data(), data.size(), o.second);
        Object v;
        if (lx.next(v))
            m_cache[o.first] = std::move(v);
    }
    return true;
}

const Object &Document::object(int num) const {
    auto c = m_cache.find(num);
    if (c != m_cache.end())
        return c->second;
    auto e = m_xref.find(num);
    if (e == m_xref.end() || e->second.kind == Entry::Free || m_loading[num])
        return null_object();
    m_loading[num] = true;
    Object o;
    if (e->second.kind == Entry::Plain) {
        if (parse_at(e->second.offset, num, o))
            m_cache[num] = std::move(o);
    } else if (load_object_stream(e->second.stream)) {
        /* load_object_stream filled the cache */
    }
    m_loading[num] = false;
    c = m_cache.find(num);
    return c != m_cache.end() ? c->second : null_object();
}

bool Document::rebuild() {
    m_xref.clear();
    m_cache.clear();
    m_trailer = Object();
    const size_t n = m_bytes.size();
    std::vector<int> streams;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isdigit(m_bytes[i]) || (i > 0 && !is_space(m_bytes[i - 1])))
            continue;
        Lexer lx(m_bytes.data(), n, i);
        Object a, b, kw;
        if (!lx.next(a) || !a.is(Type::Int) || !lx.next(b) || !b.is(Type::Int) || !lx.next(kw) ||
            !kw.is(Type::Keyword) || kw.str != "obj")
            continue;
        Entry e;
        e.kind = Entry::Plain;
        e.offset = i;
        m_xref[a.as_int()] = e;          // a later copy wins
        i = lx.pos() - 1;
    }
    /* Trailers, the last first; and objects packed in object streams */
    const std::string_view hay((const char *) m_bytes.data(), n);
    for (size_t at = hay.rfind("trailer"); at != std::string::npos;
         at = at ? hay.rfind("trailer", at - 1) : std::string::npos) {
        Lexer lx(m_bytes.data(), n, at + 7);
        Object t;
        if (lx.next(t) && t.is(Type::Dict))
            merge_trailer(t);
        if (at == 0)
            break;
    }
    std::vector<int> nums;
    for (const auto &kv : m_xref)
        nums.push_back(kv.first);
    for (int num : nums) {
        const Object &o = object(num);
        if (o.is(Type::Stream) && get(o, "Type").is_name("ObjStm")) {
            std::string data;
            if (!stream_bytes(o, data))
                continue;
            Lexer head((const uint8_t *) data.data(), data.size());
            const int count = get(o, "N").as_int();
            for (int k = 0; k < count; ++k) {
                Object on, off;
                if (!head.next(on) || !head.next(off) || !on.is(Type::Int))
                    break;
                if (!m_xref.count(on.as_int())) {
                    Entry e;
                    e.kind = Entry::InStream;
                    e.stream = num;
                    e.index = k;
                    m_xref[on.as_int()] = e;
                }
            }
        } else if (o.is(Type::Stream) && get(o, "Type").is_name("XRef")) {
            merge_trailer(o);
        }
    }
    if (!m_trailer.get("Root")) {
        for (const auto &kv : m_xref) {
            const Object &o = object(kv.first);
            if (o.is(Type::Dict) && get(o, "Type").is_name("Catalog")) {
                Object r;
                r.type = Type::Ref;
                r.num = kv.first;
                Object t;
                t.type = Type::Dict;
                t.dict = std::make_shared<Dict>();
                (*t.dict)["Root"] = r;
                merge_trailer(t);
                break;
            }
        }
    }
    return m_trailer.get("Root") != nullptr;
}

void Document::collect_pages(const Object &node, const Object &inherited, int depth) {
    if (depth > 64 || !node.dict || m_pages.size() > 100000)
        return;
    /* What a page inherits from its ancestors */
    Object inh = inherited;
    if (!inh.dict) {
        inh.type = Type::Dict;
        inh.dict = std::make_shared<Dict>();
    } else {
        inh.dict = std::make_shared<Dict>(*inherited.dict);
    }
    for (const char *k : {"Resources", "MediaBox", "CropBox", "Rotate"})
        if (const Object *v = node.get(k))
            (*inh.dict)[k] = *v;
    const Object &kids = get(node, "Kids");
    if (kids.is(Type::Array) && !get(node, "Type").is_name("Page")) {
        for (size_t i = 0; i < kids.size(); ++i) {
            const Object &kid = resolve(kids[i]);
            if (&kid == &node)
                continue;
            collect_pages(kid, inh, depth + 1);
        }
        return;
    }
    Object page = node;
    page.dict = std::make_shared<Dict>(*node.dict);
    for (const auto &kv : *inh.dict)
        if (!page.dict->count(kv.first))
            (*page.dict)[kv.first] = kv.second;
    m_pages.push_back(std::move(page));
}

bool Document::stream_bytes(const Object &stream, std::string &out, std::string *stopped) const {
    out.clear();
    if (stopped)
        stopped->clear();
    if (!stream.is(Type::Stream) || !stream.data)
        return false;
    out = *stream.data;
    const Object &f = get(stream, "Filter");
    const Object &p = get(stream, "DecodeParms");
    std::vector<std::string> filters;
    std::vector<const Object *> parms;
    if (f.is(Type::Name)) {
        filters.push_back(f.str);
        parms.push_back(&p);
    } else if (f.is(Type::Array)) {
        for (size_t i = 0; i < f.size(); ++i) {
            const Object &fi = resolve(f[i]);
            if (!fi.is(Type::Name))
                continue;
            filters.push_back(fi.str);
            parms.push_back(p.is(Type::Array) ? &resolve(p[i]) : &p);
        }
    }
    for (size_t i = 0; i < filters.size(); ++i) {
        const std::string &name = filters[i];
        const Object &parm = *parms[i];
        std::string next;
        if (name == "FlateDecode" || name == "Fl" || name == "LZWDecode" || name == "LZW") {
            const bool flate = name[0] == 'F';
            const bool ok = flate ? flate_decode(out, next)
                                  : lzw_decode(out, next, get(parm, "EarlyChange").as_int(1) != 0);
            if (!ok)
                return false;
            if (parm.dict) {
                const int pred = get(parm, "Predictor").as_int(1);
                if (pred > 1)
                    unpredict(next, pred, get(parm, "Colors").as_int(1),
                              get(parm, "BitsPerComponent").as_int(8), get(parm, "Columns").as_int(1));
            }
        } else if (name == "ASCIIHexDecode" || name == "AHx") {
            ascii_hex_decode(out, next);
        } else if (name == "ASCII85Decode" || name == "A85") {
            if (!ascii85_decode(out, next))
                return false;
        } else if (name == "RunLengthDecode" || name == "RL") {
            run_length_decode(out, next);
        } else if (name == "DCTDecode" || name == "DCT" || name == "JPXDecode" || name == "CCITTFaxDecode" ||
                   name == "CCF" || name == "JBIG2Decode" || name == "Crypt") {
            if (stopped)
                *stopped = name == "DCT" ? "DCTDecode" : name == "CCF" ? "CCITTFaxDecode" : name;
            return true;                 // the caller reads that one
        } else {
            return false;                // a filter this reader doesn't know
        }
        out = std::move(next);
    }
    return true;
}

} // namespace pdf
} // namespace pagemade
