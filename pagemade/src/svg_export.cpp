/*
 * pagemade/svg_export.cpp — see svg_export.h.
 *
 * Graphics come from build_page's draw list, one item at a time so the
 * stacking order and groups are kept. Text does not: a draw list has glyph
 * ids, and an editable SVG needs the characters. Those come from the
 * composed lines and the story, the way the PDF's ToUnicode map does: each
 * glyph's cluster is a byte offset into its paragraph's text.
 */
#include "svg_export.h"

#include "composer/utf8.h"
#include "drawlist.h"
#include "geometry.h"
#include "image.h"
#include "image_color.h"
#include "vector.h"

#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../ext/glfw/deps/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace pagemade {

namespace {

constexpr float kPicturePpi = 300.f;     // pictures resized to this, as placed
constexpr int kMaxPictureEdge = 8000;    // and never past this many pixels

/* ---- Small writers ---------------------------------------------------- */

std::string num(double v) {
    if (!std::isfinite(v))
        v = 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

std::string xml_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;   // attributes are always in double quotes
        default: out += c;
        }
    }
    return out;
}

std::string hex_color(const Color &c) {
    auto byte = [](float v) { return (int) std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

std::string matrix(const Transform &t) {
    if (t == Transform())
        return {};
    return " transform=\"matrix(" + num(t.a) + " " + num(t.b) + " " + num(t.c) + " " +
           num(t.d) + " " + num(t.e) + " " + num(t.f) + ")\"";
}

std::string path_data(const Path &p) {
    std::string d;
    for (const PathCmd &c : p) {
        switch (c.kind) {
        case PathCmd::Move: d += "M" + num(c.x) + " " + num(c.y); break;
        case PathCmd::Line: d += "L" + num(c.x) + " " + num(c.y); break;
        case PathCmd::Quad:
            d += "Q" + num(c.cx1) + " " + num(c.cy1) + " " + num(c.x) + " " + num(c.y);
            break;
        case PathCmd::Cubic:
            d += "C" + num(c.cx1) + " " + num(c.cy1) + " " + num(c.cx2) + " " + num(c.cy2) + " " +
                 num(c.x) + " " + num(c.y);
            break;
        case PathCmd::Close: d += "Z"; break;
        }
    }
    return d;
}

std::string base64(const uint8_t *data, size_t n) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t) data[i] << 16 | (i + 1 < n ? (uint32_t) data[i + 1] << 8 : 0) |
                           (i + 2 < n ? (uint32_t) data[i + 2] : 0);
        out += tab[v >> 18 & 63];
        out += tab[v >> 12 & 63];
        out += i + 1 < n ? tab[v >> 6 & 63] : '=';
        out += i + 2 < n ? tab[v & 63] : '=';
    }
    return out;
}

void png_sink(void *context, void *data, int size) {
    auto *out = static_cast<std::string *>(context);
    out->append(static_cast<const char *>(data), (size_t) size);
}

/* ---- The document being written --------------------------------------- */

struct Svg {
    std::string defs, body;
    int next = 0;
    std::string id(const char *prefix) { return std::string(prefix) + std::to_string(++next); }
};

/* A clip-path attribute for the frame (0, 0)–(w, h) in the op's space, and
 * the nested clips of a placed graphic. Opens one <g> per clip; returns how
 * many to close. */
int open_clips(Svg &svg, const Transform &xf, float clip_w, float clip_h, const DrawClip &clip) {
    svg.body += "<g" + matrix(xf) + ">";
    int open = 1;
    if (clip_w > 0.f && clip_h > 0.f) {
        const std::string id = svg.id("clip");
        svg.defs += "<clipPath id=\"" + id + "\"><rect x=\"0\" y=\"0\" width=\"" + num(clip_w) +
                    "\" height=\"" + num(clip_h) + "\"/></clipPath>\n";
        svg.body += "<g clip-path=\"url(#" + id + ")\">";
        ++open;
    }
    if (clip) {
        for (const std::vector<ClipPath> &layer : *clip) {
            const std::string id = svg.id("clip");
            svg.defs += "<clipPath id=\"" + id + "\">";
            for (const ClipPath &cp : layer)
                svg.defs += "<path d=\"" + path_data(cp.path) + "\"" +
                            (cp.rule == FillRule::EvenOdd ? " clip-rule=\"evenodd\"" : "") + "/>";
            svg.defs += "</clipPath>\n";
            svg.body += "<g clip-path=\"url(#" + id + ")\">";
            ++open;
        }
    }
    return open;
}

