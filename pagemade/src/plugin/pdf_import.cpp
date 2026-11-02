/*
 * pagemade/plugin/pdf_import.cpp — the PDF decoder: one page of a PDF as
 * a VectorScene, so File > Place, drag and drop and reopening place a PDF
 * the way they place an SVG.
 *
 * The page's content stream is interpreted into the scene's shapes, clips
 * and pictures, in paint order, in the page's crop box (turned by /Rotate)
 * measured in points, y down:
 *
 *   paths, fills (nonzero, even-odd), strokes with dashes, caps and joins;
 *   clipping paths; Gray, RGB, CMYK, ICC-based, Cal, Lab, Indexed,
 *   Separation and DeviceN colors (tint functions of every type: sampled,
 *   exponential, stitching and PostScript calculator); axial and radial
 *   shadings, painted (sh) or as pattern fills and strokes, as gradients;
 *   constant alpha from graphics states; form XObjects;
 *   image XObjects and inline images: a JPEG goes in as it is, anything
 *   else is decoded to pixels (1 to 16 bits, any of those color spaces,
 *   stencil masks, soft masks) and kept as PNG;
 *   text in the PDF's embedded fonts (TrueType, CFF, Type 1), drawn as
 *   outlines through FreeType at the PDF's own glyph positions.
 *
 * A font the PDF names but doesn't embed (the standard 14) is kept as text
 * runs placed glyph by glyph, which the UI thread outlines in the nearest
 * installed face (Helvetica in Nimbus Sans, Times in Nimbus Roman, and so
 * on); its positions still come from the PDF's widths.
 *
 * Not drawn yet, and named on the scene: mesh and function shadings,
 * tiling patterns, Type 3 fonts, JPEG 2000, CCITT and JBIG2 images,
 * soft-mask groups. Plain C++ on
 * FreeType and miniz: the same on macOS, Linux and Windows.
 */
#include "plugin/pdf_parse.h"
#include "plugin/plugin.h"

#include "geometry.h"
#include "image.h"
#include "page.h"
#include "vector.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../../ext/glfw/deps/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace pagemade {

