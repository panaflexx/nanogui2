/*
 * pagemade/pdf.cpp — see pdf.h.
 */
#include "pdf.h"

#include "composer/font.h"
#include "drawlist.h"
#include "image.h"
#include "page.h"

#include <hb.h>
#include <hb-subset.h>
#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
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

/* UTF-8 -> big-endian UTF-16 hex for ToUnicode (BMP only here; anything
 * astral would need a surrogate pair). */
std::string utf16_hex(const std::string &utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char) utf8[i];
        int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        uint32_t cp = c & (0x7F >> n);
        for (int k = 1; k <= n && i + k < utf8.size(); ++k)
            cp = (cp << 6) | ((unsigned char) utf8[i + k] & 0x3F);
        if (cp > 0xFFFF) {   // surrogate pair
            cp -= 0x10000;
            out += hex16(0xD800 + (cp >> 10)) + hex16(0xDC00 + (cp & 0x3FF));
        } else {
            out += hex16(cp);
        }
        i += (size_t) n + 1;
    }
    return out;
}

std::string flate(const std::string &in) {
    mz_ulong bound = mz_compressBound((mz_ulong) in.size());
    std::string out(bound, '\0');
    mz_ulong have = bound;
    if (mz_compress2((unsigned char *) out.data(), &have,
                     (const unsigned char *) in.data(), (mz_ulong) in.size(), 6) != MZ_OK)
        return {};
    out.resize(have);
    return out;
}

size_t char_len(const std::string &s, size_t i) {
    unsigned char c = (unsigned char) s[i];
    return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
}

/* ---- The writer ------------------------------------------------------------
 *
 * Objects are collected as strings; byte offsets and the xref table are
 * computed when the file is assembled.
 */
struct Writer {
    std::vector<std::string> objects;

    int add(std::string body) {
        objects.push_back(std::move(body));
        return (int) objects.size();
    }
    /* A stream object, Flate-compressed. */
    int add_stream(const std::string &dict, const std::string &data) {
        std::string z = flate(data);
        return add("<< " + dict + " /Length " + std::to_string(z.size()) +
                   " /Filter /FlateDecode >>\nstream\n" + z + "\nendstream");
    }
    /* A stream already encoded (a JPEG's own bytes). `dict` includes /Filter. */
    int add_raw_stream(const std::string &dict, const std::string &data) {
        return add("<< " + dict + " /Length " + std::to_string(data.size()) +
                   " >>\nstream\n" + data + "\nendstream");
    }

    bool write(const std::string &path, int root) {
        std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
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
               " /Root " + std::to_string(root) + " 0 R >>\n"
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
                      std::map<const Font *, PdfFont> &fonts) {
    for (size_t si = 0; si < comps.size() && si < doc.stories.size(); ++si) {
        const Story &story = doc.stories[si].story;
        for (const ComposedLine &l : comps[si].lines) {
            if (l.para >= story.paragraphs.size())
                continue;
            /* Glyphs on a hidden page are not drawn, so they are not embedded. */
            if (l.frame < doc.stories[si].thread.size()) {
                size_t pg = 0;
                if (doc.find_item(doc.stories[si].thread[l.frame], &pg) &&
                    pg < doc.pages.size() && doc.pages[pg].hidden)
                    continue;
            }
            const std::string text = paragraph_text(story.paragraphs[l.para]);
            for (const GlyphRun &r : l.runs) {
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
                    uint32_t from = g.cluster, to = from;
                    for (size_t k = i + 1; k < r.glyphs.size(); ++k)
                        if (r.glyphs[k].cluster > from) {
                            to = r.glyphs[k].cluster;
                            break;
                        }
                    if (to <= from)
                        to = from + (uint32_t) char_len(text, from);
                    to = std::min(to, (uint32_t) text.size());
                    if (from < text.size())
                        pf.unicode[g.gid] = text.substr(from, to - from);
                }
            }
        }
    }
}