void close_groups(Svg &svg, int n) {
    while (n-- > 0)
        svg.body += "</g>";
    svg.body += "\n";
}

/* A paint attribute's value: the color, or a gradient defined for it. */
std::string paint(Svg &svg, const Color &color, const DrawGradientRef &gradient,
                  const char *opacity_attr) {
    std::string out;
    if (gradient && gradient->stops.size() >= 2) {
        const DrawGradient &g = *gradient;
        const std::string id = svg.id("grad");
        const char *spread = g.spread == Spread::Reflect ? "reflect"
                           : g.spread == Spread::Repeat ? "repeat" : "pad";
        const std::string xf = "matrix(" + num(g.xf.a) + " " + num(g.xf.b) + " " + num(g.xf.c) +
                               " " + num(g.xf.d) + " " + num(g.xf.e) + " " + num(g.xf.f) + ")";
        if (g.radial)
            svg.defs += "<radialGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" "
                        "cx=\"0\" cy=\"0\" r=\"1\" fx=\"" + num(g.focal.x) + "\" fy=\"" +
                        num(g.focal.y) + "\"";
        else
            svg.defs += "<linearGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" "
                        "x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\"";
        svg.defs += " gradientTransform=\"" + xf + "\" spreadMethod=\"" + spread + "\">";
        for (const GradientStop &s : g.stops) {
            svg.defs += "<stop offset=\"" + num(s.offset) + "\" stop-color=\"" + hex_color(s.color) + "\"";
            if (s.color.a < 1.f)
                svg.defs += " stop-opacity=\"" + num(s.color.a) + "\"";
            svg.defs += "/>";
        }
        svg.defs += g.radial ? "</radialGradient>\n" : "</linearGradient>\n";
        out = "\"url(#" + id + ")\"";
    } else {
        out = "\"" + hex_color(color) + "\"";
        if (color.a < 1.f)
            out += std::string(" ") + opacity_attr + "=\"" + num(color.a) + "\"";
    }
    return out;
}

void write_fill(Svg &svg, const DrawFill &f) {
    const int open = open_clips(svg, f.xf, f.clip_w, f.clip_h, f.clip);
    svg.body += "<path d=\"" + path_data(f.path) + "\" fill=" +
                paint(svg, f.color, f.gradient, "fill-opacity") +
                (f.rule == FillRule::EvenOdd ? " fill-rule=\"evenodd\"" : "") + "/>";
    close_groups(svg, open);
}

void write_stroke(Svg &svg, const DrawStroke &s) {
    const int open = open_clips(svg, s.xf, s.clip_w, s.clip_h, s.clip);
    svg.body += "<path d=\"" + path_data(s.path) + "\" fill=\"none\" stroke=" +
                paint(svg, s.color, s.gradient, "stroke-opacity") + " stroke-width=\"" +
                num(s.width) + "\"";
    if (s.cap != LineCap::Butt)
        svg.body += std::string(" stroke-linecap=\"") + (s.cap == LineCap::Round ? "round" : "square") + "\"";
    if (s.join != LineJoin::Miter)
        svg.body += std::string(" stroke-linejoin=\"") + (s.join == LineJoin::Round ? "round" : "bevel") + "\"";
    if (!s.dash.empty()) {
        svg.body += " stroke-dasharray=\"";
        for (size_t i = 0; i < s.dash.size(); ++i)
            svg.body += (i ? " " : "") + num(s.dash[i]);
        svg.body += "\"";
        if (s.dash_offset != 0.f)
            svg.body += " stroke-dashoffset=\"" + num(s.dash_offset) + "\"";
    }
    svg.body += "/>";
    close_groups(svg, open);
}

