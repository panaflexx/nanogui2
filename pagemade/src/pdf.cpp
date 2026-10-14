/*
 * pagemade/pdf.cpp — see pdf.h.
 */
#include "pdf.h"

#include "composer/font.h"
#include "composer/utf8.h"
#include "drawlist.h"
#include "image.h"
#include "image_color.h"
#include "page.h"

#include <hb.h>
#include <hb-subset.h>
#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <ctime>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

namespace pagemade {

namespace {

/* ---- Small helpers ------------------------------------------------------- */

/* Points with up to three decimals, nothing trailing. */
std::string num(float v) {
    if (std::fabs(v) < 5e-4f)
        v = 0;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.3f", (double) v);
    std::string s = buf;
    while (s.size() > 1 && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s.empty() ? "0" : s;
}

std::string hex16(uint32_t cp) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", cp);
    return buf;
}

/* UTF-8 -> big-endian UTF-16 hex for ToUnicode. Astral scalars are surrogate pairs. */
std::string utf16_hex(const std::string &utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = utf8_decode(utf8, i);
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out += hex16(0xD800 + (cp >> 10)) + hex16(0xDC00 + (cp & 0x3FF));
        } else {
            out += hex16(cp);
        }
        i += utf8_len(utf8, i);
    }
    return out;
}

std::optional<std::string> flate(const std::string &in) {
    mz_ulong bound = mz_compressBound((mz_ulong) in.size());
    std::string out(bound, '\0');
    mz_ulong have = bound;
    if (mz_compress2((unsigned char *) out.data(), &have,
                     (const unsigned char *) in.data(), (mz_ulong) in.size(), 6) != MZ_OK)
        return std::nullopt;
    out.resize(have);
    return out;
}

/* ---- The writer ------------------------------------------------------------
 *
 * Objects are collected as strings; byte offsets and the xref table are
 * computed when the file is assembled.
 */
struct Writer {
    std::vector<std::string> objects;
    bool ok = true;
    std::map<std::string, int> icc_objects;   // profile bytes -> stream, written once

    int add(std::string body) {
        objects.push_back(std::move(body));
        return (int) objects.size();
    }
    /* A stream object, Flate-compressed. A failed compress marks the file unusable. */
    int add_stream(const std::string &dict, const std::string &data) {
        auto z = flate(data);
        if (!z) {
            ok = false;
            z = std::string();
        }
        return add("<< " + dict + " /Length " + std::to_string(z->size()) +
                   " /Filter /FlateDecode >>\nstream\n" + *z + "\nendstream");
    }
    /* A stream already encoded (a JPEG's own bytes). `dict` includes /Filter. */
    int add_raw_stream(const std::string &dict, const std::string &data) {
        return add("<< " + dict + " /Length " + std::to_string(data.size()) +
                   " >>\nstream\n" + data + "\nendstream");
    }