/* Subset and embed one face; returns false when hb-subset fails. */
bool embed_font(Writer &w, PdfFont &pf, int &out_res) {
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

    static int tag_seq = 0;
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

std::string emit_color(const Color &c, bool stroke) {
    return num(c.r) + " " + num(c.g) + " " + num(c.b) + (stroke ? " RG\n" : " rg\n");
}

void emit_cm(std::string &s, const Transform &t) {
    s += "q " + num(t.a) + " " + num(t.b) + " " + num(t.c) + " " + num(t.d) + " " +
         num(t.e) + " " + num(t.f) + " cm\n";
}

/* A glyph run as real text: one BT/ET, each glyph at its composed position
 * (Tm carries the horizontal scale and the y-flip that keeps glyphs
 * upright under the page's y-down CTM). */
void emit_glyphs(std::string &s, const GlyphRun &r, int res) {
    s += emit_color(r.color, false);
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
    if (!r.underline && !r.strike)
        return;
    float x0 = 0, x1 = 0, base = 0;
    bool span = false;
    for (const PlacedGlyph &g : r.glyphs) {
        if (g.flags & PlacedGlyph::Invisible)
            continue;
        const float right = g.x + std::max(0.f, g.adv);
        if (!span) { x0 = g.x; x1 = right; base = g.y; span = true; }
        else { x0 = std::min(x0, g.x); x1 = std::max(x1, right); }
    }
    if (!span || !(x1 > x0) || !r.font)
        return;
    const float sy = r.size / (float) r.font->units_per_em();
    auto rule = [&](float design_y, float design_thick) {
        const float y = base - design_y * sy;
        const float thick = std::max(0.25f, design_thick * sy);
        s += emit_color(r.color, true);
        s += num(thick) + " w\n";
        s += num(x0) + " " + num(y) + " m " + num(x1) + " " + num(y) + " l S\n";
    };
    if (r.underline)
        rule(r.font->underline_position(), r.font->underline_thickness());
    if (r.strike)
        rule(r.font->strike_position(), r.font->strike_thickness());
}

/* One XObject for the picture's source. JPEG keeps its DCT stream and its
 * stored (unrotated) pixel size; the page stream turns it upright. Anything
 * else is decoded upright at full resolution and Flate-encoded — still the
 * source samples, not the screen proxy. 0 when there is nothing to embed. */
struct PdfPicture {
    int obj = 0;
    bool upright = false;               // pixels already oriented (not a raw JPEG)
};

PdfPicture embed_picture(Writer &w, const ImageStore::Entry &e) {
    PdfPicture out;
    const ImageMetadata &m = e.meta;
    const bool jpeg = m.format == "jpeg" ||
                      (e.source.size() >= 3 && e.source[0] == 0xFF && e.source[1] == 0xD8 &&
                       e.source[2] == 0xFF);
    if (jpeg && m.stored_width_px > 0 && m.stored_height_px > 0) {
        const char *cs = (m.model == ColorModel::Gray || m.channels == 1) ? "DeviceGray"
                       : (m.model == ColorModel::CMYK || m.channels == 4) ? "DeviceCMYK"
                       : "DeviceRGB";
        int bits = m.bit_depth == 12 ? 12 : 8;
        std::string dict = "/Type /XObject /Subtype /Image /Width " +
                           std::to_string(m.stored_width_px) + " /Height " +
                           std::to_string(m.stored_height_px) + " /ColorSpace /" + cs +
                           " /BitsPerComponent " + std::to_string(bits) +
                           " /Interpolate true /Filter /DCTDecode";
        out.obj = w.add_raw_stream(dict, std::string((const char *) e.source.data(), e.source.size()));
        out.upright = false;
        return out;
    }

    DecodedImage full;
    std::string err;
    if (!image_loader().decode(e.source.data(), e.source.size(), &full, &err) ||
        full.width <= 0 || full.height <= 0)
        return out;
    const size_t n = (size_t) full.width * (size_t) full.height;
    bool alpha = false;
    for (size_t i = 0; i < n; ++i)
        if (full.rgba[i * 4 + 3] != 255) { alpha = true; break; }
    const bool gray = full.meta.model == ColorModel::Gray && !alpha;
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
                       " /Height " + std::to_string(full.height) + " /ColorSpace /" +
                       std::string(gray ? "DeviceGray" : "DeviceRGB") +
                       " /BitsPerComponent 8 /Interpolate true";
    if (alpha) {
        int sm = w.add_stream("/Type /XObject /Subtype /Image /Width " + std::to_string(full.width) +
                              " /Height " + std::to_string(full.height) +
                              " /ColorSpace /DeviceGray /BitsPerComponent 8", mask);
        dict += " /SMask " + std::to_string(sm) + " 0 R";
    }
    out.obj = w.add_stream(dict, samples);
    out.upright = true;
    return out;
}

void emit_missing_image(std::string &s, const DrawImage &im) {
    s += "0.9 0.9 0.9 rg\n0 0 " + num(im.clip_w) + " " + num(im.clip_h) + " re\nf\n";
    s += "0.6 0.25 0.25 RG\n1 w\n";
    s += "0 0 m " + num(im.clip_w) + " " + num(im.clip_h) + " l S\n";
    s += num(im.clip_w) + " 0 m 0 " + num(im.clip_h) + " l S\n";
}

} // namespace