/* ---- Pictures ------------------------------------------------------------ */

/* A data: URI for the picture as placed `w` x `h` points wide (scaled by
 * the item's transform `k`). Empty when the picture can't be read. */
std::string encoded_picture(const std::vector<uint8_t> &raw, const ImageMetadata &meta, float w,
                            float h, float k) {
    const int want = std::clamp((int) std::ceil(std::max(w, h) * k / 72.f * kPicturePpi), 16,
                                kMaxPictureEdge);
    const int have = std::max(meta.width_px, meta.height_px);
    const bool jpeg = meta.format == "jpeg";
    const bool png = meta.format == "png";
    /* As it is: a JPEG or PNG every app reads, upright, sRGB already, and
     * not far past what the placement needs */
    if ((jpeg || png) && meta.orientation <= 1 && !icc_needs_conversion(meta.icc) &&
        have > 0 && have <= want * 3 / 2)
        return std::string("data:image/") + (jpeg ? "jpeg" : "png") + ";base64," +
               base64(raw.data(), raw.size());
    std::vector<uint8_t> rgba;
    int dw = 0, dh = 0;
    std::string err;
    if (!make_display(raw.data(), raw.size(), have > want ? want : 0, &rgba, &dw, &dh, &err) ||
        dw <= 0 || dh <= 0)
        return {};
    bool alpha = false;
    for (size_t i = 3; i < rgba.size() && !alpha; i += 4)
        alpha = rgba[i] != 255;
    std::string data;
    if (!alpha && encode_jpeg(rgba.data(), dw, dh, 90, &data))
        return "data:image/jpeg;base64," + base64((const uint8_t *) data.data(), data.size());
    data.clear();
    if (!stbi_write_png_to_func(png_sink, &data, dw, dh, 4, rgba.data(), dw * 4))
        return {};
    return "data:image/png;base64," + base64((const uint8_t *) data.data(), data.size());
}

std::string picture_uri(const ImageStore *images, uint32_t asset, float w, float h, float k) {
    if (!images || !asset)
        return {};
    ImageStore::Bytes bytes;
    ImageMetadata meta;
    if (!images->export_source(asset, &bytes, &meta)) {
        const std::string path = images->path_of(asset);
        if (path.empty())
            return {};
        ImageImport read = read_image_import(path);
        if (!read.ok)
            return {};
        bytes = std::make_shared<const std::vector<uint8_t>>(std::move(read.bytes));
        meta = std::move(read.meta);
    }
    if (!bytes || bytes->empty())
        return {};
    return encoded_picture(*bytes, meta, w, h, k);
}

void write_image(Svg &svg, const DrawImage &im, const ImageStore *images) {
    const bool inline_bytes = im.bytes && !im.bytes->empty();
    if (!im.asset && !inline_bytes)
        return;                          // an empty frame: only its border prints
    const float k = std::sqrt(std::fabs(im.xf.a * im.xf.d - im.xf.b * im.xf.c));
    std::string uri;
    if (inline_bytes) {
        ImageMetadata meta;
        image_loader().probe(im.bytes->data(), im.bytes->size(), &meta, nullptr);
        uri = encoded_picture(*im.bytes, meta, im.w, im.h, k);
    } else {
        uri = picture_uri(images, im.asset, im.w, im.h, k);
    }
    const int open = open_clips(svg, im.xf, im.clip_w, im.clip_h, im.clip);
    if (uri.empty()) {
        /* Missing: the gray box with a cross that PDF prints */
        svg.body += "<rect x=\"0\" y=\"0\" width=\"" + num(im.clip_w) + "\" height=\"" +
                    num(im.clip_h) + "\" fill=\"#e6e6e6\"/><path d=\"M0 0L" + num(im.clip_w) + " " +
                    num(im.clip_h) + "M" + num(im.clip_w) + " 0L0 " + num(im.clip_h) +
                    "\" stroke=\"#994040\" stroke-width=\"1\"/>";
    } else {
        svg.body += "<image x=\"" + num(im.x) + "\" y=\"" + num(im.y) + "\" width=\"" + num(im.w) +
                    "\" height=\"" + num(im.h) + "\" preserveAspectRatio=\"none\"";
        if (im.opacity < 1.f)
            svg.body += " opacity=\"" + num(im.opacity) + "\"";
        svg.body += " xlink:href=\"" + uri + "\"/>";
    }
    close_groups(svg, open);
}