    /* `trailer` adds keys to the trailer (/Info, /ID). */
    bool write(const std::string &path, int root, const char *version = "1.7",
               const std::string &trailer = std::string()) {
        if (!ok)
            return false;
        std::string out = std::string("%PDF-") + version + "\n%\xE2\xE3\xCF\xD3\n";
        std::vector<size_t> offs(objects.size());
        for (size_t i = 0; i < objects.size(); ++i) {
            offs[i] = out.size();
            out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        size_t xref = out.size();
        out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
        out += "0000000000 65535 f \n";
        char buf[32];
        for (size_t off : offs) {
            std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
            out += buf;
        }
        out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
               " /Root " + std::to_string(root) + " 0 R" + trailer + " >>\n"
               "startxref\n" + std::to_string(xref) + "\n%%EOF\n";
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    }
};

/* ---- Fonts ---------------------------------------------------------------
 *
 * One embedded font per used face: the face subset to the used glyphs
 * (ids retained, so a two-byte character code is the glyph id), widths in
 * thousandths of an em, and a ToUnicode CMap so the text copies and
 * searches as the original characters.
 */
struct PdfFont {
    const Font *font = nullptr;
    std::set<uint32_t> gids;
    /* gid -> the text it came from (a ligature's glyph maps to both chars). */
    std::map<uint32_t, std::string> unicode;
};

/* The text span of every used glyph, recovered from the compositions
 * (clusters are byte offsets into the paragraph's text). */
void collect_unicodes(const PageDoc &doc, const std::vector<Composition> &comps,
                      std::map<const Font *, PdfFont> &fonts, const std::set<size_t> &pages) {
    for (size_t si = 0; si < comps.size() && si < doc.stories.size(); ++si) {
        const Story &story = doc.stories[si].story;
        for (const ComposedLine &l : comps[si].lines) {
            if (l.para >= story.paragraphs.size())
                continue;
            /* Only glyphs that are drawn are embedded: on an exported page,
             * on a visible layer */
            if (l.frame < doc.stories[si].thread.size()) {
                size_t pg = 0;
                const Item *frame = doc.find_item(doc.stories[si].thread[l.frame], &pg);
                if (frame && (!pages.count(pg) || !doc.item_visible(*frame)))
                    continue;
            }
            const std::string text = paragraph_text(story.paragraphs[l.para]);
            for (size_t ri = 0; ri < l.runs.size(); ++ri) {
                const GlyphRun &r = l.runs[ri];
                if (!r.font)
                    continue;
                PdfFont &pf = fonts[r.font];
                pf.font = r.font;
                for (size_t i = 0; i < r.glyphs.size(); ++i) {
                    const PlacedGlyph &g = r.glyphs[i];
                    if (g.flags & PlacedGlyph::Invisible)
                        continue;
                    pf.gids.insert(g.gid);
                    if (pf.unicode.count(g.gid) || (g.flags & PlacedGlyph::Inserted))
                        continue;
                    /* A page marker's glyphs are the digits it shows: map each
                     * to its digit, not to the marker in the text */
                    if (g.cluster < text.size()) {
                        size_t at = g.cluster;
                        if (is_page_marker(utf8_decode(text, at))) {
                            for (char c : std::string("0123456789#")) {
                                hb_codepoint_t gid = 0;
                                if (hb_font_get_nominal_glyph(r.font->hb(), (hb_codepoint_t) c, &gid) &&
                                    gid == g.gid) {
                                    pf.unicode[g.gid] = std::string(1, c);
                                    break;
                                }
                            }
                            continue;
                        }
                    }
                    /* A ligature's cluster is its first character. The end is
                     * the next glyph on the line, including a later run: the
                     * last glyph of a run has no neighbor inside the run, and
                     * one character would drop the rest of "fi". */
                    uint32_t from = g.cluster, to = from;
                    bool found = false;
                    auto take = [&](uint32_t cluster) {
                        if (!found && cluster > from) {
                            to = cluster;
                            found = true;
                        }
                    };
                    for (size_t k = i + 1; k < r.glyphs.size(); ++k)
                        take(r.glyphs[k].cluster);
                    for (size_t rj = ri + 1; rj < l.runs.size() && !found; ++rj)
                        for (const PlacedGlyph &ng : l.runs[rj].glyphs)
                            take(ng.cluster);
                    if (!found)
                        to = l.byte_end;
                    to = std::min(to, (uint32_t) text.size());
                    if (from < to)
                        pf.unicode[g.gid] = text.substr(from, to - from);
                }
            }
        }
    }
}

/* Subset and embed one face; returns false when hb-subset fails. */
bool embed_font(Writer &w, PdfFont &pf, int &out_res, int &tag_seq) {
    const Font *font = pf.font;
    const int upem = font->units_per_em();
    auto em = [&](float v) { return std::lround(v * 1000.0f / upem); };

    hb_subset_input_t *input = hb_subset_input_create_or_fail();
    if (!input)
        return false;
    hb_set_t *keep = hb_subset_input_glyph_set(input);
    hb_set_add(keep, 0);   // .notdef
    for (uint32_t gid : pf.gids)
        hb_set_add(keep, gid);
    hb_subset_input_set_flags(input, HB_SUBSET_FLAGS_RETAIN_GIDS |
                                     HB_SUBSET_FLAGS_PASSTHROUGH_UNRECOGNIZED);
    hb_face_t *sub = hb_subset_or_fail(font->hb_face(), input);
    hb_subset_input_destroy(input);
    if (!sub)
        return false;
    const bool cff = font->is_cff();
    /* CIDFontType0C takes the raw CFF program, not the sfnt wrapper. */
    hb_blob_t *blob = cff ? hb_face_reference_table(sub, HB_TAG('C', 'F', 'F', ' '))
                          : hb_face_reference_blob(sub);
    unsigned blen = 0;
    const char *bdata = hb_blob_get_data(blob, &blen);
    std::string bytes(bdata, blen);
    hb_blob_destroy(blob);
    hb_face_destroy(sub);

    char tag[8];
    std::snprintf(tag, sizeof tag, "PGM%c%c%c+", 'A' + (tag_seq / 26 / 26) % 26,
                  'A' + (tag_seq / 26) % 26, 'A' + tag_seq % 26);
    ++tag_seq;
    const std::string ps = tag + font->postscript_name();

    int file = w.add_stream(cff ? "/Subtype /CIDFontType0C" : "/Length1 " +
                                std::to_string(bytes.size()),
                            bytes);

    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (uint32_t gid : pf.gids) {
        float a, b, c, d;
        font->glyph_bounds(gid, a, b, c, d);
        x0 = std::min(x0, a); y0 = std::min(y0, b);
        x1 = std::max(x1, c); y1 = std::max(y1, d);
    }
    if (x0 > x1)
        x0 = y0 = x1 = y1 = 0;

    int desc = w.add("<< /Type /FontDescriptor /FontName /" + ps +
                     " /Flags 4 /FontBBox [" + std::to_string(em(x0)) + " " +
                     std::to_string(em(y0)) + " " + std::to_string(em(x1)) + " " +
                     std::to_string(em(y1)) + "] /ItalicAngle 0 /Ascent " +
                     std::to_string(em(font->ascender())) + " /Descent " +
                     std::to_string(em(font->descender())) + " /CapHeight " +
                     std::to_string(em(font->cap_height())) + " /StemV 80 " +
                     (cff ? "/FontFile3 " : "/FontFile2 ") + std::to_string(file) +
                     " 0 R >>");

    /* Widths: one [gid [w]] chunk per glyph keeps the W array simple. */
    std::string widths;
    for (uint32_t gid : pf.gids)
        widths += " " + std::to_string(gid) + " [" +
                  std::to_string(em(font->advance(gid))) + "]";
    std::string cid = "<< /Type /Font /Subtype /" +
                      std::string(cff ? "CIDFontType0" : "CIDFontType2") +
                      " /BaseFont /" + ps +
                      " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity)"
                      " /Supplement 0 >> /FontDescriptor " + std::to_string(desc) +
                      " 0 R /DW " + std::to_string(em(font->advance(font->space_glyph()))) +
                      " /W [" + widths + " ]";
    if (!cff)
        cid += " /CIDToGIDMap /Identity";   // subsetting retained the gids
    cid += " >>";
    int cidfont = w.add(std::move(cid));

    /* ToUnicode, in chunks of 100 entries. */
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    size_t n = 0;
    size_t total = pf.unicode.size();
    for (auto it = pf.unicode.begin(); it != pf.unicode.end();) {
        size_t chunk = std::min((size_t) 100, total - n);
        cmap += std::to_string(chunk) + " beginbfchar\n";
        for (size_t k = 0; k < chunk; ++k, ++it)
            cmap += "<" + hex16(it->first) + "> <" + utf16_hex(it->second) + ">\n";
        cmap += "endbfchar\n";
        n += chunk;
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    int touni = w.add_stream("", cmap);

    out_res = w.add("<< /Type /Font /Subtype /Type0 /BaseFont /" + ps +
                    " /Encoding /Identity-H /DescendantFonts [" +
                    std::to_string(cidfont) + " 0 R] /ToUnicode " +
                    std::to_string(touni) + " 0 R >>");
    return true;
}

/* ---- Page content ---------------------------------------------------------- */

void emit_path(std::string &s, const Path &p) {
    float px = 0, py = 0;
    for (const PathCmd &c : p) {
        switch (c.kind) {
        case PathCmd::Move: s += num(c.x) + " " + num(c.y) + " m\n"; px = c.x; py = c.y; break;
        case PathCmd::Line: s += num(c.x) + " " + num(c.y) + " l\n"; px = c.x; py = c.y; break;
        case PathCmd::Quad: {
            /* PDF has only cubics: elevate the quadratic. */
            float c1x = px + 2.f / 3.f * (c.cx1 - px), c1y = py + 2.f / 3.f * (c.cy1 - py);
            float c2x = c.x + 2.f / 3.f * (c.cx1 - c.x), c2y = c.y + 2.f / 3.f * (c.cy1 - c.y);
            s += num(c1x) + " " + num(c1y) + " " + num(c2x) + " " + num(c2y) + " " +
                 num(c.x) + " " + num(c.y) + " c\n";
            px = c.x; py = c.y;
            break;
        }
        case PathCmd::Cubic:
            s += num(c.cx1) + " " + num(c.cy1) + " " + num(c.cx2) + " " + num(c.cy2) +
                 " " + num(c.x) + " " + num(c.y) + " c\n";
            px = c.x; py = c.y;
            break;
        case PathCmd::Close: s += "h\n"; break;
        }
    }
}

/* RGB, device or tagged (the page's /CS0, an sRGB ICCBased space), plus
 * an ExtGState when the color is translucent. `alphas` records thousandths
 * of alpha; the objects are created once content is known. */
std::string emit_color(const Color &c, bool stroke, std::map<int, float> &alphas,
                       bool tagged = false) {
    const std::string rgb = num(c.r) + " " + num(c.g) + " " + num(c.b);
    std::string s = tagged ? (stroke ? "/CS0 CS " + rgb + " SC\n" : "/CS0 cs " + rgb + " sc\n")
                           : rgb + (stroke ? " RG\n" : " rg\n");
    if (c.a < 0.9995f) {
        int key = (int) std::lround((double) c.a * 1000.0);
        if (key < 0) key = 0;
        if (key > 1000) key = 1000;
        alphas[key] = key / 1000.f;
        s += "/A" + std::to_string(key) + " gs\n";
    }
    return s;
}

void emit_cm(std::string &s, const Transform &t) {
    s += "q " + num(t.a) + " " + num(t.b) + " " + num(t.c) + " " + num(t.d) + " " +
         num(t.e) + " " + num(t.f) + " cm\n";
}

/* A glyph run as real text: one BT/ET, each glyph at its composed position
 * (Tm carries the horizontal scale and the y-flip that keeps glyphs
 * upright under the page's y-down CTM). */
void emit_glyphs(std::string &s, const GlyphRun &r, const Color &color, int res,
                 std::map<int, float> &alphas, bool tagged) {
    s += emit_color(color, false, alphas, tagged);
    s += "BT /F" + std::to_string(res) + " " + num(r.size) + " Tf\n";
    const float hs = r.hscale * 0.01f;
    for (const PlacedGlyph &g : r.glyphs) {
        if (g.flags & PlacedGlyph::Invisible)
            continue;
        char code[8];
        std::snprintf(code, sizeof code, "%04X", g.gid);
        s += num(hs) + " 0 0 -1 " + num(g.x) + " " + num(g.y) + " Tm <" + code + "> Tj\n";
    }
    s += "ET\n";
    std::vector<RuleSpan> rules;
    glyph_run_rules(r, rules);
    for (const RuleSpan &rule : rules) {
        s += emit_color(color, true, alphas, tagged);
        s += num(rule.thickness) + " w\n";
        s += num(rule.x0) + " " + num(rule.y) + " m " + num(rule.x1) + " " + num(rule.y) + " l S\n";
    }
}

/* The color space for RGB samples: the picture's own profile when it has
 * one (a phone's Display P3), so the viewer maps the gamut; otherwise
 * DeviceRGB, which viewers take as sRGB. */
std::string rgb_space(Writer &w, const std::vector<uint8_t> &icc, bool tagged = false) {
    if (!icc_is_rgb(icc) && !tagged)
        return "/DeviceRGB";
    const std::vector<uint8_t> &use = icc_is_rgb(icc) ? icc : icc_srgb();
    const std::string bytes(use.begin(), use.end());
    auto it = w.icc_objects.find(bytes);
    if (it == w.icc_objects.end())
        it = w.icc_objects.emplace(bytes, w.add_stream("/N 3 /Alternate /DeviceRGB", bytes)).first;
    return "[/ICCBased " + std::to_string(it->second) + " 0 R]";
}

/* HEIC and AVIF photos go into the PDF as JPEG: a viewer can't read the
 * originals, and lossless samples of a 12-megapixel photo run to tens of
 * megabytes. */
bool photo_format(const std::string &format) {
    return format == "heic" || format == "heif" || format == "avif";
}

/* How one picture is written: its pixel size (0: as stored), compression
 * and color handling, from PdfSettings and how large it is placed. */
struct PictureOpts {
    int width = 0, height = 0;           // resample to this (upright pixels)
    PdfSettings::Compression compression = PdfSettings::Compression::Automatic;
    int quality = 92;
    bool tagged = false;
    bool interpolate = true;             // smoothing when scaled up (not in PDF/X)
};

struct PdfPicture {
    int obj = 0;
    bool upright = false;               // pixels already oriented (not a raw JPEG)
    bool soft_mask = false;             // transparent pixels (an /SMask)
};

/* One XObject for the picture's source. A JPEG that isn't resampled keeps
 * its DCT stream and its stored (unrotated) pixel size; the page stream
 * turns it upright. Everything else is decoded upright, resampled when
 * asked, and written as JPEG (photos, or everything opaque with Jpeg) or
 * Flate. Still the source samples, never the screen proxy. 0 when there
 * is nothing to embed. */
PdfPicture embed_picture(Writer &w, const ImageStore::Entry &e, const PictureOpts &opt) {
    PdfPicture out;
    if (!e.loaded())
        return out;                      // not read (the caller loads before export)
    const ImageMetadata &m = e.meta;
    const uint8_t *src = e.data();
    const bool jpeg = m.format == "jpeg" ||
                      (e.size() >= 3 && src[0] == 0xFF && src[1] == 0xD8 && src[2] == 0xFF);
    const bool resample = opt.width > 0 && opt.height > 0 &&
                          (opt.width < m.width_px || opt.height < m.height_px);
    const bool gray_jpeg = m.model == ColorModel::Gray || m.channels == 1;
    const bool cmyk_jpeg = m.model == ColorModel::CMYK || m.channels == 4;
    /* Tagged color has no place for a device gray or CMYK JPEG: decode it */
    if (jpeg && !resample && m.stored_width_px > 0 && m.stored_height_px > 0 &&
        !(opt.tagged && (gray_jpeg || cmyk_jpeg))) {
        const std::string cs = gray_jpeg ? "/DeviceGray" : cmyk_jpeg ? "/DeviceCMYK"
                             : rgb_space(w, m.icc, opt.tagged);
        int bits = m.bit_depth == 12 ? 12 : 8;
        std::string dict = "/Type /XObject /Subtype /Image /Width " +
                           std::to_string(m.stored_width_px) + " /Height " +
                           std::to_string(m.stored_height_px) + " /ColorSpace " + cs +
                           " /BitsPerComponent " + std::to_string(bits) +
                           std::string(opt.interpolate ? " /Interpolate true" : "") + " /Filter /DCTDecode";
        out.obj = w.add_raw_stream(dict, std::string((const char *) e.data(), e.size()));
        out.upright = false;
        return out;
    }

    DecodedImage full;
    std::string err;
    if (!image_loader().decode(e.data(), e.size(), &full, &err) ||
        full.width <= 0 || full.height <= 0)
        return out;
    if (resample) {
        std::vector<uint8_t> small;
        const int dw = std::clamp(opt.width, 1, full.width);
        const int dh = std::clamp(opt.height, 1, full.height);
        resample_rgba(full.rgba.data(), full.width, full.height, dw, dh, small);
        full.rgba = std::move(small);
        full.width = dw;
        full.height = dh;
    }
    const size_t n = (size_t) full.width * (size_t) full.height;
    bool alpha = false;
    for (size_t i = 0; i < n; ++i)
        if (full.rgba[i * 4 + 3] != 255) { alpha = true; break; }
    const bool gray = full.meta.model == ColorModel::Gray && !alpha && !opt.tagged;

    const bool as_jpeg = !alpha &&
        (opt.compression == PdfSettings::Compression::Jpeg ||
         (opt.compression == PdfSettings::Compression::Automatic &&
          (jpeg || photo_format(full.meta.format))));
    if (as_jpeg) {
        std::string data;
        if (encode_jpeg(full.rgba.data(), full.width, full.height, opt.quality, &data)) {
            out.obj = w.add_raw_stream(
                "/Type /XObject /Subtype /Image /Width " + std::to_string(full.width) +
                    " /Height " + std::to_string(full.height) + " /ColorSpace " +
                    rgb_space(w, full.meta.icc, opt.tagged) +
                    " /BitsPerComponent 8 /Interpolate true /Filter /DCTDecode",
                data);
            out.upright = true;
            return out;
        }
    }
    std::string samples(n * (gray ? 1 : 3), '\0');
    std::string mask;
    if (alpha)
        mask.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *p = full.rgba.data() + i * 4;
        if (gray) {
            samples[i] = (char) p[0];
        } else {
            samples[i * 3] = (char) p[0];
            samples[i * 3 + 1] = (char) p[1];
            samples[i * 3 + 2] = (char) p[2];
        }
        if (alpha)
            mask[i] = (char) p[3];
    }
    std::string dict = "/Type /XObject /Subtype /Image /Width " + std::to_string(full.width) +
                       " /Height " + std::to_string(full.height) + " /ColorSpace " +
                       (gray ? std::string("/DeviceGray") : rgb_space(w, full.meta.icc, opt.tagged)) +
                       " /BitsPerComponent 8" + std::string(opt.interpolate ? " /Interpolate true" : "");
    if (alpha) {
        int sm = w.add_stream("/Type /XObject /Subtype /Image /Width " + std::to_string(full.width) +
                              " /Height " + std::to_string(full.height) +
                              " /ColorSpace /DeviceGray /BitsPerComponent 8", mask);
        dict += " /SMask " + std::to_string(sm) + " 0 R";
    }
    out.obj = w.add_stream(dict, samples);
    out.upright = true;
    out.soft_mask = alpha;
    return out;
}

void emit_missing_image(std::string &s, const DrawImage &im, bool tagged) {
    s += std::string(tagged ? "/CS0 cs 0.9 0.9 0.9 sc" : "0.9 0.9 0.9 rg") + "\n0 0 " +
         num(im.clip_w) + " " + num(im.clip_h) + " re\nf\n";
    s += std::string(tagged ? "/CS0 CS 0.6 0.25 0.25 SC" : "0.6 0.25 0.25 RG") + "\n1 w\n";
    s += "0 0 m " + num(im.clip_w) + " " + num(im.clip_h) + " l S\n";
    s += num(im.clip_w) + " 0 m 0 " + num(im.clip_h) + " l S\n";
}

/* ---- Export settings ------------------------------------------------------ */

/* The pages the settings export, by index. */
std::vector<size_t> export_pages(const PageDoc &doc, const PdfSettings &o) {
    std::vector<size_t> out;
    const int n = (int) doc.pages.size();
    const int first = o.all_pages ? 1 : std::clamp(o.first_page, 1, std::max(1, n));
    const int last = o.all_pages ? n : std::clamp(o.last_page, first, std::max(1, n));
    for (int i = first; i <= last && i <= n; ++i)
        if (!doc.pages[(size_t) i - 1].hidden || o.include_hidden)
            out.push_back((size_t) i - 1);
    return out;
}

/* How fine each picture is where it is placed largest, in ppi, on the
 * exported pages; `where` gets a page it is on (1-based). */
std::map<uint32_t, float> placed_ppi(const PageDoc &doc, const std::vector<size_t> &pages,
                                     std::map<uint32_t, size_t> *where = nullptr) {
    std::map<uint32_t, float> out;
    for (size_t pi : pages)
        for (const Item &it : doc.pages[pi].items) {
            const PlacedImage *im = it.image();
            if (!im || !doc.item_visible(it))
                continue;
            const ImageAsset *a = doc.find_image(im->asset);
            if (!a || a->meta.width_px <= 0 || a->meta.height_px <= 0)
                continue;
            const float pw = std::fabs(im->w) * std::hypot(it.xf.a, it.xf.b);
            const float ph = std::fabs(im->h) * std::hypot(it.xf.c, it.xf.d);
            if (pw <= 0.f || ph <= 0.f)
                continue;
            const float ppi = std::min((float) a->meta.width_px / (pw / 72.f),
                                       (float) a->meta.height_px / (ph / 72.f));
            auto f = out.find(im->asset);
            if (f == out.end() || ppi < f->second) {
                out[im->asset] = ppi;
                if (where)
                    (*where)[im->asset] = pi + 1;
            }
        }
    return out;
}

/* A PDF text string: literal when plain ASCII, UTF-16BE otherwise. */
std::string pdf_text(const std::string &utf8) {
    bool ascii = true;
    for (unsigned char c : utf8)
        ascii = ascii && c >= 32 && c < 127;
    if (!ascii)
        return "<FEFF" + utf16_hex(utf8) + ">";
    std::string s = "(";
    for (char c : utf8) {
        if (c == '(' || c == ')' || c == '\\')
            s += '\\';
        s += c;
    }
    return s + ")";
}

std::string xml_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

/* The output intent's profile: a file, or the built-in sRGB. */
bool read_profile(const std::string &path, std::vector<uint8_t> *icc) {
    if (path.empty()) {
        *icc = icc_srgb();
        return true;
    }
    std::string err;
    return read_file(path, icc, &err) && icc_info(*icc, nullptr, nullptr);
}

/* The characterization a profile stands for, as the registry names it
 * ("FOGRA39" for FOGRA39L_coated.icc), else its description. */
std::string output_condition(const std::string &path, const std::string &description) {
    std::string stem = path.substr(path.find_last_of('/') + 1);
    stem = stem.substr(0, stem.find_last_of('.'));
    if (stem.rfind("FOGRA", 0) == 0) {
        size_t i = 5;
        while (i < stem.size() && std::isdigit((unsigned char) stem[i]))
            ++i;
        return stem.substr(0, i);
    }
    static const std::pair<const char *, const char *> known[] = {
        {"GRACoL_TR006", "CGATS TR 006"}, {"SWOP_TR005", "CGATS TR 005"},
        {"SWOP_TR003", "CGATS TR 003"}, {"SNAP_TR002", "CGATS TR 002"},
    };
    for (const auto &k : known)
        if (stem.rfind(k.first, 0) == 0)
            return k.second;
    return description.empty() ? stem : description;
}

/* Now, as a PDF date and as XMP's ISO 8601 */
void dates(std::string *pdf, std::string *xmp) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "D:%Y%m%d%H%M%S+00'00'", &tm);
    *pdf = buf;
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    *xmp = buf;
}

