/*
 * pagemade/json_util.h — the JSON writer and reader helpers shared by the
 * publication file (docfile.cpp) and the layout templates.
 *
 * JsonOut writes floats in their shortest round-trip form, so files stay
 * readable and diffable. Reading goes through dict.h; the helpers below
 * take a value that may be missing or of the wrong type and leave the
 * output alone then, so a file only overrides the defaults it names.
 */
#pragma once

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "dict.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "page.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <vector>

/* Floating-point to_chars came to libc++ in 14 (LLVM 10's, on an old Mac,
 * has only the integer forms). */
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 14000
#define PAGEMADE_FLOAT_TO_CHARS 0
#else
#define PAGEMADE_FLOAT_TO_CHARS 1
#endif

namespace pagemade {
namespace json {

class JsonOut {
public:
    std::string text;

    JsonOut &key(const char *k) {
        prefix();
        quote(k);
        text += ": ";
        m_after_key = true;
        return *this;
    }
    void begin_object() { prefix(); text += '{'; m_first.push_back(true); }
    void end_object()   { close('}'); }
    void begin_array()  { prefix(); text += '['; m_first.push_back(true); }
    void end_array()    { close(']'); }
    void value(const std::string &s) { prefix(); quote(s); }
    void value(const char *s)        { value(std::string(s)); }
    void value(bool b)               { prefix(); text += b ? "true" : "false"; }
    void value(float f)              { prefix(); number(f); }
    void value(double d)             { prefix(); number(d); }
    void value(int i)                { prefix(); text += std::to_string(i); }
    void value(uint32_t u)           { prefix(); text += std::to_string(u); }
    /* A short list of numbers on one line: [1, 0, 0, 1, 54, 54]. */
    void numbers(std::initializer_list<float> v) {
        prefix();
        text += '[';
        bool first = true;
        for (float f : v) {
            if (!first) text += ", ";
            first = false;
            number(f);
        }
        text += ']';
    }
    void ids(const std::vector<uint32_t> &v) {
        prefix();
        text += '[';
        for (size_t i = 0; i < v.size(); ++i)
            text += (i ? ", " : "") + std::to_string(v[i]);
        text += ']';
    }

private:
    std::vector<bool> m_first;   // per open container: nothing written yet
    bool m_after_key = false;

    void newline() {
        text += '\n';
        text.append(m_first.size() * 2, ' ');
    }
    void prefix() {
        if (m_after_key) {
            m_after_key = false;
            return;
        }
        if (m_first.empty())
            return;
        if (!m_first.back())
            text += ',';
        m_first.back() = false;
        newline();
    }
    void close(char c) {
        const bool empty = m_first.back();
        m_first.pop_back();
        if (!empty)
            newline();
        text += c;
    }
    void number(float f) {
        if (!std::isfinite(f))
            f = 0.f;
        char buf[32];
#if PAGEMADE_FLOAT_TO_CHARS
        auto r = std::to_chars(buf, buf + sizeof buf, f);
        text.append(buf, r.ptr);
#else
        text.append(buf, shortest_digits(buf, sizeof buf, f));
#endif
    }
    void number(double d) {
        if (!std::isfinite(d))
            d = 0;
        char buf[64];
#if PAGEMADE_FLOAT_TO_CHARS
        auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::general);
        text.append(buf, r.ptr);
#else
        text.append(buf, shortest_digits(buf, sizeof buf, d));
#endif
    }
#if !PAGEMADE_FLOAT_TO_CHARS
    /* The fewest %g digits that read back to the same value, as to_chars
     * writes them. The read back uses the current locale, as snprintf
     * does; the decimal mark is then made a point. */
    template <typename T> static size_t shortest_digits(char *buf, size_t n, T v) {
        const int most = std::is_same<T, float>::value ? 9 : 17;
        int len = 0;
        for (int digits = 1; digits <= most; ++digits) {
            len = std::snprintf(buf, n, "%.*g", digits, (double) v);
            if (len <= 0 || (size_t) len >= n)
                break;
            const T back = std::is_same<T, float>::value ? (T) std::strtof(buf, nullptr)
                                                         : (T) std::strtod(buf, nullptr);
            if (back == v)
                break;
        }
        for (int i = 0; i < len; ++i)
            if (buf[i] == ',')
                buf[i] = '.';
        return len > 0 ? (size_t) len : 0;
    }
#endif
    void quote(const std::string &s) {
        static const char digits[] = "0123456789abcdef";
        text += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"':  text += "\\\""; break;
            case '\\': text += "\\\\"; break;
            case '\n': text += "\\n"; break;
            case '\r': text += "\\r"; break;
            case '\t': text += "\\t"; break;
            default:
                if (c < 0x20) {
                    text += "\\u00";
                    text += digits[c >> 4];
                    text += digits[c & 15];
                } else {
                    text += (char) c;    // UTF-8 passes through
                }
            }
        }
        text += '"';
    }
};

/* ---- Names for enums, as both files spell them ----------------------------- */