/* ---- Text -------------------------------------------------------------------- */

/* What another program needs to find the face: the family in the font's own
 * name table (the typographic family, else the family), its weight and
 * whether it is italic. */
struct FontFace {
    std::string family;
    int weight = 400;
    bool italic = false;
};

std::string face_name(hb_face_t *face, hb_ot_name_id_t id) {
    unsigned int len = 0;
    for (hb_language_t lang : {hb_language_from_string("en", -1), HB_LANGUAGE_INVALID}) {
        len = hb_ot_name_get_utf8(face, id, lang, nullptr, nullptr);
        if (len) {
            std::string out(len + 1, '\0');
            unsigned int size = len + 1;
            hb_ot_name_get_utf8(face, id, lang, &size, &out[0]);
            out.resize(size);
            return out;
        }
    }
    return {};
}

const FontFace &face_of(const Font *font, std::map<const Font *, FontFace> &cache) {
    auto it = cache.find(font);
    if (it != cache.end())
        return it->second;
    FontFace f;
    if (font && font->hb_face()) {
        f.family = face_name(font->hb_face(), HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
        if (f.family.empty())
            f.family = face_name(font->hb_face(), HB_OT_NAME_ID_FONT_FAMILY);
#if HB_VERSION_ATLEAST(3, 0, 0)
        if (font->hb()) {
            f.weight = (int) std::lround(hb_style_get_value(font->hb(), HB_STYLE_TAG_WEIGHT));
            f.italic = hb_style_get_value(font->hb(), HB_STYLE_TAG_ITALIC) > 0.5f ||
                       std::fabs(hb_style_get_value(font->hb(), HB_STYLE_TAG_SLANT_RATIO)) > 0.01f;
        }
#endif
    }
    if (f.family.empty() && font)
        f.family = font->postscript_name();
    f.weight = std::clamp(f.weight, 100, 900);
    return cache.emplace(font, std::move(f)).first->second;
}

/* pagemade's family aliases fall back to the matching generic family */
const char *generic_family(const std::string &family) {
    if (family == "Serif")
        return "serif";
    if (family == "Mono" || family == "Monospace")
        return "monospace";
    return "sans-serif";
}

/* Capitals for all-caps text: ASCII, Latin-1 and Latin Extended-A, which
 * covers what the built-in faces set. Anything else is left as typed. */
std::string upper(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8_decode(s, i);
        const size_t n = std::max<size_t>(1, utf8_len(s, i));
        /* Latin Extended-A pairs capital, small; the small one is odd in
         * some blocks and even in others */
        const bool odd_small = (cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177);
        const bool even_small = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
        if (cp >= 'a' && cp <= 'z')
            cp -= 32;
        else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
            cp -= 32;
        else if (cp == 0xFF)
            cp = 0x178;
        else if ((odd_small && (cp & 1) && cp != 0x131) || (even_small && !(cp & 1)))
            cp -= 1;
        if (cp < 0x80) {
            out += (char) cp;
        } else if (cp < 0x800) {
            out += (char) (0xC0 | cp >> 6);
            out += (char) (0x80 | (cp & 63));
        } else {
            out.append(s, i, n);         // three and four byte forms: as typed
        }
        i += n;
    }
    return out;
}