std::string hex64(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llX", (unsigned long long) v);
    return buf;
}

/* PDF/X-4's identification: what the file is and where it came from */
std::string xmp_packet(const PdfSettings &o, const std::string &date, const std::string &uuid) {
    std::string x =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "<rdf:Description rdf:about=\"\"\n"
        "  xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
        "  xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
        "  xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\"\n"
        "  xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\"\n"
        "  xmlns:pdfxid=\"http://www.npes.org/pdfx/ns/id/\">\n"
        "<dc:format>application/pdf</dc:format>\n";
    if (!o.title.empty())
        x += "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" + xml_escape(o.title) +
             "</rdf:li></rdf:Alt></dc:title>\n";
    if (!o.author.empty())
        x += "<dc:creator><rdf:Seq><rdf:li>" + xml_escape(o.author) + "</rdf:li></rdf:Seq></dc:creator>\n";
    if (!o.subject.empty())
        x += "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">" + xml_escape(o.subject) +
             "</rdf:li></rdf:Alt></dc:description>\n";
    x += "<xmp:CreateDate>" + date + "</xmp:CreateDate>\n"
         "<xmp:ModifyDate>" + date + "</xmp:ModifyDate>\n"
         "<xmp:MetadataDate>" + date + "</xmp:MetadataDate>\n"
         "<xmp:CreatorTool>pagemade</xmp:CreatorTool>\n"
         "<pdf:Producer>pagemade</pdf:Producer>\n"
         "<pdf:Trapped>False</pdf:Trapped>\n"
         "<xmpMM:DocumentID>uuid:" + uuid + "</xmpMM:DocumentID>\n"
         "<xmpMM:InstanceID>uuid:" + uuid + "</xmpMM:InstanceID>\n"
         "<xmpMM:VersionID>1</xmpMM:VersionID>\n"
         "<xmpMM:RenditionClass>default</xmpMM:RenditionClass>\n"
         "<pdfxid:GTS_PDFXVersion>PDF/X-4</pdfxid:GTS_PDFXVersion>\n"
         "</rdf:Description>\n</rdf:RDF>\n</x:xmpmeta>\n";
    x += std::string(20, ' ') + "\n<?xpacket end=\"w\"?>";
    return x;
}