namespace {

using pdf::Object;
using pdf::Type;

void note(VectorScene &scene, const char *what) {
    for (const std::string &s : scene.skipped)
        if (s == what)
            return;
    scene.skipped.emplace_back(what);
}

float clamp01(float v) {
    return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

/* ---- Glyph names and the standard encodings ------------------------------ */

/* Unicode for the glyph names text in the Latin fonts uses; uniXXXX and
 * uXXXX[XX] are read as numbers. 0 when unknown. */
uint32_t glyph_unicode(const std::string &name) {
    static const std::map<std::string, uint32_t> names = {
        {"space", 32}, {"exclam", 33}, {"quotedbl", 34}, {"numbersign", 35}, {"dollar", 36},
        {"percent", 37}, {"ampersand", 38}, {"quotesingle", 39}, {"quoteright", 0x2019},
        {"parenleft", 40}, {"parenright", 41}, {"asterisk", 42}, {"plus", 43}, {"comma", 44},
        {"hyphen", 45}, {"minus", 0x2212}, {"period", 46}, {"slash", 47}, {"zero", 48}, {"one", 49},
        {"two", 50}, {"three", 51}, {"four", 52}, {"five", 53}, {"six", 54}, {"seven", 55},
        {"eight", 56}, {"nine", 57}, {"colon", 58}, {"semicolon", 59}, {"less", 60}, {"equal", 61},
        {"greater", 62}, {"question", 63}, {"at", 64}, {"bracketleft", 91}, {"backslash", 92},
        {"bracketright", 93}, {"asciicircum", 94}, {"underscore", 95}, {"grave", 96},
        {"quoteleft", 0x2018}, {"braceleft", 123}, {"bar", 124}, {"braceright", 125},
        {"asciitilde", 126}, {"exclamdown", 0xA1}, {"cent", 0xA2}, {"sterling", 0xA3},
        {"currency", 0xA4}, {"yen", 0xA5}, {"brokenbar", 0xA6}, {"section", 0xA7},
        {"dieresis", 0xA8}, {"copyright", 0xA9}, {"ordfeminine", 0xAA}, {"guillemotleft", 0xAB},
        {"logicalnot", 0xAC}, {"registered", 0xAE}, {"macron", 0xAF}, {"degree", 0xB0},
        {"plusminus", 0xB1}, {"twosuperior", 0xB2}, {"threesuperior", 0xB3}, {"acute", 0xB4},
        {"mu", 0xB5}, {"paragraph", 0xB6}, {"periodcentered", 0xB7}, {"cedilla", 0xB8},
        {"onesuperior", 0xB9}, {"ordmasculine", 0xBA}, {"guillemotright", 0xBB},
        {"onequarter", 0xBC}, {"onehalf", 0xBD}, {"threequarters", 0xBE}, {"questiondown", 0xBF},
        {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acircumflex", 0xC2}, {"Atilde", 0xC3},
        {"Adieresis", 0xC4}, {"Aring", 0xC5}, {"AE", 0xC6}, {"Ccedilla", 0xC7}, {"Egrave", 0xC8},
        {"Eacute", 0xC9}, {"Ecircumflex", 0xCA}, {"Edieresis", 0xCB}, {"Igrave", 0xCC},
        {"Iacute", 0xCD}, {"Icircumflex", 0xCE}, {"Idieresis", 0xCF}, {"Eth", 0xD0},
        {"Ntilde", 0xD1}, {"Ograve", 0xD2}, {"Oacute", 0xD3}, {"Ocircumflex", 0xD4},
        {"Otilde", 0xD5}, {"Odieresis", 0xD6}, {"multiply", 0xD7}, {"Oslash", 0xD8},
        {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucircumflex", 0xDB}, {"Udieresis", 0xDC},
        {"Yacute", 0xDD}, {"Thorn", 0xDE}, {"germandbls", 0xDF}, {"agrave", 0xE0},
        {"aacute", 0xE1}, {"acircumflex", 0xE2}, {"atilde", 0xE3}, {"adieresis", 0xE4},
        {"aring", 0xE5}, {"ae", 0xE6}, {"ccedilla", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9},
        {"ecircumflex", 0xEA}, {"edieresis", 0xEB}, {"igrave", 0xEC}, {"iacute", 0xED},
        {"icircumflex", 0xEE}, {"idieresis", 0xEF}, {"eth", 0xF0}, {"ntilde", 0xF1},
        {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocircumflex", 0xF4}, {"otilde", 0xF5},
        {"odieresis", 0xF6}, {"divide", 0xF7}, {"oslash", 0xF8}, {"ugrave", 0xF9},
        {"uacute", 0xFA}, {"ucircumflex", 0xFB}, {"udieresis", 0xFC}, {"yacute", 0xFD},
        {"thorn", 0xFE}, {"ydieresis", 0xFF}, {"OE", 0x152}, {"oe", 0x153}, {"Scaron", 0x160},
        {"scaron", 0x161}, {"Ydieresis", 0x178}, {"Zcaron", 0x17D}, {"zcaron", 0x17E},
        {"florin", 0x192}, {"circumflex", 0x2C6}, {"tilde", 0x2DC}, {"endash", 0x2013},
        {"emdash", 0x2014}, {"quotesinglbase", 0x201A}, {"quotedblleft", 0x201C},
        {"quotedblright", 0x201D}, {"quotedblbase", 0x201E}, {"dagger", 0x2020},
        {"daggerdbl", 0x2021}, {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"perthousand", 0x2030},
        {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"Euro", 0x20AC},
        {"trademark", 0x2122}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"dotlessi", 0x131},
        {"nbspace", 0xA0}, {"sfthyphen", 0xAD},
    };
    if (name.size() == 1 && std::isalpha((unsigned char) name[0]))
        return (uint32_t) (unsigned char) name[0];
    auto it = names.find(name);
    if (it != names.end())
        return it->second;
    if (name.size() == 7 && name.compare(0, 3, "uni") == 0)
        return (uint32_t) std::strtoul(name.c_str() + 3, nullptr, 16);
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u')
        return (uint32_t) std::strtoul(name.c_str() + 1, nullptr, 16);
    return 0;
}

/* WinAnsiEncoding, codes 128 to 159 (the rest is Latin-1) */
uint32_t win_ansi(int code) {
    static const uint16_t high[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
        0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    if (code >= 128 && code < 160)
        return high[code - 128];
    return (uint32_t) code;
}

/* MacRomanEncoding, codes 128 and up */
uint32_t mac_roman(int code) {
    static const uint16_t high[128] = {
        0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
        0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
        0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
        0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
        0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
        0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
        0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
        0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7};
    if (code >= 128 && code < 256)
        return high[code - 128];
    return (uint32_t) code;
}

/* StandardEncoding differs from ASCII at two codes below 128 */
uint32_t standard_enc(int code) {
    if (code == 0x27)
        return 0x2019;
    if (code == 0x60)
        return 0x2018;
    return code < 128 ? (uint32_t) code : win_ansi(code);
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char) cp;
    } else if (cp < 0x800) {
        out += (char) (0xC0 | cp >> 6);
        out += (char) (0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        out += (char) (0xE0 | cp >> 12);
        out += (char) (0x80 | ((cp >> 6) & 63));
        out += (char) (0x80 | (cp & 63));
    } else {
        out += (char) (0xF0 | cp >> 18);
        out += (char) (0x80 | ((cp >> 12) & 63));
        out += (char) (0x80 | ((cp >> 6) & 63));
        out += (char) (0x80 | (cp & 63));
    }
}

/* ---- Fonts ------------------------------------------------------------------- */

struct PdfFont {
    bool ok = false;
    bool type3 = false;
    bool composite = false;              // Type0: codes of `code_bytes`, CIDs
    int code_bytes = 1;
    /* A two-byte code to a CID (an embedded CMap); identity when empty */
    std::map<uint32_t, uint32_t> cid_of;
    std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> cid_ranges;   // lo, (hi, first CID)
    std::vector<uint16_t> cid_to_gid;    // CIDFontType2's map; identity when empty
    /* Widths in thousandths of the size: simple fonts by code, composite by CID */
    std::map<uint32_t, float> widths;
    float default_width = 1000.f;
    float missing_width = 0.f;
    /* An embedded program */
    FT_Face face = nullptr;
    std::shared_ptr<std::string> program;      // FreeType reads from here
    float units = 1000.f;                // units per em
    std::vector<int> code_gid;           // simple fonts: code -> glyph index
    /* Code -> text (ToUnicode, else the encoding) */
    std::map<uint32_t, std::string> text;
    /* Not embedded: the families to draw it in, and its style */
    std::vector<std::string> families;
    int weight = 400;
    bool italic = false;
    std::map<int, Path> outlines;        // glyph index -> outline, font units, y up

    uint32_t cid(uint32_t code) const {
        if (!composite)
            return code;
        auto it = cid_of.find(code);
        if (it != cid_of.end())
            return it->second;
        for (const auto &r : cid_ranges)
            if (code >= r.first && code <= r.second.first)
                return r.second.second + (code - r.first);
        return code;                     // Identity-H
    }
    float width(uint32_t code) const {
        auto it = widths.find(composite ? cid(code) : code);
        if (it != widths.end())
            return it->second;
        return composite ? default_width : missing_width;
    }
    int glyph(uint32_t code) const {
        if (composite) {
            const uint32_t c = cid(code);
            if (!cid_to_gid.empty())
                return c < cid_to_gid.size() ? cid_to_gid[c] : 0;
            return (int) c;
        }
        return code < code_gid.size() ? code_gid[code] : 0;
    }
};

/* The ToUnicode CMap and embedded encoding CMaps: begin/endbfchar,
 * bfrange, cidchar and cidrange, enough to read what writers produce */
void parse_cmap(const std::string &s, PdfFont &f, bool to_unicode) {
    pdf::Lexer lx((const uint8_t *) s.data(), s.size());
    std::vector<Object> ops;
    Object o;
    auto code_value = [](const std::string &bytes) {
        uint32_t v = 0;
        for (unsigned char c : bytes)
            v = (v << 8) | c;
        return v;
    };
    auto utf16 = [](const std::string &bytes) {
        std::string out;
        for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
            uint32_t u = ((unsigned char) bytes[i] << 8) | (unsigned char) bytes[i + 1];
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < bytes.size()) {
                const uint32_t lo = ((unsigned char) bytes[i + 2] << 8) | (unsigned char) bytes[i + 3];
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            append_utf8(out, u);
        }
        return out;
    };
    std::vector<Object> args;
    while (lx.next(o)) {
        if (!o.is(Type::Keyword)) {
            args.push_back(o);
            continue;
        }
        const std::string &k = o.str;
        if (k == "begincodespacerange" || k == "beginbfchar" || k == "beginbfrange" ||
            k == "begincidchar" || k == "begincidrange") {
            args.clear();
            continue;
        }
        if (k == "endcodespacerange") {
            for (const Object &a : args)
                if (a.is(Type::String) && !to_unicode)
                    f.code_bytes = std::max(f.code_bytes, (int) a.str.size());
        } else if (k == "endbfchar" && to_unicode) {
            for (size_t i = 0; i + 1 < args.size(); i += 2)
                if (args[i].is(Type::String) && args[i + 1].is(Type::String))
                    f.text[code_value(args[i].str)] = utf16(args[i + 1].str);
        } else if (k == "endbfrange" && to_unicode) {
            for (size_t i = 0; i + 2 < args.size(); i += 3) {
                if (!args[i].is(Type::String) || !args[i + 1].is(Type::String))
                    continue;
                const uint32_t lo = code_value(args[i].str), hi = code_value(args[i + 1].str);
                if (hi < lo || hi - lo > 65535)
                    continue;
                const Object &dst = args[i + 2];
                for (uint32_t c = lo; c <= hi; ++c) {
                    if (dst.is(Type::String)) {
                        std::string b = dst.str;
                        if (!b.empty()) {
                            uint32_t last = (unsigned char) b.back() + (c - lo);
                            b.back() = (char) (last & 0xFF);
                        }
                        f.text[c] = utf16(b);
                    } else if (dst.is(Type::Array) && c - lo < dst.size() && dst[c - lo].is(Type::String)) {
                        f.text[c] = utf16(dst[c - lo].str);
                    }
                }
            }
        } else if (k == "endcidchar" && !to_unicode) {
            for (size_t i = 0; i + 1 < args.size(); i += 2)
                if (args[i].is(Type::String) && args[i + 1].is_number())
                    f.cid_of[code_value(args[i].str)] = (uint32_t) args[i + 1].number;
        } else if (k == "endcidrange" && !to_unicode) {
            for (size_t i = 0; i + 2 < args.size(); i += 3)
                if (args[i].is(Type::String) && args[i + 1].is(Type::String) && args[i + 2].is_number())
                    f.cid_ranges.push_back({code_value(args[i].str),
                                            {code_value(args[i + 1].str), (uint32_t) args[i + 2].number}});
        }
        args.clear();
    }
}

/* The families a font's name stands for among installed faces */
void fallback_families(const std::string &base, PdfFont &f) {
    std::string name = base;
    const size_t plus = name.find('+');
    if (plus == 6)
        name = name.substr(7);           // a subset tag
    std::string lower;
    for (char c : name)
        lower += (char) std::tolower((unsigned char) c);
    auto has = [&](const char *s) { return lower.find(s) != std::string::npos; };
    if (has("bold") || has("black") || has("heavy") || has("semibold") || has("demi"))
        f.weight = 700;
    if (has("italic") || has("oblique"))
        f.italic = true;
    if (has("courier") || has("mono"))
        f.families = {"Nimbus Mono PS", "Liberation Mono", "monospace"};
    else if (has("times") || has("roman") || has("serif") || has("georgia") || has("garamond") || has("book"))
        f.families = {"Nimbus Roman", "Liberation Serif", "serif"};
    else if (has("symbol"))
        f.families = {"Standard Symbols PS", "serif"};
    else if (has("dingbat"))
        f.families = {"D050000L", "serif"};
    else
        f.families = {"Nimbus Sans", "Liberation Sans", "sans-serif"};
    /* The PDF's own name first, in case that family is installed */
    const size_t dash = name.find_first_of("-,");
    f.families.insert(f.families.begin(), dash == std::string::npos ? name : name.substr(0, dash));
}

void load_glyph(PdfFont &f, int gid, Path &out) {
    out.clear();
    if (!f.face || FT_Load_Glyph(f.face, (FT_UInt) gid,
                                 FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
        return;
    FT_Outline &ol = f.face->glyph->outline;
    struct Ctx {
        Path *p;
        float x = 0, y = 0;
    } ctx{&out};
    FT_Outline_Funcs fn;
    fn.move_to = [](const FT_Vector *to, void *u) {
        Ctx *c = (Ctx *) u;
        if (!c->p->empty())
            c->p->push_back({PathCmd::Close, 0, 0, 0, 0, 0, 0});
        c->p->push_back({PathCmd::Move, 0, 0, 0, 0, (float) to->x, (float) to->y});
        c->x = (float) to->x;
        c->y = (float) to->y;
        return 0;
    };
    fn.line_to = [](const FT_Vector *to, void *u) {
        Ctx *c = (Ctx *) u;
        c->p->push_back({PathCmd::Line, 0, 0, 0, 0, (float) to->x, (float) to->y});
        return 0;
    };
    fn.conic_to = [](const FT_Vector *ctl, const FT_Vector *to, void *u) {
        Ctx *c = (Ctx *) u;
        c->p->push_back({PathCmd::Quad, (float) ctl->x, (float) ctl->y, 0, 0, (float) to->x, (float) to->y});
        return 0;
    };
    fn.cubic_to = [](const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *u) {
        Ctx *c = (Ctx *) u;
        c->p->push_back({PathCmd::Cubic, (float) c1->x, (float) c1->y, (float) c2->x, (float) c2->y,
                         (float) to->x, (float) to->y});
        return 0;
    };
    fn.shift = 0;
    fn.delta = 0;
    FT_Outline_Decompose(&ol, &fn, &ctx);
    if (!out.empty())
        out.push_back({PathCmd::Close, 0, 0, 0, 0, 0, 0});
}

/* ---- Functions (tint transforms) --------------------------------------------- */

class Functions {
public:
    explicit Functions(const pdf::Document &d) : m_doc(d) {}
    /* `fn` (a function or an array of them, one per output) on `in` */
    bool eval(const Object &fn_ref, const std::vector<float> &in, std::vector<float> &out, int depth = 0) const {
        const Object &fn = m_doc.resolve(fn_ref);
        out.clear();
        if (depth > 8)
            return false;
        if (fn.is(Type::Array)) {
            for (size_t i = 0; i < fn.size(); ++i) {
                std::vector<float> one;
                if (!eval(fn[i], in, one, depth + 1) || one.empty())
                    return false;
                out.push_back(one[0]);
            }
            return true;
        }
        const int type = m_doc.get(fn, "FunctionType").as_int(-1);
        const float x = in.empty() ? 0.f : in[0];
        if (type == 2) {
            const Object &c0 = m_doc.get(fn, "C0"), &c1 = m_doc.get(fn, "C1");
            const float n = (float) m_doc.get(fn, "N").as_number(1);
            const size_t k = std::max<size_t>(c0.is(Type::Array) ? c0.size() : 1, c1.is(Type::Array) ? c1.size() : 1);
            const float t = std::pow(std::max(0.f, x), n);
            for (size_t i = 0; i < k; ++i) {
                const float a = c0.is(Type::Array) ? (float) c0[i].as_number(0) : 0.f;
                const float b = c1.is(Type::Array) ? (float) c1[i].as_number(1) : 1.f;
                out.push_back(a + t * (b - a));
            }
            return true;
        }
        if (type == 3) {
            const Object &fns = m_doc.get(fn, "Functions"), &bounds = m_doc.get(fn, "Bounds");
            const Object &encode = m_doc.get(fn, "Encode"), &domain = m_doc.get(fn, "Domain");
            const float d0 = (float) domain[0].as_number(0), d1 = (float) domain[1].as_number(1);
            size_t i = 0;
            while (i < bounds.size() && x >= (float) bounds[i].as_number())
                ++i;
            const float lo = i == 0 ? d0 : (float) bounds[i - 1].as_number();
            const float hi = i < bounds.size() ? (float) bounds[i].as_number() : d1;
            const float e0 = (float) encode[2 * i].as_number(0), e1 = (float) encode[2 * i + 1].as_number(1);
            const float t = hi > lo ? (x - lo) / (hi - lo) : 0.f;
            return i < fns.size() && eval(fns[i], {e0 + t * (e1 - e0)}, out, depth + 1);
        }
        if (type == 0 && fn.is(Type::Stream)) {
            std::string samples;
            if (!m_doc.stream_bytes(fn, samples))
                return false;
            const Object &size = m_doc.get(fn, "Size"), &range = m_doc.get(fn, "Range");
            const int bps = m_doc.get(fn, "BitsPerSample").as_int(8);
            const int count = size[0].as_int(1);
            const size_t outs = range.size() / 2;
            if (count < 1 || outs == 0 || bps <= 0 || bps > 32)
                return false;
            const Object &domain = m_doc.get(fn, "Domain");
            const float d0 = (float) domain[0].as_number(0), d1 = (float) domain[1].as_number(1);
            const float t = d1 > d0 ? clamp01((x - d0) / (d1 - d0)) : 0.f;
            const int idx = std::min(count - 1, (int) std::lround(t * (float) (count - 1)));
            const double maxv = std::pow(2.0, bps) - 1.0;
            for (size_t o = 0; o < outs; ++o) {
                const size_t bit = ((size_t) idx * outs + o) * (size_t) bps;
                uint64_t v = 0;
                for (int b = 0; b < bps; ++b) {
                    const size_t at = bit + (size_t) b;
                    if (at / 8 >= samples.size())
                        break;
                    v = (v << 1) | (((unsigned char) samples[at / 8] >> (7 - at % 8)) & 1);
                }
                const float r0 = (float) range[2 * o].as_number(0), r1 = (float) range[2 * o + 1].as_number(1);
                out.push_back(r0 + (float) (v / maxv) * (r1 - r0));
            }
            return true;
        }
        if (type == 4 && fn.is(Type::Stream)) {
            std::string code;
            if (!m_doc.stream_bytes(fn, code))
                return false;
            const Object &domain = m_doc.get(fn, "Domain"), &range = m_doc.get(fn, "Range");
            std::vector<double> stack;
            for (size_t i = 0; i < in.size(); ++i) {
                double v = in[i];
                if (domain.size() >= 2 * i + 2)
                    v = std::clamp(v, domain[2 * i].as_number(), domain[2 * i + 1].as_number());
                stack.push_back(v);
            }
            if (!run_ps(code, stack))
                return false;
            const size_t outs = range.size() / 2;
            if (outs == 0 || stack.size() < outs)
                return false;
            for (size_t o = 0; o < outs; ++o) {
                const double v = stack[stack.size() - outs + o];
                out.push_back((float) std::clamp(v, range[2 * o].as_number(), range[2 * o + 1].as_number()));
            }
            return true;
        }
        return false;
    }

private:
    /* A type 4 function: PostScript's arithmetic, boolean, stack and
     * if/ifelse operators on a number stack. Programs are parsed once. */
    struct PsOp {
        enum Kind : uint8_t { Num, Op, Block } kind = Num;
        double value = 0;
        std::string op;
        std::vector<PsOp> block;
    };

    static bool parse_ps(const std::string &code, size_t &i, std::vector<PsOp> &out, int depth) {
        while (i < code.size()) {
            const char c = code[i];
            if (std::isspace((unsigned char) c)) {
                ++i;
            } else if (c == '{') {
                ++i;
                PsOp b;
                b.kind = PsOp::Block;
                if (depth > 32 || !parse_ps(code, i, b.block, depth + 1))
                    return false;
                out.push_back(std::move(b));
            } else if (c == '}') {
                ++i;
                return depth > 0;
            } else {
                const size_t start = i;
                while (i < code.size() && !std::isspace((unsigned char) code[i]) && code[i] != '{' && code[i] != '}')
                    ++i;
                const std::string tok = code.substr(start, i - start);
                PsOp op;
                char *end = nullptr;
                const double v = std::strtod(tok.c_str(), &end);
                if (end && *end == '\0' && !tok.empty()) {
                    op.kind = PsOp::Num;
                    op.value = v;
                } else {
                    op.kind = PsOp::Op;
                    op.op = tok;
                }
                out.push_back(std::move(op));
            }
        }
        return depth == 0;
    }

    static bool exec_ps(const std::vector<PsOp> &prog, std::vector<double> &st, int depth) {
        if (depth > 32)
            return false;
        auto pop = [&](double &v) {
            if (st.empty())
                return false;
            v = st.back();
            st.pop_back();
            return true;
        };
        for (size_t k = 0; k < prog.size(); ++k) {
            const PsOp &p = prog[k];
            if (p.kind == PsOp::Num) {
                st.push_back(p.value);
                continue;
            }
            if (p.kind == PsOp::Block) {
                /* `{a} if` or `{a} {b} ifelse` */
                if (k + 1 < prog.size() && prog[k + 1].kind == PsOp::Op && prog[k + 1].op == "if") {
                    double c;
                    if (!pop(c))
                        return false;
                    if (c != 0 && !exec_ps(p.block, st, depth + 1))
                        return false;
                    ++k;
                } else if (k + 2 < prog.size() && prog[k + 1].kind == PsOp::Block &&
                           prog[k + 2].kind == PsOp::Op && prog[k + 2].op == "ifelse") {
                    double c;
                    if (!pop(c))
                        return false;
                    if (!exec_ps(c != 0 ? p.block : prog[k + 1].block, st, depth + 1))
                        return false;
                    k += 2;
                } else {
                    return false;
                }
                continue;
            }
            const std::string &o = p.op;
            double a = 0, b = 0;
            auto two = [&] { return pop(b) && pop(a); };
            if (o == "add") { if (!two()) return false; st.push_back(a + b); }
            else if (o == "sub") { if (!two()) return false; st.push_back(a - b); }
            else if (o == "mul") { if (!two()) return false; st.push_back(a * b); }
            else if (o == "div") { if (!two()) return false; st.push_back(b != 0 ? a / b : 0); }
            else if (o == "idiv") { if (!two()) return false; st.push_back((long) b != 0 ? (double) ((long) a / (long) b) : 0); }
            else if (o == "mod") { if (!two()) return false; st.push_back((long) b != 0 ? (double) ((long) a % (long) b) : 0); }
            else if (o == "exp") { if (!two()) return false; st.push_back(std::pow(a, b)); }
            else if (o == "atan") { if (!two()) return false; double d = std::atan2(a, b) * 180.0 / 3.14159265358979; st.push_back(d < 0 ? d + 360 : d); }
            else if (o == "eq") { if (!two()) return false; st.push_back(a == b); }
            else if (o == "ne") { if (!two()) return false; st.push_back(a != b); }
            else if (o == "gt") { if (!two()) return false; st.push_back(a > b); }
            else if (o == "ge") { if (!two()) return false; st.push_back(a >= b); }
            else if (o == "lt") { if (!two()) return false; st.push_back(a < b); }
            else if (o == "le") { if (!two()) return false; st.push_back(a <= b); }
            else if (o == "and") { if (!two()) return false; st.push_back((double) ((long) a & (long) b)); }
            else if (o == "or") { if (!two()) return false; st.push_back((double) ((long) a | (long) b)); }
            else if (o == "xor") { if (!two()) return false; st.push_back((double) ((long) a ^ (long) b)); }
            else if (o == "bitshift") { if (!two()) return false; long v = (long) a; int sh = (int) b; st.push_back((double) (sh >= 0 ? v << sh : v >> -sh)); }
            else if (o == "exch") { if (!two()) return false; st.push_back(b); st.push_back(a); }
            else if (o == "neg") { if (!pop(a)) return false; st.push_back(-a); }
            else if (o == "abs") { if (!pop(a)) return false; st.push_back(std::fabs(a)); }
            else if (o == "ceiling") { if (!pop(a)) return false; st.push_back(std::ceil(a)); }
            else if (o == "floor") { if (!pop(a)) return false; st.push_back(std::floor(a)); }
            else if (o == "round") { if (!pop(a)) return false; st.push_back(std::floor(a + 0.5)); }
            else if (o == "truncate" || o == "cvi") { if (!pop(a)) return false; st.push_back(std::trunc(a)); }
            else if (o == "cvr") { if (!pop(a)) return false; st.push_back(a); }
            else if (o == "sqrt") { if (!pop(a)) return false; st.push_back(std::sqrt(std::max(0.0, a))); }
            else if (o == "sin") { if (!pop(a)) return false; st.push_back(std::sin(a * 3.14159265358979 / 180.0)); }
            else if (o == "cos") { if (!pop(a)) return false; st.push_back(std::cos(a * 3.14159265358979 / 180.0)); }
            else if (o == "ln") { if (!pop(a)) return false; st.push_back(a > 0 ? std::log(a) : 0); }
            else if (o == "log") { if (!pop(a)) return false; st.push_back(a > 0 ? std::log10(a) : 0); }
            else if (o == "not") { if (!pop(a)) return false; st.push_back(a == 0 ? 1 : 0); }
            else if (o == "true") st.push_back(1);
            else if (o == "false") st.push_back(0);
            else if (o == "pop") { if (!pop(a)) return false; }
            else if (o == "dup") { if (st.empty()) return false; st.push_back(st.back()); }
            else if (o == "copy") {
                if (!pop(a)) return false;
                const size_t n = (size_t) std::max(0.0, a);
                if (n > st.size()) return false;
                for (size_t i = st.size() - n, e = st.size(); i < e; ++i) st.push_back(st[i]);
            } else if (o == "index") {
                if (!pop(a)) return false;
                const size_t n = (size_t) std::max(0.0, a);
                if (n >= st.size()) return false;
                st.push_back(st[st.size() - 1 - n]);
            } else if (o == "roll") {
                if (!two()) return false;
                const long n = (long) a, j = (long) b;
                if (n <= 0 || (size_t) n > st.size()) continue;
                const long shift = ((j % n) + n) % n;
                std::rotate(st.end() - n, st.end() - shift, st.end());
            } else {
                return false;            // an operator a type 4 function can't have
            }
        }
        return true;
    }

    bool run_ps(const std::string &code, std::vector<double> &stack) const {
        auto it = m_ps.find(code);
        if (it == m_ps.end()) {
            std::vector<PsOp> top;
            size_t i = 0;
            if (!parse_ps(code, i, top, 0))
                top.clear();
            /* The program is one block: { ... } */
            std::vector<PsOp> prog = top.size() == 1 && top[0].kind == PsOp::Block ? top[0].block : top;
            it = m_ps.emplace(code, std::move(prog)).first;
        }
        return exec_ps(it->second, stack, 0);
    }

    const pdf::Document &m_doc;
    mutable std::map<std::string, std::vector<PsOp>> m_ps;
};

/* ---- Color spaces ------------------------------------------------------------ */

struct ColorSpace {
    enum Kind { Gray, RGB, CMYK, Lab, Indexed, Tint, Pattern } kind = Gray;
    int n = 1;                           // components an operator gives
    std::shared_ptr<ColorSpace> base;    // Indexed, Tint (the alternate), Pattern
    std::string lookup;                  // Indexed
    int hival = 0;
    Object tint_fn;                      // Tint
    float white[3] = {0.9505f, 1.f, 1.089f};   // Lab
};

Color lab_to_rgb(float L, float a, float b, const float *wp) {
    auto f = [](float t) { return t > 6.f / 29.f ? t * t * t : 3.f * (6.f / 29.f) * (6.f / 29.f) * (t - 4.f / 29.f); };
    const float fy = (L + 16.f) / 116.f;
    const float X = wp[0] * f(fy + a / 500.f), Y = wp[1] * f(fy), Z = wp[2] * f(fy - b / 200.f);
    float r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float bl = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
    auto gamma = [](float v) { v = clamp01(v); return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f; };
    return {gamma(r), gamma(g), gamma(bl), 1.f};
}

/* ---- The interpreter ------------------------------------------------------- */

/* An axial or radial shading, its colors sampled into stops */
struct Shading {
    bool radial = false;
    float x0 = 0, y0 = 0, x1 = 1, y1 = 0, r0 = 0, r1 = 1;
    std::vector<GradientStop> stops;
    Transform space;                     // shading space -> the graphic's box
    bool has_bbox = false;
    float bbox[4] = {0, 0, 0, 0};
};

/* The gradient paint for a shading: linear from (x0, y0) to (x1, y1), or
 * radial with the outer circle as the unit circle and the inner circle's
 * center as the focal point (an inner radius moves the stops outward) */
VectorPaint shading_paint(const Shading &sh, float alpha) {
    VectorPaint p;
    p.kind = sh.radial ? PaintKind::Radial : PaintKind::Linear;
    p.stops = sh.stops;
    p.spread = Spread::Pad;
    if (!sh.radial) {
        float dx = sh.x1 - sh.x0, dy = sh.y1 - sh.y0;
        if (dx == 0.f && dy == 0.f)
            dx = 1e-3f;
        p.xf = sh.space * Transform{dx, dy, -dy, dx, sh.x0, sh.y0};
    } else {
        const float r1 = std::max(sh.r1, 1e-3f);
        p.xf = sh.space * Transform{r1, 0, 0, r1, sh.x1, sh.y1};
        p.focal = {(sh.x0 - sh.x1) / r1, (sh.y0 - sh.y1) / r1};
        if (sh.r0 > 0.f)
            for (GradientStop &st : p.stops)
                st.offset = (sh.r0 + st.offset * (r1 - sh.r0)) / r1;
    }
    for (GradientStop &st : p.stops)
        st.color.a *= alpha;
    p.color = p.stops.empty() ? Color{0, 0, 0, 1} : p.stops[p.stops.size() / 2].color;
    return p;
}

struct GState {
    Transform ctm;
    std::shared_ptr<ColorSpace> fill_cs, stroke_cs;
    Color fill{0, 0, 0, 1}, stroke{0, 0, 0, 1};
    bool fill_pattern = false, stroke_pattern = false;
    std::shared_ptr<Shading> fill_shading, stroke_shading;   // a shading pattern
    float fill_alpha = 1.f, stroke_alpha = 1.f;
    float line_width = 1.f;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    float miter = 10.f;
    std::vector<float> dash;
    float dash_phase = 0.f;
    int clip = -1;                       // VectorScene::clips
    /* Text state */
    PdfFont *font = nullptr;
    float font_size = 0.f, char_space = 0.f, word_space = 0.f, hscale = 1.f, leading = 0.f, rise = 0.f;
    int render = 0;
};

class Interpreter {
public:
    Interpreter(const pdf::Document &doc, VectorScene &scene) : m_doc(doc), m_scene(scene), m_fn(doc) {
        m_device_gray = std::make_shared<ColorSpace>();
        m_device_rgb = std::make_shared<ColorSpace>();
        m_device_rgb->kind = ColorSpace::RGB;
        m_device_rgb->n = 3;
        m_device_cmyk = std::make_shared<ColorSpace>();
        m_device_cmyk->kind = ColorSpace::CMYK;
        m_device_cmyk->n = 4;
        FT_Init_FreeType(&m_ft);
    }
    ~Interpreter() {
        for (auto &kv : m_fonts)
            if (kv.second->face)
                FT_Done_Face(kv.second->face);
        if (m_ft)
            FT_Done_FreeType(m_ft);
    }

    void run_page(const Object &page, const Transform &base) {
        GState gs;
        gs.ctm = base;
        m_base = base;
        gs.fill_cs = gs.stroke_cs = m_device_gray;
        const Object &res = m_doc.get(page, "Resources");
        const Object &contents = m_doc.get(page, "Contents");
        std::string data;
        if (contents.is(Type::Stream)) {
            m_doc.stream_bytes(contents, data);
        } else if (contents.is(Type::Array)) {
            for (size_t i = 0; i < contents.size(); ++i) {
                std::string part;
                if (m_doc.stream_bytes(m_doc.resolve(contents[i]), part)) {
                    data += part;
                    data += '\n';        // a token can't run across streams
                }
            }
        }
        run(data, res, gs, 0);
    }

private:
    void remark(const char *what) {
        for (const std::string &s : m_scene.remarks)
            if (s == what)
                return;
        m_scene.remarks.emplace_back(what);
    }

    /* ---- Paths ---- */
    void move_to(const GState &gs, float x, float y) {
        const Point p = gs.ctm.apply({x, y});
        m_path.push_back({PathCmd::Move, 0, 0, 0, 0, p.x, p.y});
        m_cur = {x, y};
        m_start = m_cur;
    }
    void line_to(const GState &gs, float x, float y) {
        if (m_path.empty())
            move_to(gs, x, y);
        const Point p = gs.ctm.apply({x, y});
        m_path.push_back({PathCmd::Line, 0, 0, 0, 0, p.x, p.y});
        m_cur = {x, y};
    }
    void curve_to(const GState &gs, float x1, float y1, float x2, float y2, float x3, float y3) {
        if (m_path.empty())
            move_to(gs, x1, y1);
        const Point a = gs.ctm.apply({x1, y1}), b = gs.ctm.apply({x2, y2}), c = gs.ctm.apply({x3, y3});
        m_path.push_back({PathCmd::Cubic, a.x, a.y, b.x, b.y, c.x, c.y});
        m_cur = {x3, y3};
    }
    void close_path() {
        if (!m_path.empty())
            m_path.push_back({PathCmd::Close, 0, 0, 0, 0, 0, 0});
        m_cur = m_start;
    }

    static float scale_of(const Transform &t) {
        return std::sqrt(std::fabs(t.a * t.d - t.b * t.c));
    }

    void paint(const GState &gs, bool fill, bool stroke, FillRule rule) {
        if (m_path.empty())
            return;
        VectorShape sh;
        sh.path = m_path;
        sh.rule = rule;
        sh.clip = gs.clip;
        if (fill && !gs.fill_pattern) {
            sh.fill.kind = PaintKind::Solid;
            sh.fill.color = gs.fill;
            sh.fill.color.a *= gs.fill_alpha;
        } else if (fill && gs.fill_shading) {
            sh.fill = shading_paint(*gs.fill_shading, gs.fill_alpha);
        } else if (fill) {
            note(m_scene, "tiling patterns");
        }
        if (stroke && (!gs.stroke_pattern || gs.stroke_shading)) {
            const float k = scale_of(gs.ctm);
            if (gs.stroke_shading) {
                sh.stroke.paint = shading_paint(*gs.stroke_shading, gs.stroke_alpha);
            } else {
                sh.stroke.paint.kind = PaintKind::Solid;
                sh.stroke.paint.color = gs.stroke;
                sh.stroke.paint.color.a *= gs.stroke_alpha;
            }
            /* A zero width is the thinnest line the device draws */
            sh.stroke.width = gs.line_width > 0.f ? gs.line_width * k : 0.25f;
            sh.stroke.cap = gs.cap;
            sh.stroke.join = gs.join;
            sh.stroke.miter_limit = gs.miter;
            for (float d : gs.dash)
                sh.stroke.dash.push_back(d * k);
            sh.stroke.dash_offset = gs.dash_phase * k;
        } else if (stroke) {
            note(m_scene, "tiling patterns");
        }
        if (sh.fill.none() && sh.stroke.none())
            return;
        if (!sh.fill.none())
            sh.windings = std::make_shared<const std::vector<uint8_t>>(fill_windings(sh.path));
        m_scene.order.push_back({VectorSlot::Shape, (int) m_scene.shapes.size()});
        m_scene.shapes.push_back(std::move(sh));
    }

    /* A new clipping layer on top of the state's clip */
    void clip(GState &gs, const Path &path, FillRule rule) {
        if (path.empty())
            return;
        ClipLayers layers = gs.clip >= 0 ? m_scene.clips[(size_t) gs.clip] : ClipLayers();
        layers.push_back({ClipPath{path, rule}});
        m_scene.clips.push_back(std::move(layers));
        gs.clip = (int) m_scene.clips.size() - 1;
    }

    /* ---- Colors ---- */
    std::shared_ptr<ColorSpace> color_space(const Object &spec_in, const Object &res, int depth = 0) {
        const Object &spec = m_doc.resolve(spec_in);
        if (depth > 8)
            return m_device_gray;
        if (spec.is(Type::Name)) {
            const std::string &n = spec.str;
            if (n == "DeviceGray" || n == "G" || n == "CalGray")
                return m_device_gray;
            if (n == "DeviceRGB" || n == "RGB" || n == "CalRGB")
                return m_device_rgb;
            if (n == "DeviceCMYK" || n == "CMYK")
                return m_device_cmyk;
            if (n == "Pattern") {
                auto cs = std::make_shared<ColorSpace>();
                cs->kind = ColorSpace::Pattern;
                cs->n = 0;
                return cs;
            }
            /* A named resource */
            const Object &named = m_doc.get(m_doc.get(res, "ColorSpace"), n.c_str());
            if (!named.is(Type::Null) && &named != &spec)
                return color_space(named, res, depth + 1);
            return m_device_gray;
        }
        if (!spec.is(Type::Array) || spec.size() == 0)
            return m_device_gray;
        const std::string fam = m_doc.resolve(spec[0]).str;
        if (fam == "ICCBased") {
            const Object &stream = m_doc.resolve(spec[1]);
            const int n = m_doc.get(stream, "N").as_int(3);
            return n == 1 ? m_device_gray : n == 4 ? m_device_cmyk : m_device_rgb;
        }
        if (fam == "CalRGB")
            return m_device_rgb;
        if (fam == "CalGray")
            return m_device_gray;
        if (fam == "Lab") {
            auto cs = std::make_shared<ColorSpace>();
            cs->kind = ColorSpace::Lab;
            cs->n = 3;
            const Object &wp = m_doc.get(m_doc.resolve(spec[1]), "WhitePoint");
            for (int i = 0; i < 3 && i < (int) wp.size(); ++i)
                cs->white[i] = (float) wp[(size_t) i].as_number(cs->white[i]);
            return cs;
        }
        if (fam == "Indexed" || fam == "I") {
            auto cs = std::make_shared<ColorSpace>();
            cs->kind = ColorSpace::Indexed;
            cs->n = 1;
            cs->base = color_space(spec[1], res, depth + 1);
            cs->hival = m_doc.resolve(spec[2]).as_int(0);
            const Object &lut = m_doc.resolve(spec[3]);
            if (lut.is(Type::String))
                cs->lookup = lut.str;
            else if (lut.is(Type::Stream))
                m_doc.stream_bytes(lut, cs->lookup);
            return cs;
        }
        if (fam == "Separation" || fam == "DeviceN") {
            auto cs = std::make_shared<ColorSpace>();
            cs->kind = ColorSpace::Tint;
            const Object &names = m_doc.resolve(spec[1]);
            cs->n = fam == "Separation" ? 1 : std::max<int>(1, (int) names.size());
            cs->base = color_space(spec[2], res, depth + 1);
            cs->tint_fn = spec[3];
            return cs;
        }
        if (fam == "Pattern") {
            auto cs = std::make_shared<ColorSpace>();
            cs->kind = ColorSpace::Pattern;
            cs->n = 0;
            return cs;
        }
        return color_space(spec[0], res, depth + 1);
    }

    Color to_rgb(const ColorSpace &cs, const std::vector<float> &c, int depth = 0) {
        auto at = [&](size_t i, float d) { return i < c.size() ? c[i] : d; };
        switch (cs.kind) {
        case ColorSpace::Gray: return {clamp01(at(0, 0)), clamp01(at(0, 0)), clamp01(at(0, 0)), 1};
        case ColorSpace::RGB: return {clamp01(at(0, 0)), clamp01(at(1, 0)), clamp01(at(2, 0)), 1};
        case ColorSpace::CMYK: {
            Cmyk k{clamp01(at(0, 0)) * 100.f, clamp01(at(1, 0)) * 100.f, clamp01(at(2, 0)) * 100.f,
                   clamp01(at(3, 1)) * 100.f};
            return cmyk_to_rgb(k);
        }
        case ColorSpace::Lab: return lab_to_rgb(at(0, 0), at(1, 0), at(2, 0), cs.white);
        case ColorSpace::Indexed: {
            if (!cs.base || depth > 4)
                return {0, 0, 0, 1};
            const int idx = std::clamp((int) std::lround(at(0, 0)), 0, std::max(0, cs.hival));
            const int n = std::max(1, cs.base->n);
            std::vector<float> bc;
            for (int i = 0; i < n; ++i) {
                const size_t pos = (size_t) idx * (size_t) n + (size_t) i;
                bc.push_back(pos < cs.lookup.size() ? (unsigned char) cs.lookup[pos] / 255.f : 0.f);
            }
            return to_rgb(*cs.base, bc, depth + 1);
        }
        case ColorSpace::Tint: {
            std::vector<float> alt;
            if (cs.base && depth < 4 && m_fn.eval(cs.tint_fn, c, alt))
                return to_rgb(*cs.base, alt, depth + 1);
            note(m_scene, "color functions");
            const float t = clamp01(at(0, 1));
            return {1.f - t, 1.f - t, 1.f - t, 1.f};   // a spot color, as gray
        }
        case ColorSpace::Pattern: break;
        }
        return {0, 0, 0, 1};
    }

    /* The color a space starts with (cs, CS) */
    Color initial(const ColorSpace &cs) {
        switch (cs.kind) {
        case ColorSpace::CMYK: return to_rgb(cs, {0, 0, 0, 1});
        case ColorSpace::Tint: return to_rgb(cs, std::vector<float>((size_t) cs.n, 1.f));
        case ColorSpace::Indexed: return to_rgb(cs, {0});
        default: return {0, 0, 0, 1};
        }
    }

    /* An axial (2) or radial (3) shading; null for the others, which are named */
    std::shared_ptr<Shading> shading(const Object &ref, const Object &res, const Transform &space) {
        const Object &obj = m_doc.resolve(ref);
        if (!obj.dict)
            return nullptr;
        const int type = m_doc.get(obj, "ShadingType").as_int(0);
        if (type != 2 && type != 3) {
            note(m_scene, "mesh and function shadings");
            return nullptr;
        }
        auto out = std::make_shared<Shading>();
        out->radial = type == 3;
        out->space = space;
        const Object &c = m_doc.get(obj, "Coords");
        if (c.size() < (size_t) (out->radial ? 6 : 4))
            return nullptr;
        auto n = [&](size_t i) { return (float) m_doc.resolve(c[i]).as_number(); };
        if (out->radial) {
            out->x0 = n(0); out->y0 = n(1); out->r0 = n(2);
            out->x1 = n(3); out->y1 = n(4); out->r1 = n(5);
        } else {
            out->x0 = n(0); out->y0 = n(1); out->x1 = n(2); out->y1 = n(3);
        }
        const Object &domain = m_doc.get(obj, "Domain");
        const float t0 = domain.size() >= 2 ? (float) domain[0].as_number(0) : 0.f;
        const float t1 = domain.size() >= 2 ? (float) domain[1].as_number(1) : 1.f;
        std::shared_ptr<ColorSpace> cs = color_space(m_doc.get(obj, "ColorSpace"), res);
        const Object *fn = obj.get("Function");
        if (!fn)
            return nullptr;
        const int samples = 24;          // enough for any gradient a function draws smoothly
        for (int i = 0; i <= samples; ++i) {
            const float u = (float) i / (float) samples;
            std::vector<float> comps;
            if (!m_fn.eval(*fn, {t0 + (t1 - t0) * u}, comps))
                continue;
            out->stops.push_back({u, to_rgb(*cs, comps)});
        }
        if (out->stops.size() < 2) {
            note(m_scene, "color functions");
            return nullptr;
        }
        const Object &bbox = m_doc.get(obj, "BBox");
        if (bbox.size() == 4) {
            out->has_bbox = true;
            for (int i = 0; i < 4; ++i)
                out->bbox[i] = (float) m_doc.resolve(bbox[(size_t) i]).as_number();
        }
        return out;
    }

    /* sh: the shading fills the clip (its bounding box, when it has one;
     * else the whole page) */
    void paint_shading(const GState &gs, const std::string &name, const Object &res) {
        std::shared_ptr<Shading> sh = shading(m_doc.get(m_doc.get(res, "Shading"), name.c_str()), res, gs.ctm);
        if (!sh)
            return;
        Path area;
        if (sh->has_bbox) {
            const float *b = sh->bbox;
            const Point p[4] = {gs.ctm.apply({b[0], b[1]}), gs.ctm.apply({b[2], b[1]}),
                                gs.ctm.apply({b[2], b[3]}), gs.ctm.apply({b[0], b[3]})};
            area = {{PathCmd::Move, 0, 0, 0, 0, p[0].x, p[0].y}, {PathCmd::Line, 0, 0, 0, 0, p[1].x, p[1].y},
                    {PathCmd::Line, 0, 0, 0, 0, p[2].x, p[2].y}, {PathCmd::Line, 0, 0, 0, 0, p[3].x, p[3].y},
                    {PathCmd::Close, 0, 0, 0, 0, 0, 0}};
        } else {
            const float w = m_scene.width, h = m_scene.height;
            area = {{PathCmd::Move, 0, 0, 0, 0, 0, 0}, {PathCmd::Line, 0, 0, 0, 0, w, 0},
                    {PathCmd::Line, 0, 0, 0, 0, w, h}, {PathCmd::Line, 0, 0, 0, 0, 0, h},
                    {PathCmd::Close, 0, 0, 0, 0, 0, 0}};
        }
        VectorShape shape;
        shape.path = std::move(area);
        shape.clip = gs.clip;
        shape.fill = shading_paint(*sh, gs.fill_alpha);
        shape.windings = std::make_shared<const std::vector<uint8_t>>(fill_windings(shape.path));
        m_scene.order.push_back({VectorSlot::Shape, (int) m_scene.shapes.size()});
        m_scene.shapes.push_back(std::move(shape));
    }

    /* A pattern color (scn /P0): a shading pattern paints with its
     * gradient, in the page's default space under the pattern's matrix */
    std::shared_ptr<Shading> pattern_shading(const std::string &name, const Object &res) {
        const Object &pat = m_doc.get(m_doc.get(res, "Pattern"), name.c_str());
        if (!pat.dict)
            return nullptr;
        if (m_doc.get(pat, "PatternType").as_int(0) != 2) {
            note(m_scene, "tiling patterns");
            return nullptr;
        }
        Transform matrix;
        const Object &m = m_doc.get(pat, "Matrix");
        if (m.size() == 6)
            matrix = {(float) m[0].as_number(), (float) m[1].as_number(), (float) m[2].as_number(),
                      (float) m[3].as_number(), (float) m[4].as_number(), (float) m[5].as_number()};
        const Object *sh = pat.get("Shading");
        return sh ? shading(*sh, res, m_base * matrix) : nullptr;
    }

    /* ---- Fonts ---- */
    PdfFont *font(const Object &ref_or_dict) {
        const Object &fd = m_doc.resolve(ref_or_dict);
        if (!fd.dict)
            return nullptr;
        const void *key = fd.dict.get();
        auto it = m_fonts.find(key);
        if (it != m_fonts.end())
            return it->second.get();
        auto f = std::make_unique<PdfFont>();
        load_font(fd, *f);
        PdfFont *raw = f.get();
        m_fonts[key] = std::move(f);
        return raw;
    }

    void load_font(const Object &fd, PdfFont &f) {
        const std::string subtype = m_doc.get(fd, "Subtype").str;
        if (subtype == "Type3") {
            f.type3 = true;
            note(m_scene, "Type 3 fonts");
            return;
        }
        Object desc_holder = fd;
        if (subtype == "Type0") {
            f.composite = true;
            f.code_bytes = 2;
            const Object &enc = m_doc.get(fd, "Encoding");
            if (enc.is(Type::Stream)) {
                std::string s;
                if (m_doc.stream_bytes(enc, s)) {
                    f.code_bytes = 1;
                    parse_cmap(s, f, false);
                    if (f.code_bytes < 1)
                        f.code_bytes = 2;
                }
            } else if (enc.is(Type::Name) && enc.str != "Identity-H" && enc.str != "Identity-V") {
                note(m_scene, "predefined CMaps");
            }
            const Object &kids = m_doc.get(fd, "DescendantFonts");
            desc_holder = m_doc.resolve(kids[0]);
            f.default_width = (float) m_doc.get(desc_holder, "DW").as_number(1000);
            const Object &w = m_doc.get(desc_holder, "W");
            for (size_t i = 0; i < w.size();) {
                const Object &a = m_doc.resolve(w[i]);
                const Object &b = m_doc.resolve(w[i + 1]);
                if (a.is_number() && b.is(Type::Array)) {
                    for (size_t k = 0; k < b.size(); ++k)
                        f.widths[(uint32_t) a.number + (uint32_t) k] = (float) m_doc.resolve(b[k]).as_number();
                    i += 2;
                } else if (a.is_number() && b.is_number() && i + 2 < w.size()) {
                    const float width = (float) m_doc.resolve(w[i + 2]).as_number();
                    for (uint32_t c = (uint32_t) a.number; c <= (uint32_t) b.number && c - (uint32_t) a.number < 65536; ++c)
                        f.widths[c] = width;
                    i += 3;
                } else {
                    break;
                }
            }
            const Object &map = m_doc.get(desc_holder, "CIDToGIDMap");
            if (map.is(Type::Stream)) {
                std::string s;
                if (m_doc.stream_bytes(map, s))
                    for (size_t i = 0; i + 1 < s.size(); i += 2)
                        f.cid_to_gid.push_back((uint16_t) (((unsigned char) s[i] << 8) | (unsigned char) s[i + 1]));
            }
        } else {
            const int first = m_doc.get(fd, "FirstChar").as_int(0);
            const Object &w = m_doc.get(fd, "Widths");
            for (size_t i = 0; i < w.size(); ++i)
                f.widths[(uint32_t) (first + (int) i)] = (float) m_doc.resolve(w[i]).as_number();
        }
        const Object &desc = m_doc.get(desc_holder, "FontDescriptor");
        f.missing_width = (float) m_doc.get(desc, "MissingWidth").as_number(0);
        const int flags = m_doc.get(desc, "Flags").as_int(0);
        const bool symbolic = (flags & 4) != 0;
        if (m_doc.get(desc, "ItalicAngle").as_number(0) != 0.0 || (flags & 64))
            f.italic = true;
        if (m_doc.get(desc, "FontWeight").as_number(400) >= 600 || (flags & (1 << 18)))
            f.weight = 700;

        /* The program, when embedded */
        const Object *file = nullptr;
        for (const char *k : {"FontFile2", "FontFile3", "FontFile"}) {
            const Object &o = m_doc.get(desc, k);
            if (o.is(Type::Stream)) {
                file = &o;
                break;
            }
        }
        if (file && m_ft) {
            auto prog = std::make_shared<std::string>();
            if (m_doc.stream_bytes(*file, *prog) && !prog->empty() &&
                FT_New_Memory_Face(m_ft, (const FT_Byte *) prog->data(), (FT_Long) prog->size(), 0, &f.face) == 0) {
                f.program = prog;
                f.units = f.face->units_per_EM > 0 ? (float) f.face->units_per_EM : 1000.f;
            } else {
                f.face = nullptr;
            }
        }
        const Object &to_unicode = m_doc.get(fd, "ToUnicode");
        if (to_unicode.is(Type::Stream)) {
            std::string s;
            if (m_doc.stream_bytes(to_unicode, s))
                parse_cmap(s, f, true);
        }

        if (!f.composite) {
            /* Code -> glyph name by the encoding, then to a glyph */
            std::vector<std::string> names(256);
            const Object &enc = m_doc.get(fd, "Encoding");
            std::string base_enc = enc.is(Type::Name) ? enc.str : m_doc.get(enc, "BaseEncoding").str;
            const Object &diffs = m_doc.get(enc, "Differences");
            int code = 0;
            for (size_t i = 0; i < diffs.size(); ++i) {
                const Object &d = m_doc.resolve(diffs[i]);
                if (d.is_number())
                    code = d.as_int();
                else if (d.is(Type::Name) && code >= 0 && code < 256)
                    names[(size_t) code++] = d.str;
            }
            f.code_gid.assign(256, 0);
            for (int c = 0; c < 256; ++c) {
                uint32_t uni = 0;
                if (!names[(size_t) c].empty())
                    uni = glyph_unicode(names[(size_t) c]);
                else if (base_enc == "MacRomanEncoding")
                    uni = mac_roman(c);
                else if (base_enc == "WinAnsiEncoding" || (base_enc.empty() && !symbolic && !f.face))
                    uni = win_ansi(c);
                else if (base_enc == "StandardEncoding" || base_enc.empty())
                    uni = standard_enc(c);
                if (!f.text.count((uint32_t) c) && uni)
                    append_utf8(f.text[(uint32_t) c], uni);
                if (!f.face)
                    continue;
                int gid = 0;
                const bool truetype = f.face->num_charmaps > 0 && FT_IS_SFNT(f.face);
                if (!names[(size_t) c].empty() && FT_HAS_GLYPH_NAMES(f.face))
                    gid = (int) FT_Get_Name_Index(f.face, (FT_String *) names[(size_t) c].c_str());
                if (!gid && truetype) {
                    /* (3,1) by Unicode, (3,0) symbol at F0xx, (1,0) Mac by code */
                    for (int i = 0; i < f.face->num_charmaps && !gid; ++i) {
                        FT_CharMap cm = f.face->charmaps[i];
                        FT_Set_Charmap(f.face, cm);
                        if (cm->platform_id == 3 && cm->encoding_id == 1 && uni && !symbolic)
                            gid = (int) FT_Get_Char_Index(f.face, uni);
                        else if (cm->platform_id == 3 && cm->encoding_id == 0)
                            gid = (int) FT_Get_Char_Index(f.face, 0xF000 + (FT_ULong) c);
                        else if (cm->platform_id == 1 && cm->encoding_id == 0)
                            gid = (int) FT_Get_Char_Index(f.face, (FT_ULong) c);
                    }
                    if (!gid && uni)
                        for (int i = 0; i < f.face->num_charmaps && !gid; ++i) {
                            FT_Set_Charmap(f.face, f.face->charmaps[i]);
                            gid = (int) FT_Get_Char_Index(f.face, uni);
                        }
                }
                if (!gid && !truetype) {
                    /* A Type 1 or CFF font's own encoding */
                    if (FT_Select_Charmap(f.face, FT_ENCODING_ADOBE_CUSTOM) == 0 ||
                        FT_Select_Charmap(f.face, FT_ENCODING_ADOBE_STANDARD) == 0 ||
                        FT_Select_Charmap(f.face, FT_ENCODING_ADOBE_EXPERT) == 0)
                        gid = (int) FT_Get_Char_Index(f.face, (FT_ULong) c);
                    if (!gid && uni && FT_Select_Charmap(f.face, FT_ENCODING_UNICODE) == 0)
                        gid = (int) FT_Get_Char_Index(f.face, uni);
                }
                if (!gid && truetype && f.face->num_charmaps == 0)
                    gid = c;
                f.code_gid[(size_t) c] = gid;
            }
        }
        if (!f.face) {
            remark("fonts not embedded, drawn in installed faces");
            fallback_families(m_doc.get(desc_holder, "BaseFont").str.empty() ? m_doc.get(fd, "BaseFont").str
                                                                             : m_doc.get(desc_holder, "BaseFont").str,
                              f);
        }
        f.ok = true;
    }

    /* ---- Text ---- */
    void show(GState &gs, const std::string &bytes) {
        PdfFont *f = gs.font;
        if (!f || !f->ok || f->type3 || gs.font_size == 0.f) {
            advance_only(gs, bytes);
            return;
        }
        const bool visible = gs.render != 3 && gs.render != 7;
        const bool fill = gs.render == 0 || gs.render == 2 || gs.render == 4 || gs.render == 6;
        const bool stroke = gs.render == 1 || gs.render == 2 || gs.render == 5 || gs.render == 6;
        Path ink;                        // one path for the whole string (embedded fonts)
        VectorText run_text;             // the glyphs to outline later (fonts not embedded)
        const Transform text_to_page = gs.ctm * m_tm;
        run_text.xf = text_to_page * Transform{gs.hscale, 0, 0, -1, 0, gs.rise};   // y down, as SVG text
        run_text.clip = gs.clip;
        float pen = 0.f;                 // text space, along the line
        const int nb = f->composite ? f->code_bytes : 1;
        for (size_t i = 0; i + (size_t) nb <= bytes.size(); i += (size_t) nb) {
            uint32_t code = 0;
            for (int k = 0; k < nb; ++k)
                code = (code << 8) | (unsigned char) bytes[i + (size_t) k];
            const float w0 = f->width(code) / 1000.f;
            if (visible) {
                if (f->face) {
                    const int gid = f->glyph(code);
                    auto it = f->outlines.find(gid);
                    if (it == f->outlines.end())
                        it = f->outlines.emplace(gid, Path()).first, load_glyph(*f, gid, it->second);
                    /* Glyph space -> text space -> page */
                    const float em = gs.font_size / f->units;
                    const Transform g = text_to_page * Transform{gs.hscale * em, 0, 0, em, pen, gs.rise};
                    for (PathCmd c : it->second) {
                        if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic) {
                            const Point p = g.apply({c.cx1, c.cy1});
                            c.cx1 = p.x;
                            c.cy1 = p.y;
                        }
                        if (c.kind == PathCmd::Cubic) {
                            const Point p = g.apply({c.cx2, c.cy2});
                            c.cx2 = p.x;
                            c.cy2 = p.y;
                        }
                        if (c.kind != PathCmd::Close) {
                            const Point p = g.apply({c.x, c.y});
                            c.x = p.x;
                            c.y = p.y;
                        }
                        ink.push_back(c);
                    }
                } else {
                    auto t = f->text.find(code);
                    if (t != f->text.end() && !t->second.empty() && t->second != " ") {
                        VectorTextRun run;
                        run.text = t->second;
                        run.families = f->families;
                        run.size = gs.font_size;
                        run.weight = f->weight;
                        run.italic = f->italic;
                        run.fill = gs.fill;
                        run.fill.a *= gs.fill_alpha;
                        run.has_x = run.has_y = true;
                        run.x = pen / gs.hscale;  // the run's space has the horizontal scale in it
                        run.y = 0.f;
                        run_text.runs.push_back(std::move(run));
                    }
                }
            }
            float tx = w0 * gs.font_size + gs.char_space;
            if (nb == 1 && code == 32)
                tx += gs.word_space;
            pen += tx * gs.hscale;
        }
        if (!ink.empty()) {
            m_path = std::move(ink);
            GState paint_gs = gs;
            paint(paint_gs, fill, stroke, FillRule::NonZero);
            m_path.clear();
        }
        if (!run_text.runs.empty()) {
            m_scene.order.push_back({VectorSlot::Text, (int) m_scene.texts.size()});
            m_scene.texts.push_back(std::move(run_text));
        }
        m_tm = m_tm * Transform::translate(pen, 0.f);
    }

    void advance_only(GState &gs, const std::string &bytes) {
        PdfFont *f = gs.font;
        const int nb = f && f->composite ? f->code_bytes : 1;
        float pen = 0.f;
        for (size_t i = 0; i + (size_t) nb <= bytes.size(); i += (size_t) nb) {
            uint32_t code = 0;
            for (int k = 0; k < nb; ++k)
                code = (code << 8) | (unsigned char) bytes[i + (size_t) k];
            float tx = (f ? f->width(code) / 1000.f : 0.5f) * gs.font_size + gs.char_space;
            if (nb == 1 && code == 32)
                tx += gs.word_space;
            pen += tx * gs.hscale;
        }
        m_tm = m_tm * Transform::translate(pen, 0.f);
    }

    /* ---- Images ---- */
    static void png_sink(void *context, void *data, int size) {
        static_cast<std::string *>(context)->append(static_cast<const char *>(data), (size_t) size);
    }

    /* The picture an image dictionary and its (filtered) data describe, as
     * bytes a decoder reads: its JPEG, or PNG from the samples */
    bool image_bytes(const Object &dict, const std::string &raw_or_stream, bool inline_image, const Object &res,
                     const GState &gs, std::shared_ptr<const std::vector<uint8_t>> &out, int &w, int &h) {
        auto key = [&](const char *full, const char *abbr) -> const Object & {
            const Object &v = m_doc.get(dict, full);
            return v.is(Type::Null) && inline_image ? m_doc.get(dict, abbr) : v;
        };
        w = key("Width", "W").as_int(0);
        h = key("Height", "H").as_int(0);
        if (w <= 0 || h <= 0 || (int64_t) w * h > 100000000)
            return false;
        std::string data;
        std::string stopped;
        if (inline_image) {
            /* Filters by hand: the inline dictionary isn't a stream */
            Object s;
            s.type = Type::Stream;
            s.dict = std::make_shared<pdf::Dict>(*dict.dict);
            if (s.dict->count("F") && !s.dict->count("Filter")) {
                Object f = (*s.dict)["F"];
                auto expand = [](Object &n) {
                    static const std::map<std::string, std::string> abbr = {
                        {"AHx", "ASCIIHexDecode"}, {"A85", "ASCII85Decode"}, {"LZW", "LZWDecode"},
                        {"Fl", "FlateDecode"}, {"RL", "RunLengthDecode"}, {"CCF", "CCITTFaxDecode"},
                        {"DCT", "DCTDecode"}};
                    auto it = abbr.find(n.str);
                    if (n.is(Type::Name) && it != abbr.end())
                        n.str = it->second;
                };
                if (f.is(Type::Name))
                    expand(f);
                else if (f.array)
                    for (Object &x : *f.array)
                        expand(x);
                (*s.dict)["Filter"] = f;
            }
            if (s.dict->count("DP") && !s.dict->count("DecodeParms"))
                (*s.dict)["DecodeParms"] = (*s.dict)["DP"];
            s.data = std::make_shared<const std::string>(raw_or_stream);
            if (!m_doc.stream_bytes(s, data, &stopped))
                return false;
        } else {
            if (!m_doc.stream_bytes(dict, data, &stopped))
                return false;
        }
        const bool mask = key("ImageMask", "IM").boolean;
        const Object &smask = m_doc.get(dict, "SMask");
        if (stopped == "DCTDecode" && !smask.is(Type::Stream)) {
            out = std::make_shared<const std::vector<uint8_t>>(data.begin(), data.end());
            return true;                 // a JPEG as it is
        }
        std::vector<uint8_t> rgba((size_t) w * (size_t) h * 4, 255);
        if (stopped == "DCTDecode") {
            /* A JPEG under a soft mask: decode it, then add the alpha */
            std::vector<uint8_t> px;
            int dw = 0, dh = 0;
            if (!make_display((const uint8_t *) data.data(), data.size(), 0, &px, &dw, &dh, nullptr) ||
                dw != w || dh != h)
                return false;
            rgba = std::move(px);
        } else if (!stopped.empty()) {
            note(m_scene, stopped == "JPXDecode" ? "JPEG 2000 images" : "fax and JBIG2 images");
            return false;
        } else {
            const int bpc = mask ? 1 : std::max(1, key("BitsPerComponent", "BPC").as_int(8));
            std::shared_ptr<ColorSpace> cs = mask ? m_device_gray : color_space(key("ColorSpace", "CS"), res);
            const int n = mask ? 1 : std::max(1, cs->n);
            const Object &decode = key("Decode", "D");
            const size_t row = ((size_t) w * (size_t) n * (size_t) bpc + 7) / 8;
            if (data.size() < row * (size_t) h)
                data.resize(row * (size_t) h, '\0');   // short data: the rest blank
            const double maxv = std::pow(2.0, bpc) - 1.0;
            std::vector<float> comps((size_t) n);
            std::map<uint64_t, Color> cache;          // indexed and spot colors repeat
            const bool cacheable = n * bpc <= 64;
            for (int y = 0; y < h; ++y) {
                const unsigned char *src = (const unsigned char *) data.data() + row * (size_t) y;
                for (int x = 0; x < w; ++x) {
                    uint64_t packed = 0;
                    for (int k = 0; k < n; ++k) {
                        const size_t bit = ((size_t) x * (size_t) n + (size_t) k) * (size_t) bpc;
                        uint32_t v = 0;
                        if (bpc == 8) {
                            v = src[bit / 8];
                        } else if (bpc == 16) {
                            v = (uint32_t) src[bit / 8] << 8 | src[bit / 8 + 1];
                        } else {
                            for (int b = 0; b < bpc; ++b) {
                                const size_t at = bit + (size_t) b;
                                v = (v << 1) | ((src[at / 8] >> (7 - at % 8)) & 1);
                            }
                        }
                        float t = (float) (v / maxv);
                        if (decode.size() >= (size_t) (2 * k + 2)) {
                            const float d0 = (float) decode[(size_t) (2 * k)].as_number();
                            const float d1 = (float) decode[(size_t) (2 * k + 1)].as_number();
                            t = d0 + t * (d1 - d0);
                        } else if (cs->kind == ColorSpace::Indexed && !mask) {
                            t = (float) v;     // an index, not a fraction
                        }
                        comps[(size_t) k] = t;
                        packed = (packed << bpc) | v;     // exact while it fits in 64 bits
                    }
                    uint8_t *px = &rgba[((size_t) y * (size_t) w + (size_t) x) * 4];
                    if (mask) {
                        /* A stencil: where the sample is 0 the fill color paints */
                        const bool on = comps[0] < 0.5f;
                        px[0] = (uint8_t) std::lround(gs.fill.r * 255.f);
                        px[1] = (uint8_t) std::lround(gs.fill.g * 255.f);
                        px[2] = (uint8_t) std::lround(gs.fill.b * 255.f);
                        px[3] = on ? 255 : 0;
                        continue;
                    }
                    Color c;
                    const bool simple = cs->kind == ColorSpace::Gray || cs->kind == ColorSpace::RGB ||
                                        cs->kind == ColorSpace::CMYK;
                    if (simple || !cacheable) {
                        c = to_rgb(*cs, comps);
                    } else {
                        auto hit = cache.find(packed);
                        if (hit == cache.end())
                            hit = cache.emplace(packed, to_rgb(*cs, comps)).first;
                        c = hit->second;
                    }
                    px[0] = (uint8_t) std::lround(c.r * 255.f);
                    px[1] = (uint8_t) std::lround(c.g * 255.f);
                    px[2] = (uint8_t) std::lround(c.b * 255.f);
                }
            }
        }
        if (smask.is(Type::Stream)) {
            /* A soft mask: its gray samples are the alpha, scaled to fit */
            std::string a;
            const int sw = m_doc.get(smask, "Width").as_int(0), shh = m_doc.get(smask, "Height").as_int(0);
            const int sb = m_doc.get(smask, "BitsPerComponent").as_int(8);
            std::string st;
            if (sw > 0 && shh > 0 && m_doc.stream_bytes(smask, a, &st) && st.empty() && sb == 8 &&
                a.size() >= (size_t) sw * (size_t) shh) {
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const int sx = (int) ((int64_t) x * sw / w), sy = (int) ((int64_t) y * shh / h);
                        rgba[((size_t) y * (size_t) w + (size_t) x) * 4 + 3] =
                            (uint8_t) a[(size_t) sy * (size_t) sw + (size_t) sx];
                    }
            }
        }
        std::string png;
        if (!stbi_write_png_to_func(png_sink, &png, w, h, 4, rgba.data(), w * 4))
            return false;
        out = std::make_shared<const std::vector<uint8_t>>(png.begin(), png.end());
        return true;
    }

    /* The unit square under the current transform, the picture's top row
     * at y 1 */
    void place_image(const GState &gs, std::shared_ptr<const std::vector<uint8_t>> bytes, int w, int h) {
        VectorImage im;
        im.xf = gs.ctm * Transform{1, 0, 0, -1, 0, 1};
        im.x = 0;
        im.y = 0;
        im.w = 1;
        im.h = 1;
        im.preserve = "none";
        im.opacity = gs.fill_alpha;
        im.clip = gs.clip;
        im.bytes = std::move(bytes);
        im.px_w = w;
        im.px_h = h;
        m_scene.order.push_back({VectorSlot::Image, (int) m_scene.images.size()});
        m_scene.images.push_back(std::move(im));
    }

    void do_xobject(GState &gs, const std::string &name, const Object &res, int depth) {
        const Object &xo = m_doc.get(m_doc.get(res, "XObject"), name.c_str());
        if (!xo.is(Type::Stream))
            return;
        const std::string sub = m_doc.get(xo, "Subtype").str;
        if (sub == "Image") {
            const void *key = xo.dict.get();
            auto hit = m_images.find(key);
            if (hit == m_images.end() || m_doc.get(xo, "ImageMask").boolean) {
                std::shared_ptr<const std::vector<uint8_t>> bytes;
                int w = 0, h = 0;
                if (!image_bytes(xo, std::string(), false, res, gs, bytes, w, h))
                    return;
                hit = m_images.emplace(key, CachedImage{bytes, w, h}).first;
            }
            place_image(gs, hit->second.bytes, hit->second.w, hit->second.h);
            return;
        }
        if (sub != "Form" || depth > 16)
            return;
        std::string data;
        if (!m_doc.stream_bytes(xo, data))
            return;
        GState inner = gs;
        const Object &m = m_doc.get(xo, "Matrix");
        if (m.size() == 6)
            inner.ctm = gs.ctm * Transform{(float) m[0].as_number(), (float) m[1].as_number(),
                                            (float) m[2].as_number(), (float) m[3].as_number(),
                                            (float) m[4].as_number(), (float) m[5].as_number()};
        const Object &bbox = m_doc.get(xo, "BBox");
        if (bbox.size() == 4) {
            Path box;
            const float x0 = (float) bbox[0].as_number(), y0 = (float) bbox[1].as_number();
            const float x1 = (float) bbox[2].as_number(), y1 = (float) bbox[3].as_number();
            const Point a = inner.ctm.apply({x0, y0}), b = inner.ctm.apply({x1, y0});
            const Point c = inner.ctm.apply({x1, y1}), d = inner.ctm.apply({x0, y1});
            box = {{PathCmd::Move, 0, 0, 0, 0, a.x, a.y}, {PathCmd::Line, 0, 0, 0, 0, b.x, b.y},
                   {PathCmd::Line, 0, 0, 0, 0, c.x, c.y}, {PathCmd::Line, 0, 0, 0, 0, d.x, d.y},
                   {PathCmd::Close, 0, 0, 0, 0, 0, 0}};
            clip(inner, box, FillRule::NonZero);
        }
        if (m_doc.get(xo, "Group").dict && m_doc.get(m_doc.get(xo, "Group"), "S").is_name("Transparency")) {
            /* drawn as its parts; group knockout and isolation aren't modeled */
        }
        const Object &fres = m_doc.get(xo, "Resources");
        const Transform saved_tm = m_tm, saved_tlm = m_tlm;
        run(data, fres.dict ? fres : res, inner, depth + 1);
        m_tm = saved_tm;
        m_tlm = saved_tlm;
    }

    void ext_gstate(GState &gs, const std::string &name, const Object &res) {
        const Object &g = m_doc.get(m_doc.get(res, "ExtGState"), name.c_str());
        if (!g.dict)
            return;
        for (const auto &kv : *g.dict) {
            const Object &v = m_doc.resolve(kv.second);
            const std::string &k = kv.first;
            if (k == "LW")
                gs.line_width = (float) v.as_number(gs.line_width);
            else if (k == "LC")
                gs.cap = (LineCap) std::clamp(v.as_int(0), 0, 2);
            else if (k == "LJ")
                gs.join = (LineJoin) std::clamp(v.as_int(0), 0, 2);
            else if (k == "ML")
                gs.miter = (float) v.as_number(gs.miter);
            else if (k == "ca")
                gs.fill_alpha = clamp01((float) v.as_number(1));
            else if (k == "CA")
                gs.stroke_alpha = clamp01((float) v.as_number(1));
            else if (k == "D" && v.size() == 2) {
                gs.dash.clear();
                const Object &arr = m_doc.resolve(v[0]);
                for (size_t i = 0; i < arr.size(); ++i)
                    gs.dash.push_back((float) m_doc.resolve(arr[i]).as_number());
                gs.dash_phase = (float) m_doc.resolve(v[1]).as_number();
            } else if (k == "Font" && v.size() == 2) {
                gs.font = font(v[0]);
                gs.font_size = (float) m_doc.resolve(v[1]).as_number();
            } else if (k == "SMask" && !v.is_name("None")) {
                note(m_scene, "soft masks");
            }
        }
    }

    /* ---- The content stream ---- */
    void run(const std::string &data, const Object &res, GState &gs_in, int depth) {
        if (depth > 16)
            return;
        std::vector<GState> stack;
        GState gs = gs_in;
        pdf::Lexer lx((const uint8_t *) data.data(), data.size());
        std::vector<Object> args;
        FillRule pending_clip = FillRule::NonZero;
        bool clip_next = false;
        auto num = [&](size_t i) { return i < args.size() ? (float) args[i].as_number() : 0.f; };
        auto end_path = [&](bool fill, bool stroke, FillRule rule) {
            paint(gs, fill, stroke, rule);
            if (clip_next)
                clip(gs, m_path, pending_clip);
            clip_next = false;
            m_path.clear();
        };
        auto set_color = [&](bool fill) {
            std::shared_ptr<ColorSpace> &cs = fill ? gs.fill_cs : gs.stroke_cs;
            if (cs->kind == ColorSpace::Pattern) {
                (fill ? gs.fill_pattern : gs.stroke_pattern) = true;
                (fill ? gs.fill_shading : gs.stroke_shading) =
                    !args.empty() && args.back().is(Type::Name) ? pattern_shading(args.back().str, res) : nullptr;
                return;
            }
            std::vector<float> c;
            for (const Object &a : args)
                if (a.is_number())
                    c.push_back((float) a.number);
            (fill ? gs.fill : gs.stroke) = to_rgb(*cs, c);
            (fill ? gs.fill_pattern : gs.stroke_pattern) = false;
            (fill ? gs.fill_shading : gs.stroke_shading) = nullptr;
        };
        Object o;
        while (lx.next(o)) {
            if (!o.is(Type::Keyword)) {
                args.push_back(std::move(o));
                if (args.size() > 64)
                    args.erase(args.begin());
                continue;
            }
            const std::string &op = o.str;
            if (op == "q") {
                stack.push_back(gs);
            } else if (op == "Q") {
                if (!stack.empty()) {
                    gs = stack.back();
                    stack.pop_back();
                }
            } else if (op == "cm" && args.size() >= 6) {
                gs.ctm = gs.ctm * Transform{num(0), num(1), num(2), num(3), num(4), num(5)};
            } else if (op == "m") {
                move_to(gs, num(0), num(1));
            } else if (op == "l") {
                line_to(gs, num(0), num(1));
            } else if (op == "c") {
                curve_to(gs, num(0), num(1), num(2), num(3), num(4), num(5));
            } else if (op == "v") {
                curve_to(gs, m_cur.x, m_cur.y, num(0), num(1), num(2), num(3));
            } else if (op == "y") {
                curve_to(gs, num(0), num(1), num(2), num(3), num(2), num(3));
            } else if (op == "h") {
                close_path();
            } else if (op == "re") {
                const float x = num(0), y = num(1), w = num(2), h = num(3);
                move_to(gs, x, y);
                line_to(gs, x + w, y);
                line_to(gs, x + w, y + h);
                line_to(gs, x, y + h);
                close_path();
            } else if (op == "f" || op == "F") {
                end_path(true, false, FillRule::NonZero);
            } else if (op == "f*") {
                end_path(true, false, FillRule::EvenOdd);
            } else if (op == "S") {
                end_path(false, true, FillRule::NonZero);
            } else if (op == "s") {
                close_path();
                end_path(false, true, FillRule::NonZero);
            } else if (op == "B") {
                end_path(true, true, FillRule::NonZero);
            } else if (op == "B*") {
                end_path(true, true, FillRule::EvenOdd);
            } else if (op == "b") {
                close_path();
                end_path(true, true, FillRule::NonZero);
            } else if (op == "b*") {
                close_path();
                end_path(true, true, FillRule::EvenOdd);
            } else if (op == "n") {
                end_path(false, false, FillRule::NonZero);
            } else if (op == "W") {
                clip_next = true;
                pending_clip = FillRule::NonZero;
            } else if (op == "W*") {
                clip_next = true;
                pending_clip = FillRule::EvenOdd;
            } else if (op == "w") {
                gs.line_width = num(0);
            } else if (op == "J") {
                gs.cap = (LineCap) std::clamp((int) num(0), 0, 2);
            } else if (op == "j") {
                gs.join = (LineJoin) std::clamp((int) num(0), 0, 2);
            } else if (op == "M") {
                gs.miter = num(0);
            } else if (op == "d" && args.size() >= 2) {
                gs.dash.clear();
                for (size_t i = 0; i < args[0].size(); ++i)
                    gs.dash.push_back((float) args[0][i].as_number());
                gs.dash_phase = num(1);
            } else if (op == "gs" && !args.empty() && args.back().is(Type::Name)) {
                ext_gstate(gs, args.back().str, res);
            } else if (op == "g" || op == "G") {
                (op == "g" ? gs.fill_cs : gs.stroke_cs) = m_device_gray;
                set_color(op == "g");
            } else if (op == "rg" || op == "RG") {
                (op == "rg" ? gs.fill_cs : gs.stroke_cs) = m_device_rgb;
                set_color(op == "rg");
            } else if (op == "k" || op == "K") {
                (op == "k" ? gs.fill_cs : gs.stroke_cs) = m_device_cmyk;
                set_color(op == "k");
            } else if ((op == "cs" || op == "CS") && !args.empty()) {
                const bool fill = op == "cs";
                std::shared_ptr<ColorSpace> cs = color_space(args.back(), res);
                (fill ? gs.fill_cs : gs.stroke_cs) = cs;
                (fill ? gs.fill_pattern : gs.stroke_pattern) = cs->kind == ColorSpace::Pattern;
                if (cs->kind != ColorSpace::Pattern)
                    (fill ? gs.fill : gs.stroke) = initial(*cs);
            } else if (op == "sc" || op == "scn" || op == "SC" || op == "SCN") {
                set_color(op == "sc" || op == "scn");
            } else if (op == "sh" && !args.empty() && args.back().is(Type::Name)) {
                paint_shading(gs, args.back().str, res);
            } else if (op == "Do" && !args.empty() && args.back().is(Type::Name)) {
                do_xobject(gs, args.back().str, res, depth);
            } else if (op == "BI") {
                inline_image(lx, res, gs);
            } else if (op == "BT") {
                m_tm = m_tlm = Transform();
            } else if (op == "ET") {
            } else if (op == "Tf" && args.size() >= 2) {
                const Object &fonts = m_doc.get(res, "Font");
                gs.font = args[0].is(Type::Name) ? font(m_doc.get(fonts, args[0].str.c_str())) : nullptr;
                gs.font_size = num(1);
            } else if (op == "Tc") {
                gs.char_space = num(0);
            } else if (op == "Tw") {
                gs.word_space = num(0);
            } else if (op == "Tz") {
                gs.hscale = num(0) / 100.f;
            } else if (op == "TL") {
                gs.leading = num(0);
            } else if (op == "Ts") {
                gs.rise = num(0);
            } else if (op == "Tr") {
                gs.render = (int) num(0);
            } else if (op == "Td") {
                m_tlm = m_tlm * Transform::translate(num(0), num(1));
                m_tm = m_tlm;
            } else if (op == "TD") {
                gs.leading = -num(1);
                m_tlm = m_tlm * Transform::translate(num(0), num(1));
                m_tm = m_tlm;
            } else if (op == "Tm" && args.size() >= 6) {
                m_tlm = Transform{num(0), num(1), num(2), num(3), num(4), num(5)};
                m_tm = m_tlm;
            } else if (op == "T*") {
                m_tlm = m_tlm * Transform::translate(0, -gs.leading);
                m_tm = m_tlm;
            } else if (op == "Tj" && !args.empty()) {
                show(gs, args.back().str);
            } else if (op == "'" && !args.empty()) {
                m_tlm = m_tlm * Transform::translate(0, -gs.leading);
                m_tm = m_tlm;
                show(gs, args.back().str);
            } else if (op == "\"" && args.size() >= 3) {
                gs.word_space = num(0);
                gs.char_space = num(1);
                m_tlm = m_tlm * Transform::translate(0, -gs.leading);
                m_tm = m_tlm;
                show(gs, args[2].str);
            } else if (op == "TJ" && !args.empty() && args.back().is(Type::Array)) {
                const Object &arr = args.back();
                for (size_t i = 0; i < arr.size(); ++i) {
                    const Object &e = arr[i];
                    if (e.is(Type::String))
                        show(gs, e.str);
                    else if (e.is_number())
                        m_tm = m_tm * Transform::translate(-(float) e.number / 1000.f * gs.font_size * gs.hscale, 0);
                }
            }
            args.clear();
        }
    }

    /* BI <dict> ID <data> EI: the data runs to an EI between white space */
    void inline_image(pdf::Lexer &lx, const Object &res, const GState &gs) {
        auto dict = std::make_shared<pdf::Dict>();
        Object k;
        while (lx.next(k)) {
            if (k.is(Type::Keyword) && k.str == "ID")
                break;
            if (!k.is(Type::Name))
                continue;
            Object v;
            if (!lx.next(v))
                return;
            (*dict)[k.str] = v;
        }
        const uint8_t *d = lx.data();
        const size_t n = lx.size();
        size_t start = lx.pos();
        if (start < n && (d[start] == ' ' || d[start] == '\n' || d[start] == '\r'))
            ++start;
        size_t end = start;
        for (; end + 2 < n; ++end) {
            const bool ws_before = end == start || d[end - 1] == ' ' || d[end - 1] == '\n' || d[end - 1] == '\r' || d[end - 1] == '\t';
            if (ws_before && d[end] == 'E' && d[end + 1] == 'I' &&
                (end + 2 >= n || d[end + 2] == ' ' || d[end + 2] == '\n' || d[end + 2] == '\r' || d[end + 2] == '\t'))
                break;
        }
        if (end + 2 >= n)
            end = n;
        size_t data_end = end;
        while (data_end > start && (d[data_end - 1] == ' ' || d[data_end - 1] == '\n' || d[data_end - 1] == '\r'))
            --data_end;
        lx.seek(std::min(n, end + 2));
        Object dobj;
        dobj.type = Type::Dict;
        dobj.dict = dict;
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        int w = 0, h = 0;
        const std::string raw((const char *) d + start, data_end - start);
        if (image_bytes(dobj, raw, true, res, gs, bytes, w, h))
            place_image(gs, bytes, w, h);
    }

    struct CachedImage {
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        int w = 0, h = 0;
    };

    const pdf::Document &m_doc;
    VectorScene &m_scene;
    Functions m_fn;
    FT_Library m_ft = nullptr;
    std::map<const void *, std::unique_ptr<PdfFont>> m_fonts;
    std::map<const void *, CachedImage> m_images;
    std::shared_ptr<ColorSpace> m_device_gray, m_device_rgb, m_device_cmyk;
    Path m_path;
    Point m_cur{0, 0}, m_start{0, 0};
    Transform m_base;                    // the page's default space -> the box (patterns use it)
    Transform m_tm, m_tlm;
};

/* The page's box (crop box within the media box) and the transform from
 * PDF space (y up) to the scene (y down, the box's top-left at 0, turned
 * by /Rotate) */
bool page_frame(const pdf::Document &doc, const Object &page, float &w, float &h, Transform &base) {
    auto box = [&](const char *key, float b[4]) {
        const Object &a = doc.get(page, key);
        if (a.size() != 4)
            return false;
        for (int i = 0; i < 4; ++i)
            b[i] = (float) doc.resolve(a[(size_t) i]).as_number();
        if (b[0] > b[2]) std::swap(b[0], b[2]);
        if (b[1] > b[3]) std::swap(b[1], b[3]);
        return b[2] > b[0] && b[3] > b[1];
    };
    float media[4] = {0, 0, 612, 792}, crop[4];
    box("MediaBox", media);
    if (box("CropBox", crop)) {
        crop[0] = std::max(crop[0], media[0]);
        crop[1] = std::max(crop[1], media[1]);
        crop[2] = std::min(crop[2], media[2]);
        crop[3] = std::min(crop[3], media[3]);
        if (!(crop[2] > crop[0]) || !(crop[3] > crop[1]))
            std::copy(media, media + 4, crop);
    } else {
        std::copy(media, media + 4, crop);
    }
    const float x0 = crop[0], y0 = crop[1], x1 = crop[2], y1 = crop[3];
    int rot = doc.get(page, "Rotate").as_int(0) % 360;
    if (rot < 0)
        rot += 360;
    rot = (rot / 90) * 90;
    const float bw = x1 - x0, bh = y1 - y0;
    switch (rot) {
    case 90: base = {0, 1, 1, 0, -y0, -x0}; w = bh; h = bw; break;
    case 180: base = {-1, 0, 0, 1, x1, -y0}; w = bw; h = bh; break;
    case 270: base = {0, -1, -1, 0, y1, x1}; w = bh; h = bw; break;
    default: base = {1, 0, 0, -1, -x0, y1}; w = bw; h = bh; break;
    }
    return true;
}

class PdfDecoder : public VectorDecoder {
public:
    const char *id() const override { return "pdf"; }
    bool sniff(const uint8_t *data, size_t n) const override {
        const size_t head = std::min<size_t>(n, 1024);
        for (size_t i = 0; i + 5 <= head; ++i)
            if (std::memcmp(data + i, "%PDF-", 5) == 0)
                return true;
        return false;
    }
    std::vector<std::pair<std::string, std::string>> types() const override {
        return {{"pdf", "PDF"}};
    }
    bool decode(const uint8_t *data, size_t n, VectorScene *out, std::string *error) const override {
        return decode_page(data, n, 0, out, error);
    }
    int page_count(const uint8_t *data, size_t n) const override {
        pdf::Document doc;
        return doc.open(data, n, nullptr) ? doc.page_count() : 0;
    }
    bool decode_page(const uint8_t *data, size_t n, int index, VectorScene *out,
                     std::string *error) const override {
        if (!out)
            return false;
        *out = VectorScene();
        out->format = "pdf";
        pdf::Document doc;
        if (!doc.open(data, n, error))
            return false;
        if (index < 0 || index >= doc.page_count()) {
            if (error)
                *error = "the PDF has no page " + std::to_string(index + 1);
            return false;
        }
        const Object &page = doc.page(index);
        Transform base;
        if (!page_frame(doc, page, out->width, out->height, base)) {
            if (error)
                *error = "the PDF page has no size";
            return false;
        }
        Interpreter(doc, *out).run_page(page, base);
        return true;
    }
};

} // namespace

void register_pdf_decoder(VectorLoader &loader) {
    loader.add(std::make_unique<PdfDecoder>());
}

} // namespace pagemade