/* Characters that are layout, not text */
bool layout_only(uint32_t cp) {
    return cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x0B || cp == kFrameBreak ||
           cp == 0x85 || cp == 0x2028 || cp == 0x2029 || cp == 0xAD || cp == kRightIndentTab;
}

/* The character `font` draws as `gid`, among `candidates`; 0 if none. */
uint32_t char_for_glyph(const Font *font, uint32_t gid, const std::u32string &candidates) {
    if (!font || !font->hb())
        return 0;
    for (char32_t c : candidates) {
        hb_codepoint_t g = 0;
        if (hb_font_get_nominal_glyph(font->hb(), (hb_codepoint_t) c, &g) && g == gid)
            return (uint32_t) c;
    }
    return 0;
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char) cp;
    } else if (cp < 0x800) {
        out += (char) (0xC0 | cp >> 6);
        out += (char) (0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        out += (char) (0xE0 | cp >> 12);
        out += (char) (0x80 | (cp >> 6 & 63));
        out += (char) (0x80 | (cp & 63));
    } else {
        out += (char) (0xF0 | cp >> 18);
        out += (char) (0x80 | (cp >> 12 & 63));
        out += (char) (0x80 | (cp >> 6 & 63));
        out += (char) (0x80 | (cp & 63));
    }
}

/* One <tspan>: glyphs of one run and one style, from the pen position of
 * the first */
struct Segment {
    const GlyphRun *run = nullptr;
    const CharStyle *style = nullptr;
    float x = 0, y = 0;
    std::string text;
};

/* The character style in force at byte `at` of a paragraph */
const CharStyle *style_at(const Paragraph &p, uint32_t at) {
    uint32_t start = 0;
    for (const Run &r : p.runs) {
        const uint32_t end = start + (uint32_t) r.text.size();
        if (at < end)
            return &r.style;
        start = end;
    }
    return p.runs.empty() ? nullptr : &p.runs.back().style;
}

std::string tspan(const Segment &s, const PageDoc &doc, std::map<const Font *, FontFace> &faces,
                  bool positioned) {
    const GlyphRun &run = *s.run;
    const FontFace &face = face_of(run.font, faces);
    const CharStyle *cs = s.style;
    std::string text = s.text;
    if (cs && cs->caps == Caps::All)
        text = upper(text);
    std::string out = "<tspan";
    if (positioned)
        out += " x=\"" + num(s.x) + "\" y=\"" + num(s.y) + "\"";
    out += " font-family=\"'" + xml_escape(face.family) + "', " +
           generic_family(cs ? cs->family : std::string()) + "\"";
    out += " font-size=\"" + num(run.size) + "\"";
    if (face.weight != 400)
        out += " font-weight=\"" + std::to_string(face.weight) + "\"";
    if (face.italic)
        out += " font-style=\"italic\"";
    if (cs && cs->caps == Caps::Small)
        out += " font-variant=\"small-caps\"";
    if (cs && cs->tracking != 0.f)
        out += " letter-spacing=\"" + num(cs->tracking / 1000.f * run.size) + "\"";
    if (!cs || cs->kerning == false)
        out += " font-kerning=\"none\"";
    const Color color = doc.text_color(run.swatch, run.tint, run.color);
    out += " fill=\"" + hex_color(color) + "\"";
    if (color.a < 1.f)
        out += " fill-opacity=\"" + num(color.a) + "\"";
    if (run.underline || run.strike) {
        out += " text-decoration=\"";
        out += run.underline ? "underline" : "";
        out += run.underline && run.strike ? " " : "";
        out += run.strike ? "line-through" : "";
        out += "\"";
    }
    out += ">" + xml_escape(text) + "</tspan>";
    return out;
}