/* Crop marks at the trim's corners, outside the bleed (PDF space, y up) */
std::string crop_marks(float off, float w, float h, float bleed, bool tagged) {
    const float gap = bleed + 3.f, len = 15.f;
    std::string s = std::string("q 0.25 w ") + (tagged ? "/CS0 CS 0 0 0 SC" : "0 0 0 RG") + "\n";
    auto line = [&](float x0, float y0, float x1, float y1) {
        s += num(x0) + " " + num(y0) + " m " + num(x1) + " " + num(y1) + " l\n";
    };
    for (float x : {off, off + w})
        for (float y : {off, off + h}) {
            const float dx = x == off ? -1.f : 1.f, dy = y == off ? -1.f : 1.f;
            line(x + dx * gap, y, x + dx * (gap + len), y);      // horizontal
            line(x, y + dy * gap, x, y + dy * (gap + len));      // vertical
        }
    return s + "S Q\n";
}

} // namespace

std::vector<PdfOutputProfile> pdf_output_profiles() {
    std::vector<PdfOutputProfile> out;
    out.push_back({"", "sRGB (built in)", "sRGB IEC61966-2.1", false});
    std::vector<std::string> dirs = {"/usr/share/color/icc", "/usr/local/share/color/icc"};
    if (const char *home = std::getenv("HOME")) {
        dirs.push_back(std::string(home) + "/.local/share/color/icc");
        dirs.push_back(std::string(home) + "/.color/icc");
    }
    std::vector<PdfOutputProfile> found;
    for (const std::string &dir : dirs) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const std::filesystem::path p = it->path();
            std::string ext = p.extension().string();
            for (char &c : ext)
                c = (char) std::tolower((unsigned char) c);
            if (ext != ".icc" && ext != ".icm")
                continue;
            /* Printing profiles only: output class, CMYK */
            std::vector<uint8_t> head(128);
            if (FILE *f = std::fopen(p.c_str(), "rb")) {
                head.resize(std::fread(head.data(), 1, head.size(), f));
                std::fclose(f);
            }
            if (head.size() < 128 || std::memcmp(head.data() + 12, "prtr", 4) != 0 ||
                std::memcmp(head.data() + 16, "CMYK", 4) != 0)
                continue;
            std::vector<uint8_t> icc;
            std::string err, space, desc;
            if (!read_file(p.string(), &icc, &err) || !icc_info(icc, &space, &desc))
                continue;
            const std::string cond = output_condition(p.string(), desc);
            found.push_back({p.string(), (desc.empty() ? p.stem().string() : desc) + " (" + cond + ")",
                             cond, true});
        }
    }
    std::sort(found.begin(), found.end(),
              [](const PdfOutputProfile &a, const PdfOutputProfile &b) { return a.name < b.name; });
    out.insert(out.end(), found.begin(), found.end());
    return out;
}