bool export_pdf(const std::string &path, const PageDoc &doc,
                const std::vector<Composition> &comps, const ImageStore *images) {
    /* Draw lists for every page, and the fonts they use. */
    std::vector<DrawList> pages;
    for (size_t i = 0; i < doc.pages.size(); ++i)
        if (!doc.pages[i].hidden)
            pages.push_back(build_page(doc, i, comps));
    if (pages.empty())
        pages.emplace_back();   // a PDF needs a page; a fully hidden document is blank

    std::map<const Font *, PdfFont> fonts;
    collect_unicodes(doc, comps, fonts);

    Writer w;
    std::map<const Font *, int> font_res;
    for (auto &kv : fonts) {
        int res = 0;
        if (!embed_font(w, kv.second, res))
            return false;
        font_res[kv.first] = res;
    }

    /* One XObject per picture, from the source bytes. A missing file is a
     * gray stand-in on that placement; it does not fail the export. */
    std::map<uint32_t, PdfPicture> image_res;
    if (images) {
        for (const DrawList &list : pages)
            for (const DrawOp &op : list)
                if (const auto *im = std::get_if<DrawImage>(&op))
                    if (im->asset && !image_res.count(im->asset))
                        if (const ImageStore::Entry *e = images->find(im->asset))
                            image_res[im->asset] = embed_picture(w, *e);
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
    res_dict += " >>";

    int pages_kids_placeholder = 0;   // filled once the page objects exist
    std::vector<int> page_objs;
    for (size_t i = 0; i < pages.size(); ++i) {
        std::string content = "1 0 0 -1 0 " + num(doc.setup.height) + " cm\n";
        for (const DrawOp &op : pages[i]) {
            if (const auto *g = std::get_if<DrawGlyphs>(&op)) {
                auto it = g->run->font ? font_res.find(g->run->font) : font_res.end();
                if (it == font_res.end())
                    continue;
                emit_cm(content, g->xf);
                emit_glyphs(content, *g->run, it->second);
                content += "Q\n";
            } else if (const auto *f = std::get_if<DrawFill>(&op)) {
                emit_cm(content, f->xf);
                content += emit_color(f->color, false);
                emit_path(content, f->path);
                content += "f\nQ\n";
            } else if (const auto *s = std::get_if<DrawStroke>(&op)) {
                emit_cm(content, s->xf);
                content += emit_color(s->color, true);
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
                    emit_missing_image(content, *im);
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
        int contents = w.add_stream("", content);
        page_objs.push_back(w.add("<< /Type /Page /Parent 0 0 R /MediaBox [0 0 " +
                                  num(doc.setup.width) + " " + num(doc.setup.height) +
                                  "] /Resources " + res_dict + " /Contents " +
                                  std::to_string(contents) + " 0 R >>"));
        (void) pages_kids_placeholder;
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
    int catalog = w.add("<< /Type /Catalog /Pages " + std::to_string(pages_obj) +
                        " 0 R >>");
    return w.write(path, catalog);
}

} // namespace pagemade