void write_text_frame(Svg &svg, const PageDoc &doc, const Item &frame,
                      const std::vector<Composition> &comps,
                      std::map<const Font *, FontFace> &faces) {
    size_t thread_index = 0;
    const StoryEntry *se = doc.story_of(frame.id, &thread_index);
    if (!se)
        return;
    const size_t si = doc.story_index(se->id);
    if (si >= comps.size())
        return;
    const Story &story = se->story;
    const std::vector<int> numbers = list_numbers(story);
    bool any = false;
    std::string lines;
    for (const ComposedLine &line : comps[si].lines) {
        if (line.frame != thread_index || line.para >= story.paragraphs.size())
            continue;
        const Paragraph &para = story.paragraphs[line.para];
        const std::string text = paragraph_text(para);
        /* Every glyph of the line in order, with its run */
        struct G { const GlyphRun *run; const PlacedGlyph *g; };
        std::vector<G> glyphs;
        for (const GlyphRun &r : line.runs)
            for (const PlacedGlyph &g : r.glyphs)
                glyphs.push_back({&r, &g});
        const bool justified = is_justified(para.style.align);
        std::vector<Segment> segs;
        auto open_seg = [&](const G &at, const CharStyle *cs) {
            Segment s;
            s.run = at.run;
            s.style = cs;
            s.x = at.g->x;
            s.y = at.g->y;
            segs.push_back(std::move(s));
        };
        std::u32string leaders = U"._-\u00B7\u2022\u2026";
        for (const TabStop &t : para.style.tabs)
            for (size_t i = 0; i < t.leader.size(); i += std::max<size_t>(1, utf8_len(t.leader, i)))
                leaders += (char32_t) utf8_decode(t.leader, i);
        bool break_next = true;          // the next glyph starts a segment
        bool in_leader = false;          // the open segment is a tab leader
        for (size_t i = 0; i < glyphs.size(); ++i) {
            const G &cur = glyphs[i];
            const PlacedGlyph &g = *cur.g;
            if (in_leader && !(g.flags & PlacedGlyph::Inserted)) {
                in_leader = false;
                break_next = true;       // text after a leader has its own x
            }
            if (g.flags & PlacedGlyph::Invisible) {
                break_next = true;       // a tab or a break: the next text has its own x
                continue;
            }
            if (g.flags & PlacedGlyph::Inserted) {
                /* Not from the text: a list marker (its own run, clusters
                 * 0), a tab leader or the hyphen of a break */
                const bool marker = para.style.list != ListKind::None && line.byte_start == 0 &&
                                    std::all_of(cur.run->glyphs.begin(), cur.run->glyphs.end(),
                                                [](const PlacedGlyph &q) {
                                                    return (q.flags & PlacedGlyph::Inserted) != 0;
                                                });
                if (marker) {
                    if (&g == &cur.run->glyphs.front()) {
                        open_seg(cur, para.runs.empty() ? nullptr : &para.runs.front().style);
                        segs.back().text = list_marker(para.style, numbers[line.para]);
                    }
                    break_next = true;
                    continue;
                }
                /* The break's hyphen ends the line and joins the word before
                 * it. Leaders are appended to the tab's run after its other
                 * glyphs, out of reading order: they get a <tspan> of their
                 * own, and the text after them another. */
                const bool hyphen = line.hyphenated && i + 1 == glyphs.size();
                const uint32_t c = char_for_glyph(cur.run->font, g.gid,
                                                  hyphen ? std::u32string(U"-\u2010") : leaders);
                if (!c)
                    continue;
                if (!hyphen && !in_leader)
                    break_next = true;
                if (break_next || segs.empty() || segs.back().run != cur.run) {
                    open_seg(cur, style_at(para, g.cluster));
                    break_next = false;
                }
                append_utf8(segs.back().text, c);
                in_leader = !hyphen;
                continue;
            }
            /* The characters this glyph draws: up to the next glyph from
             * the text, or the end of the line */
            uint32_t end = line.byte_end;
            for (size_t j = i + 1; j < glyphs.size(); ++j)
                if (!(glyphs[j].g->flags & PlacedGlyph::Inserted) && glyphs[j].g->cluster > g.cluster) {
                    end = glyphs[j].g->cluster;
                    break;
                }
            /* A glyph sharing its cluster with the one before (a mark, a
             * glyph decomposed in shaping) adds no characters */
            const bool repeat = i > 0 && !(glyphs[i - 1].g->flags & PlacedGlyph::Inserted) &&
                                glyphs[i - 1].g->cluster == g.cluster;
            const CharStyle *cs = style_at(para, g.cluster);
            if (break_next || segs.empty() || segs.back().run != cur.run ||
                segs.back().style != cs || std::fabs(segs.back().y - g.y) > 0.01f) {
                open_seg(cur, cs);
                break_next = false;
            }
            if (repeat)
                continue;
            for (size_t at = g.cluster; at < end && at < text.size();) {
                const uint32_t cp = utf8_decode(text, at);
                const size_t n = std::max<size_t>(1, utf8_len(text, at));
                if (is_page_marker(cp)) {
                    /* Its glyphs are the digits it shows */
                    for (size_t j = i; j < glyphs.size() && glyphs[j].g->cluster == g.cluster; ++j)
                        if (uint32_t d = char_for_glyph(glyphs[j].run->font, glyphs[j].g->gid,
                                                        U"0123456789#"))
                            append_utf8(segs.back().text, d);
                } else if (!layout_only(cp)) {
                    segs.back().text.append(text, at, n);
                }
                at += n;
            }
            /* Justified text moves its words apart: each word keeps its place */
            if (justified && end <= text.size() && g.cluster < text.size() && text[g.cluster] == ' ')
                break_next = true;
        }
        /* Trailing spaces draw nothing; most programs would still measure them */
        while (!segs.empty()) {
            std::string &t = segs.back().text;
            while (!t.empty() && t.back() == ' ')
                t.pop_back();
            if (!t.empty())
                break;
            segs.pop_back();
        }
        if (segs.empty())
            continue;
        any = true;
        /* One <text> per line. A set width other than 100% scales the
         * glyphs: that part goes in a <text> of its own, scaled. */
        std::string plain;
        for (const Segment &s : segs) {
            if (std::fabs(s.run->hscale - 100.f) > 0.01f) {
                lines += "<text xml:space=\"preserve\" transform=\"translate(" + num(s.x) + " " +
                         num(s.y) + ") scale(" + num(s.run->hscale / 100.f) + " 1)\">" +
                         tspan(Segment{s.run, s.style, 0.f, 0.f, s.text}, doc, faces, true) +
                         "</text>\n";
                continue;
            }
            plain += tspan(s, doc, faces, true);
        }
        if (!plain.empty())
            lines += "<text xml:space=\"preserve\">" + plain + "</text>\n";
    }
    if (!any)
        return;
    svg.body += "<g" + matrix(frame.xf) + ">\n" + lines + "</g>\n";
}

} // namespace