std::vector<std::string> pdf_preflight(const PageDoc &doc, const std::vector<Composition> &comps,
                                       const ImageStore *images, const PdfSettings &o) {
    std::vector<std::string> out;
    const std::vector<size_t> pages = export_pages(doc, o);
    if (pages.empty())
        out.push_back("No pages to export: every chosen page is hidden.");
    else if (pages.size() % 2 == 1 && pages.size() > 1)
        out.push_back(std::to_string(pages.size()) +
                      " pages: book printers usually need an even page count.");
    if (o.color == PdfSettings::Color::Cmyk)
        out.push_back("CMYK conversion isn't available yet; colors are written as tagged RGB.");

    /* Pictures: too coarse where placed, or missing */
    std::map<uint32_t, size_t> where;
    const std::map<uint32_t, float> ppi = placed_ppi(doc, pages, &where);
    for (const auto &kv : ppi) {
        const ImageAsset *a = doc.find_image(kv.first);
        const std::string name = a && !a->name.empty() ? a->name : "a picture";
        if (kv.second + 0.5f < o.min_ppi) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.0f", (double) kv.second);
            out.push_back(name + " is " + buf + " ppi as placed on page " +
                          std::to_string(where[kv.first]) + " (below " +
                          std::to_string((int) o.min_ppi) + "): it may print soft.");
        }
        ImageStore::Bytes bytes;
        ImageMetadata meta;
        if (images && !images->export_source(kv.first, &bytes, &meta))
            out.push_back(name + " (page " + std::to_string(where[kv.first]) +
                          ") can't be read: it prints as a gray box.");
    }

    /* Transparency, and text that doesn't fit */
    std::set<size_t> transparent;
    for (size_t pi : pages)
        for (const Item &it : doc.pages[pi].items) {
            if (!doc.item_visible(it))
                continue;
            if (const Shape *sh = it.shape()) {
                const Color f = doc.resolve(sh->fill), st = doc.resolve(sh->stroke.paint);
                if ((!sh->fill.none() && f.a < 0.999f) || (!sh->stroke.none() && st.a < 0.999f))
                    transparent.insert(pi + 1);
            }
            if (const PlacedImage *im = it.image())
                if (const ImageAsset *a = doc.find_image(im->asset))
                    if (a->meta.alpha)
                        transparent.insert(pi + 1);
        }
    if (!transparent.empty()) {
        std::string list;
        for (size_t p : transparent)
            list += (list.empty() ? "" : ", ") + std::to_string(p);
        out.push_back("Transparency on page " + list +
                      ": some printers (Prodigi among them) ask for it to be flattened.");
    }
    for (size_t si = 0; si < comps.size() && si < doc.stories.size(); ++si)
        if (comps[si].overset) {
            size_t pg = 0;
            const StoryEntry &se = doc.stories[si];
            if (!se.thread.empty())
                doc.find_item(se.thread.back(), &pg);
            out.push_back("Text doesn't fit its blocks (overset) on page " +
                          std::to_string(pg + 1) + ".");
        }
    for (const Layer &l : doc.layers)
        if (!l.visible)
            out.push_back("Layer \"" + l.name + "\" is hidden and won't print.");
    if (o.pdfx4 && !o.output_intent.empty()) {
        std::vector<uint8_t> icc;
        if (!read_profile(o.output_intent, &icc))
            out.push_back("The output intent profile can't be read: " + o.output_intent);
    }
    return out;
}