inline const char *align_name(Align a) {
    switch (a) {
    case Align::Left: return "left";
    case Align::Center: return "center";
    case Align::Right: return "right";
    case Align::Justify: return "justify";
    case Align::ForceJustify: return "force-justify";
    case Align::JustifyCenter: return "justify-center";
    case Align::JustifyRight: return "justify-right";
    }
    return "left";
}
inline Align align_from(const std::string &s) {
    return s == "center" ? Align::Center : s == "right" ? Align::Right :
           s == "justify" ? Align::Justify : s == "force-justify" ? Align::ForceJustify :
           s == "justify-center" ? Align::JustifyCenter :
           s == "justify-right" ? Align::JustifyRight : Align::Left;
}

inline const char *line_style_name(LineStyle l) {
    switch (l) {
    case LineStyle::Solid: return "solid";
    case LineStyle::Dashed: return "dashed";
    case LineStyle::Dotted: return "dotted";
    case LineStyle::DashDot: return "dash-dot";
    case LineStyle::Double: return "double";
    case LineStyle::ThickThin: return "thick-thin";
    case LineStyle::ThinThick: return "thin-thick";
    case LineStyle::ThinThickThin: return "thin-thick-thin";
    }
    return "solid";
}
inline LineStyle line_style_from(const std::string &s) {
    for (LineStyle l : {LineStyle::Dashed, LineStyle::Dotted, LineStyle::DashDot, LineStyle::Double,
                        LineStyle::ThickThin, LineStyle::ThinThick, LineStyle::ThinThickThin})
        if (s == line_style_name(l))
            return l;
    return LineStyle::Solid;
}

inline const char *shape_name(Shape::Kind k) {
    switch (k) {
    case Shape::Kind::Rect: return "rectangle";
    case Shape::Kind::Ellipse: return "ellipse";
    case Shape::Kind::Line: return "line";
    case Shape::Kind::Polygon: return "polygon";
    }
    return "rectangle";
}
inline Shape::Kind shape_from(const std::string &s) {
    return s == "ellipse" ? Shape::Kind::Ellipse : s == "line" ? Shape::Kind::Line :
           s == "polygon" ? Shape::Kind::Polygon : Shape::Kind::Rect;
}

/* ---- Reading ------------------------------------------------------------- */

inline const DictValue *field(const DictValue *o, const char *k) {
    return o && o->type == DICT_OBJECT ? dict_object_get(o, k) : nullptr;
}
inline size_t count(const DictValue *a) {
    return a && a->type == DICT_ARRAY ? a->array_value.length : 0;
}
inline const DictValue *at(const DictValue *a, size_t i) {
    return a && a->type == DICT_ARRAY && i < a->array_value.length ? a->array_value.items[i]
                                                                   : nullptr;
}

inline bool number(const DictValue *v, double &out) {
    if (v && v->type == DICT_NUMBER) { out = v->number_value; return true; }
    if (v && v->type == DICT_INT64) { out = (double) v->int64_value; return true; }
    return false;
}
inline void get(const DictValue *o, const char *k, float &out) {
    double d;
    if (number(field(o, k), d) && std::isfinite(d)) out = (float) d;
}
inline void get(const DictValue *o, const char *k, int &out) {
    double d;
    if (number(field(o, k), d) && std::isfinite(d)) out = (int) d;
}
inline void get(const DictValue *o, const char *k, uint32_t &out) {
    double d;
    if (number(field(o, k), d) && d >= 0 && d <= 4294967295.0) out = (uint32_t) d;
}
inline void get(const DictValue *o, const char *k, bool &out) {
    const DictValue *v = field(o, k);
    if (v && v->type == DICT_BOOL) out = v->bool_value != 0;
}
inline void get(const DictValue *o, const char *k, std::string &out) {
    const DictValue *v = field(o, k);
    if (v && v->type == DICT_STRING && v->string_value) out = v->string_value;
}
/* Up to n numbers from an array field. */
inline size_t floats(const DictValue *o, const char *k, float *out, size_t n) {
    const DictValue *a = field(o, k);
    size_t m = std::min(n, count(a));
    for (size_t i = 0; i < m; ++i) {
        double d;
        if (number(at(a, i), d)) out[i] = (float) d;
    }
    return m;
}
inline void get(const DictValue *o, const char *k, Color &c) {
    float v[4] = {c.r, c.g, c.b, c.a};
    if (floats(o, k, v, 4) >= 3)
        c = {v[0], v[1], v[2], v[3]};
}

/* Parses `text`; null (and `error`) when nothing could be read. Free with
 * dict_destroy. */
inline DictValue *parse(const std::string &text, std::string *error) {
    char err[256] = {0};
    DictValue *root = text.empty() ? nullptr
        : dict_deserialize_json(text.data(), text.size(), text.size(), err, sizeof err);
    if (!root && error)
        *error = *err ? std::string(err) : std::string("empty file");
    return root;
}

/* dict_destroy at the end of a scope. */
struct Owned {
    DictValue *v = nullptr;
    explicit Owned(DictValue *value) : v(value) {}
    ~Owned() { dict_destroy(v); }
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
};

} // namespace json
} // namespace pagemade