std::string items_text(const PageDoc &doc, size_t page, const std::vector<ItemId> &ids,
                       const std::vector<Composition> &comps) {
    if (page >= doc.pages.size())
        return {};
    std::string out;
    for (const Item &it : doc.pages[page].items) {
        if (!it.is_text() || !doc.item_visible(it) ||
            std::find(ids.begin(), ids.end(), it.id) == ids.end())
            continue;
        size_t thread_index = 0;
        const StoryEntry *se = doc.story_of(it.id, &thread_index);
        const size_t si = se ? doc.story_index(se->id) : SIZE_MAX;
        if (!se || si >= comps.size())
            continue;
        const Story &story = se->story;
        const std::vector<int> numbers = list_numbers(story);
        std::string block;
        for (const ComposedLine &line : comps[si].lines) {
            if (line.frame != thread_index || line.para >= story.paragraphs.size())
                continue;
            const Paragraph &para = story.paragraphs[line.para];
            const std::string text = paragraph_text(para);
            if (line.byte_start == 0 && para.style.list != ListKind::None)
                block += list_marker(para.style, numbers[line.para]) + "\t";
            for (size_t at = line.byte_start; at < line.byte_end && at < text.size();) {
                const uint32_t cp = utf8_decode(text, at);
                const size_t n = std::max<size_t>(1, utf8_len(text, at));
                if (cp == '\n' || cp == 0x0B || cp == kFrameBreak || cp == 0x85 || cp == 0x2028 ||
                    cp == 0x2029)
                    block += '\n';
                else if (cp == '\t' || cp == kRightIndentTab)
                    block += '\t';
                else if (is_page_marker(cp)) {
                    /* The number it shows: its glyphs on this line are digits */
                    for (const GlyphRun &r : line.runs)
                        for (const PlacedGlyph &g : r.glyphs)
                            if (g.cluster == at)
                                if (uint32_t d = char_for_glyph(r.font, g.gid, U"0123456789#"))
                                    append_utf8(block, d);
                } else if (cp != 0xAD && cp != '\r')
                    block.append(text, at, n);
                at += n;
            }
            if (line.para_end)
                block += '\n';
        }
        if (block.empty())
            continue;
        if (!out.empty() && out.back() != '\n')
            out += '\n';
        out += block;
    }
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

std::string items_svg(const PageDoc &doc, size_t page, const std::vector<ItemId> &ids,
                      const std::vector<Composition> &comps, const ImageStore *images,
                      const VectorStore *vectors) {
    if (page >= doc.pages.size())
        return {};
    auto chosen = [&](const Item &it) {
        return std::find(ids.begin(), ids.end(), it.id) != ids.end() && doc.item_visible(it);
    };
    /* The bounds, with room for strokes that stand past the frames */
    Bounds b;
    float pad = 0.f;
    for (const Item &it : doc.pages[page].items) {
        if (!chosen(it))
            continue;
        b.add(item_bounds(it));
        if (const Shape *sh = it.shape())
            pad = std::max(pad, sh->stroke.none() ? 0.f : sh->stroke.weight);
        if (const PlacedImage *im = it.image())
            pad = std::max(pad, im->stroke.none() ? 0.f : im->stroke.weight);
        if (const PlacedVector *pv = it.vector())
            pad = std::max(pad, pv->stroke.none() ? 0.f : pv->stroke.weight);
    }
    if (b.empty())
        return {};
    pad = std::ceil(pad * 0.5f + 0.5f);
    const float x0 = b.x0 - pad, y0 = b.y0 - pad;
    const float w = std::max(1.f, b.x1 - b.x0 + 2 * pad), h = std::max(1.f, b.y1 - b.y0 + 2 * pad);

    Svg svg;
    std::map<const Font *, FontFace> faces;
    uint32_t group = 0;                  // the open <g>, if any
    for (const Item &it : doc.pages[page].items) {
        if (!chosen(it))
            continue;
        if (it.group != group) {
            if (group)
                svg.body += "</g>\n";
            group = it.group;
            if (group)
                svg.body += "<g id=\"group" + std::to_string(group) + "\">\n";
        }
        if (it.is_text()) {
            write_text_frame(svg, doc, it, comps, faces);
            continue;
        }
        const std::vector<ItemId> one{it.id};
        for (const DrawOp &op : build_page(doc, page, comps, &one, true, vectors)) {
            if (const DrawFill *f = std::get_if<DrawFill>(&op))
                write_fill(svg, *f);
            else if (const DrawStroke *s = std::get_if<DrawStroke>(&op))
                write_stroke(svg, *s);
            else if (const DrawImage *im = std::get_if<DrawImage>(&op))
                write_image(svg, *im, images);
        }
    }
    if (group)
        svg.body += "</g>\n";

    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                      "xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" width=\"" +
                      num(w) + "pt\" height=\"" + num(h) + "pt\" viewBox=\"" + num(x0) + " " +
                      num(y0) + " " + num(w) + " " + num(h) + "\">\n";
    if (!svg.defs.empty())
        out += "<defs>\n" + svg.defs + "</defs>\n";
    out += svg.body;
    out += "</svg>\n";
    return out;
}

} // namespace pagemade