bool export_pdf(const std::string &path, const PageDoc &doc,
                const std::vector<Composition> &comps, const ImageStore *images,
                const PdfSettings *settings) {
    const PdfSettings &o = settings ? *settings : doc.pdf;
    /* PDF/X and tagged RGB tag every color; CMYK is not done yet and is
     * written as tagged RGB */
    const bool tagged = o.pdfx4 || o.color != PdfSettings::Color::Rgb;
    const std::vector<size_t> chosen = export_pages(doc, o);

    /* Draw lists for the chosen pages, and the fonts they use. */
    std::vector<DrawList> pages;
    for (size_t i : chosen)
        pages.push_back(build_page(doc, i, comps));
    if (pages.empty())
        pages.emplace_back();   // a PDF needs a page; a fully hidden document is blank

    std::map<const Font *, PdfFont> fonts;
    collect_unicodes(doc, comps, fonts, std::set<size_t>(chosen.begin(), chosen.end()));

    Writer w;
    std::map<const Font *, int> font_res;
    int tag_seq = 0;
    for (auto &kv : fonts) {
        int res = 0;
        if (!embed_font(w, kv.second, res, tag_seq))
            return false;
        font_res[kv.first] = res;
    }

    /* One XObject per picture, from the source bytes, at the resolution
     * its largest placement needs. A missing file is a gray stand-in on
     * that placement; it does not fail the export. */
    std::map<uint32_t, PdfPicture> image_res;
    const std::map<uint32_t, float> ppi = placed_ppi(doc, chosen);
    bool soft_masks = false;
    if (images) {
        for (const DrawList &list : pages)
            for (const DrawOp &op : list)
                if (const auto *im = std::get_if<DrawImage>(&op))
                    if (im->asset && !image_res.count(im->asset)) {
                        ImageStore::Bytes bytes;
                        ImageMetadata meta;
                        if (!images->export_source(im->asset, &bytes, &meta))
                            continue;
                        PictureOpts po;
                        po.compression = o.compression;
                        po.quality = std::clamp(o.jpeg_quality, 10, 100);
                        po.tagged = tagged;
                        po.interpolate = !o.pdfx4;
                        auto f = ppi.find(im->asset);
                        if (o.downsample && f != ppi.end() && f->second > o.downsample_above &&
                            o.downsample_to > 0.f) {
                            const float k = o.downsample_to / f->second;
                            po.width = std::max(1, (int) std::lround(meta.width_px * k));
                            po.height = std::max(1, (int) std::lround(meta.height_px * k));
                        }
                        ImageStore::Entry held;
                        held.source = std::move(bytes);
                        held.meta = std::move(meta);
                        image_res[im->asset] = embed_picture(w, held, po);
                        soft_masks = soft_masks || image_res[im->asset].soft_mask;
                    }
    }

    /* The sheet: the trim, the bleed past it, and room for crop marks */
    const float W = doc.setup.width, H = doc.setup.height;
    const float bleed = o.bleed ? std::max(0.f, doc.setup.bleed) : 0.f;
    const float off = o.crop_marks ? bleed + 3.f + 15.f + 3.f : bleed;

    /* Page streams first, so translucent colors can name ExtGState objects
     * before the resource dict is written into each page. */
    std::map<int, float> alphas;
    std::vector<std::string> contents_src;
    contents_src.reserve(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        /* Clipped to the bleed (nothing past it reaches the slug), then
         * the page's y-down space with the trim's corner at the origin */
        std::string content = "q " + num(off - bleed) + " " + num(off - bleed) + " " +
                              num(W + 2 * bleed) + " " + num(H + 2 * bleed) + " re W n\n";
        content += "1 0 0 -1 " + num(off) + " " + num(H + off) + " cm\n";
        for (const DrawOp &op : pages[i]) {
            if (const auto *g = std::get_if<DrawGlyphs>(&op)) {
                auto it = g->run->font ? font_res.find(g->run->font) : font_res.end();
                if (it == font_res.end())
                    continue;
                emit_cm(content, g->xf);
                emit_glyphs(content, *g->run, g->color, it->second, alphas, tagged);
                content += "Q\n";
            } else if (const auto *f = std::get_if<DrawFill>(&op)) {
                emit_cm(content, f->xf);
                content += emit_color(f->color, false, alphas, tagged);
                emit_path(content, f->path);
                content += "f\nQ\n";
            } else if (const auto *s = std::get_if<DrawStroke>(&op)) {
                emit_cm(content, s->xf);
                content += emit_color(s->color, true, alphas, tagged);
                content += num(s->width) + " w\n";
                if (!s->dash.empty()) {
                    content += "[";
                    for (float d : s->dash)
                        content += num(d) + " ";
                    content += "] 0 d\n";
                }
                content += std::to_string(s->cap == LineCap::Round ? 1
                                           : s->cap == LineCap::Square ? 2 : 0) + " J\n";
                content += std::to_string(s->join == LineJoin::Round ? 1
                                           : s->join == LineJoin::Bevel ? 2 : 0) + " j\n";
                emit_path(content, s->path);
                content += "S\nQ\n";
            } else if (const auto *im = std::get_if<DrawImage>(&op)) {
                if (!(im->clip_w > 0.f) || !(im->clip_h > 0.f))
                    continue;
                emit_cm(content, im->xf);
                auto found = image_res.find(im->asset);
                const bool have = found != image_res.end() && found->second.obj &&
                                  im->w != 0.f && im->h != 0.f;
                if (!have) {
                    emit_missing_image(content, *im, tagged);
                } else {
                    /* The frame clips in item space; the picture's own matrix
                     * then maps the unit square onto the placement rect. */
                    content += "0 0 " + num(im->clip_w) + " " + num(im->clip_h) + " re W n\n";
                    int orient = 1;
                    if (!found->second.upright && images)
                        if (const ImageStore::Entry *e = images->find(im->asset))
                            orient = e->meta.orientation >= 1 ? e->meta.orientation : 1;
                    float m[6];
                    image_pdf_matrix(orient, im->x, im->y, im->w, im->h, m);
                    content += num(m[0]) + " " + num(m[1]) + " " + num(m[2]) + " " +
                               num(m[3]) + " " + num(m[4]) + " " + num(m[5]) + " cm\n";
                    content += "/Im" + std::to_string(im->asset) + " Do\n";
                }
                content += "Q\n";
            }
        }
        content += "Q\n";
        if (o.crop_marks)
            content += crop_marks(off, W, H, bleed, tagged);
        contents_src.push_back(std::move(content));
    }

    std::map<int, int> alpha_objs;
    for (const auto &kv : alphas)
        alpha_objs[kv.first] = w.add("<< /Type /ExtGState /ca " + num(kv.second) +
                                     " /CA " + num(kv.second) + " >>");

    std::string srgb_space;
    if (tagged) {
        const std::vector<uint8_t> &icc = icc_srgb();
        srgb_space = rgb_space(w, std::vector<uint8_t>(icc.begin(), icc.end()), true);
    }

    std::string res_dict = "<< /Font <<";
    for (const auto &kv : font_res)
        res_dict += " /F" + std::to_string(kv.second) + " " + std::to_string(kv.second) +
                    " 0 R";
    res_dict += " >>";
    bool any_image = false;
    for (const auto &kv : image_res)
        any_image |= kv.second.obj != 0;
    if (any_image) {
        res_dict += " /XObject <<";
        for (const auto &kv : image_res)
            if (kv.second.obj)
                res_dict += " /Im" + std::to_string(kv.first) + " " +
                            std::to_string(kv.second.obj) + " 0 R";
        res_dict += " >>";
    }
    if (!alpha_objs.empty()) {
        res_dict += " /ExtGState <<";
        for (const auto &kv : alpha_objs)
            res_dict += " /A" + std::to_string(kv.first) + " " + std::to_string(kv.second) +
                        " 0 R";
        res_dict += " >>";
    }
    if (tagged)
        res_dict += " /ColorSpace << /CS0 " + srgb_space + " >>";
    res_dict += " >>";

    /* Trim and bleed boxes whenever the sheet is larger than the trim, and
     * always for PDF/X; a transparency group in sRGB for PDF/X */
    std::string boxes = "/MediaBox [0 0 " + num(W + 2 * off) + " " + num(H + 2 * off) + "]";
    if (off > 0.f || o.pdfx4)
        boxes += " /TrimBox [" + num(off) + " " + num(off) + " " + num(off + W) + " " +
                 num(off + H) + "] /BleedBox [" + num(off - bleed) + " " + num(off - bleed) +
                 " " + num(off + W + bleed) + " " + num(off + H + bleed) + "]";
    if (o.pdfx4 && (!alphas.empty() || soft_masks))
        boxes += " /Group << /Type /Group /S /Transparency /CS " + srgb_space + " >>";

    std::vector<int> page_objs;
    for (const std::string &content : contents_src) {
        int contents = w.add_stream("", content);
        page_objs.push_back(w.add("<< /Type /Page /Parent 0 0 R " + boxes +
                                  " /Resources " + res_dict + " /Contents " +
                                  std::to_string(contents) + " 0 R >>"));
    }

    std::string kids;
    for (int p : page_objs)
        kids += " " + std::to_string(p) + " 0 R";
    int pages_obj = w.add("<< /Type /Pages /Kids [" + kids + " ] /Count " +
                          std::to_string(page_objs.size()) + " >>");
    /* /Parent references resolve now that the Pages object exists. */
    for (int p : page_objs) {
        std::string &body = w.objects[(size_t) p - 1];
        size_t at = body.find("/Parent 0 0 R");
        if (at != std::string::npos)
            body.replace(at, 13, "/Parent " + std::to_string(pages_obj) + " 0 R");
    }

    /* Document information, identification and (PDF/X) the output intent */
    std::string pdf_date, xmp_date;
    dates(&pdf_date, &xmp_date);
    const uint64_t h1 = image_bytes_hash((const uint8_t *) path.data(), path.size()) ^
                        (uint64_t) std::time(nullptr);
    const uint64_t h2 = image_bytes_hash((const uint8_t *) pdf_date.data(), pdf_date.size()) ^
                        (uint64_t) page_objs.size();
    const std::string id = hex64(h1) + hex64(h2);
    std::string catalog_extra, trailer = " /ID [<" + id + "> <" + id + ">]";
    if (o.pdfx4 || !o.title.empty() || !o.author.empty() || !o.subject.empty()) {
        std::string info = "<< /Creator (pagemade) /Producer (pagemade) /CreationDate (" +
                           pdf_date + ") /ModDate (" + pdf_date + ")";
        if (!o.title.empty()) info += " /Title " + pdf_text(o.title);
        if (!o.author.empty()) info += " /Author " + pdf_text(o.author);
        if (!o.subject.empty()) info += " /Subject " + pdf_text(o.subject);
        if (o.pdfx4) info += " /Trapped /False /GTS_PDFXVersion (PDF/X-4)";
        info += " >>";
        trailer += " /Info " + std::to_string(w.add(info)) + " 0 R";
    }
    if (o.pdfx4) {
        std::vector<uint8_t> icc;
        if (!read_profile(o.output_intent, &icc))
            read_profile("", &icc);      // unreadable: sRGB rather than no intent
        std::string space, desc;
        icc_info(icc, &space, &desc);
        const int n = space == "CMYK" ? 4 : space == "GRAY" ? 1 : 3;
        const int prof = w.add_stream("/N " + std::to_string(n),
                                      std::string(icc.begin(), icc.end()));
        const std::string cond = o.output_intent.empty() ? "sRGB IEC61966-2.1"
                                 : output_condition(o.output_intent, desc);
        const int intent = w.add("<< /Type /OutputIntent /S /GTS_PDFX /OutputConditionIdentifier " +
                                 pdf_text(cond) + " /OutputCondition " + pdf_text(desc.empty() ? cond : desc) +
                                 " /RegistryName (http://www.color.org) /Info " +
                                 pdf_text(desc.empty() ? cond : desc) + " /DestOutputProfile " +
                                 std::to_string(prof) + " 0 R >>");
        const std::string uuid = id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) +
                                 "-" + id.substr(16, 4) + "-" + id.substr(20, 12);
        const std::string xmp = xmp_packet(o, xmp_date, uuid);
        const int meta = w.add_raw_stream("/Type /Metadata /Subtype /XML", xmp);
        catalog_extra = " /OutputIntents [" + std::to_string(intent) + " 0 R] /Metadata " +
                        std::to_string(meta) + " 0 R";
    }
    int catalog = w.add("<< /Type /Catalog /Pages " + std::to_string(pages_obj) + " 0 R" +
                        catalog_extra + " >>");
    return w.write(path, catalog, o.pdfx4 ? "1.6" : "1.7", trailer);
}

} // namespace pagemade
