/*
 * pagemade/composer/font.cpp — HarfBuzz-backed fonts and the font library.
 */
#include "composer/font.h"

#include <hb.h>
#include <hb-ot.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pagemade {

std::shared_ptr<Font> Font::load_file(const std::string &path, unsigned index) {
    hb_blob_t *blob = hb_blob_create_from_file_or_fail(path.c_str());
    if (!blob)
        return nullptr;
    auto f = from_blob(blob, index);
    if (f) {
        size_t slash = path.find_last_of("/\\");
        f->m_name = slash == std::string::npos ? path : path.substr(slash + 1);
        f->m_path = path;
    }
    return f;
}

std::shared_ptr<Font> Font::load_bytes(std::vector<char> bytes, const std::string &name,
                                       unsigned index) {
    if (bytes.empty())
        return nullptr;
    auto *owned = new std::vector<char>(std::move(bytes));
    hb_blob_t *blob = hb_blob_create_or_fail(owned->data(), (unsigned) owned->size(),
                                             HB_MEMORY_MODE_READONLY, owned,
                                             [](void *p) { delete (std::vector<char> *) p; });
    if (!blob) {
        delete owned;
        return nullptr;
    }
    auto f = from_blob(blob, index);
    if (f)
        f->m_name = name;
    return f;
}

const char *Font::data(size_t *size) const {
    unsigned n = 0;
    const char *d = hb_blob_get_data(m_blob, &n);
    if (size)
        *size = n;
    return d;
}

std::shared_ptr<Font> Font::load_memory(const void *data, size_t size, unsigned index) {
    hb_blob_t *blob = hb_blob_create_or_fail((const char *) data, (unsigned) size,
                                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    if (!blob)
        return nullptr;
    auto f = from_blob(blob, index);
    if (f)
        f->m_name = "(embedded)";
    return f;
}

std::shared_ptr<Font> Font::from_blob(hb_blob_t *blob, unsigned index) {
    hb_face_t *face = hb_face_create(blob, index);
    if (!face || hb_face_get_glyph_count(face) == 0) {
        hb_face_destroy(face);
        hb_blob_destroy(blob);
        return nullptr;
    }
    std::shared_ptr<Font> f(new Font());
    f->m_blob = blob;
    f->m_face = face;
    /* hb_font_create() uses HarfBuzz's own OpenType functions at a scale
     * of units-per-em with no ppem: advances, GPOS kerning and outlines
     * all come back unhinted, in design units. */
    f->m_font = hb_font_create(face);
    f->m_upem = (int) hb_face_get_upem(face);

    hb_position_t v;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &v))
        f->m_ascender = (float) v;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &v))
        f->m_descender = (float) v;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_CAP_HEIGHT, &v) && v > 0)
        f->m_cap_height = (float) v;
    else
        f->m_cap_height = f->m_ascender * 0.7f;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_X_HEIGHT, &v) && v > 0)
        f->m_x_height = (float) v;
    else
        f->m_x_height = f->m_ascender * 0.5f;

    f->m_ul_pos = -f->m_upem * 0.1f;
    f->m_ul_thick = std::max(1.f, f->m_upem * 0.05f);
    f->m_st_pos = f->m_x_height * 0.5f;
    f->m_st_thick = f->m_ul_thick;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &v))
        f->m_ul_pos = (float) v;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &v) && v > 0)
        f->m_ul_thick = (float) v;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &v))
        f->m_st_pos = (float) v;
    if (hb_ot_metrics_get_position(f->m_font, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &v) && v > 0)
        f->m_st_thick = (float) v;

    hb_codepoint_t gid = 0;
    if (hb_font_get_nominal_glyph(f->m_font, ' ', &gid))
        f->m_space_gid = gid;
    return f;
}

Font::~Font() {
    hb_font_destroy(m_font);
    hb_face_destroy(m_face);
    hb_blob_destroy(m_blob);
}

float Font::advance(uint32_t gid) const {
    return (float) hb_font_get_glyph_h_advance(m_font, gid);
}

float Font::small_cap_scale() const {
    if (m_cap_height > 1.f && m_x_height > 1.f) {
        float s = m_x_height / m_cap_height;
        if (s < 0.6f) s = 0.6f;
        if (s > 0.85f) s = 0.85f;
        return s;
    }
    return 0.75f;
}

bool Font::has_small_caps() const {
    if (m_smcp_known)
        return m_smcp;
    m_smcp_known = true;
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, "a", 1, 0, 1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(m_font, buf, nullptr, 0);
    unsigned n = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
    uint32_t plain = n ? info[0].codepoint : 0;
    hb_buffer_clear_contents(buf);
    hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_add_utf8(buf, "a", 1, 0, 1);
    hb_buffer_guess_segment_properties(buf);
    hb_feature_t feat = {HB_TAG('s', 'm', 'c', 'p'), 1, 0, (unsigned) -1};
    hb_shape(m_font, buf, &feat, 1);
    info = hb_buffer_get_glyph_infos(buf, &n);
    uint32_t small = n ? info[0].codepoint : 0;
    hb_buffer_destroy(buf);
    m_smcp = small != 0 && small != plain;
    return m_smcp;
}

bool Font::is_cff() const {
    hb_blob_t *t = hb_face_reference_table(m_face, HB_TAG('C', 'F', 'F', ' '));
    unsigned len = hb_blob_get_length(t);
    hb_blob_destroy(t);
    return len > 0;
}

std::string Font::postscript_name() const {
    char buf[256];
    unsigned len = sizeof buf - 1;
    hb_ot_name_get_utf8(m_face, HB_OT_NAME_ID_POSTSCRIPT_NAME,
                        HB_LANGUAGE_INVALID, &len, buf);
    if (len > 0)
        return std::string(buf, len);
    /* Fallback: the file name without extension, spaces stripped. */
    std::string n = m_name;
    size_t dot = n.find_last_of('.');
    if (dot != std::string::npos)
        n.resize(dot);
    std::string out;
    for (char c : n)
        if (c > 32 && c < 127 && c != '(' && c != ')' && c != '<' && c != '>' &&
            c != '[' && c != ']' && c != '{' && c != '}' && c != '/' && c != '%')
            out += c;
    return out.empty() ? "Font" : out;
}

void Font::glyph_bounds(uint32_t gid, float &x0, float &y0, float &x1, float &y1) const {
    hb_glyph_extents_t e;
    if (!hb_font_get_glyph_extents(m_font, gid, &e)) {
        x0 = y0 = x1 = y1 = 0;
        return;
    }
    x0 = (float) e.x_bearing;
    y0 = (float) (e.y_bearing + e.height);   // y up: bottom
    x1 = (float) (e.x_bearing + e.width);
    y1 = (float) e.y_bearing;                // top
}

/* ---- Outline extraction -------------------------------------------------- */

namespace {

void draw_move(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float x, float y, void *) {
    ((GlyphOutline *) d)->cmds.push_back({PathCmd::Move, 0, 0, 0, 0, x, y});
}
void draw_line(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float x, float y, void *) {
    ((GlyphOutline *) d)->cmds.push_back({PathCmd::Line, 0, 0, 0, 0, x, y});
}
void draw_quad(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float cx, float cy,
               float x, float y, void *) {
    ((GlyphOutline *) d)->cmds.push_back({PathCmd::Quad, cx, cy, 0, 0, x, y});
}
void draw_cubic(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float c1x, float c1y,
                float c2x, float c2y, float x, float y, void *) {
    ((GlyphOutline *) d)->cmds.push_back({PathCmd::Cubic, c1x, c1y, c2x, c2y, x, y});
}
void draw_close(hb_draw_funcs_t *, void *d, hb_draw_state_t *, void *) {
    ((GlyphOutline *) d)->cmds.push_back({PathCmd::Close, 0, 0, 0, 0, 0, 0});
}

hb_draw_funcs_t *outline_funcs() {
    static hb_draw_funcs_t *funcs = [] {
        hb_draw_funcs_t *f = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(f, draw_move, nullptr, nullptr);
        hb_draw_funcs_set_line_to_func(f, draw_line, nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(f, draw_quad, nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(f, draw_cubic, nullptr, nullptr);
        hb_draw_funcs_set_close_path_func(f, draw_close, nullptr, nullptr);
        hb_draw_funcs_make_immutable(f);
        return f;
    }();
    return funcs;
}

/* Mark each contour solid or hole by comparing its winding with the
 * largest contour's. Control points are included in the shoelace sum,
 * which keeps the sign right for glyph-shaped contours. */
void classify_contours(GlyphOutline &o) {
    std::vector<double> areas;
    double area = 0, px = 0, py = 0, sx = 0, sy = 0;
    bool open = false;
    auto edge = [&](double x, double y) {
        area += (px * y - x * py);
        px = x; py = y;
    };
    auto finish = [&] {
        if (!open) return;
        edge(sx, sy);
        areas.push_back(area);
        open = false;
    };
    for (const PathCmd &c : o.cmds) {
        switch (c.kind) {
        case PathCmd::Move:
            finish();
            area = 0; px = sx = c.x; py = sy = c.y; open = true;
            break;
        case PathCmd::Line:  edge(c.x, c.y); break;
        case PathCmd::Quad:  edge(c.cx1, c.cy1); edge(c.x, c.y); break;
        case PathCmd::Cubic: edge(c.cx1, c.cy1); edge(c.cx2, c.cy2); edge(c.x, c.y); break;
        case PathCmd::Close: finish(); break;
        }
    }
    finish();

    size_t biggest = 0;
    for (size_t i = 1; i < areas.size(); ++i)
        if (std::fabs(areas[i]) > std::fabs(areas[biggest]))
            biggest = i;
    o.hole.assign(areas.size(), false);
    for (size_t i = 0; i < areas.size(); ++i)
        o.hole[i] = (areas[i] < 0) != (areas[biggest] < 0);
}

} // namespace

const GlyphOutline &Font::outline(uint32_t gid) const {
    auto it = m_outlines.find(gid);
    if (it != m_outlines.end())
        return it->second;
    GlyphOutline &o = m_outlines[gid];
    hb_font_draw_glyph(m_font, gid, outline_funcs(), &o);
    classify_contours(o);
    return o;
}

/* ---- FontLibrary ------------------------------------------------------- */

namespace {

int weight_from_style(const std::string &style) {
    auto has = [&](const char *s) { return style.find(s) != std::string::npos; };
    if (has("Thin") || has("Hairline")) return 100;
    if (has("ExtraLight") || has("Extra Light") || has("UltraLight") || has("Ultra Light"))
        return 200;
    if (has("Light")) return 300;
    if (has("Medium")) return 500;
    if (has("SemiBold") || has("Semi Bold") || has("DemiBold") || has("Demi Bold") || has("Demi"))
        return 600;
    if (has("ExtraBold") || has("Extra Bold") || has("UltraBold") || has("Ultra Bold"))
        return 800;
    if (has("Black") || has("Heavy")) return 900;
    if (has("Bold")) return 700;
    return 400;
}

bool italic_from_style(const std::string &style) {
    return style.find("Italic") != std::string::npos || style.find("Oblique") != std::string::npos;
}

} // namespace

std::string FontLibrary::width_of(const std::string &style) {
    static const char *keys[] = {"Condensed", "Narrow", "Compressed", "Expanded", "Extended", "Wide"};
    for (const char *k : keys)
        if (style.find(k) != std::string::npos)
            return k;
    return {};
}

FontLibrary::Entry FontLibrary::loaded_entry(const std::string &family, const std::string &style,
                                             std::shared_ptr<Font> font, bool document) const {
    Entry e;
    e.family = family;
    e.style = style;
    e.font = std::move(font);
    e.path = e.font ? e.font->path() : std::string();
    e.weight = weight_from_style(style);
    e.italic = italic_from_style(style);
    e.document = document;
    return e;
}

const Font *FontLibrary::ensure(const Entry &e) const {
    if (e.font)
        return e.font.get();
    if (e.failed || e.path.empty())
        return nullptr;
    e.font = Font::load_file(e.path, e.index);
    if (!e.font)
        e.failed = true;
    return e.font.get();
}

void FontLibrary::add(const std::string &family, const std::string &style,
                      std::shared_ptr<Font> font) {
    if (font)
        m_faces.push_back(loaded_entry(family, style, std::move(font), false));
}

bool FontLibrary::add_file(const std::string &family, const std::string &style,
                           const std::string &path) {
    auto f = Font::load_file(path);
    if (!f)
        return false;
    add(family, style, std::move(f));
    return true;
}

void FontLibrary::add_catalog(const std::string &family, const std::string &style,
                              const std::string &path, unsigned index, int weight, bool italic) {
    if (family.empty() || path.empty())
        return;
    for (const Entry &e : m_faces)
        if (e.family == family && e.style == style)
            return;
    Entry e;
    e.family = family;
    e.style = style.empty() ? "Regular" : style;
    e.path = path;
    e.index = index;
    e.weight = weight > 0 ? weight : weight_from_style(e.style);
    e.italic = italic || italic_from_style(e.style);
    m_faces.push_back(std::move(e));
}

void FontLibrary::add_document_font(const std::string &family, const std::string &style,
                                    std::shared_ptr<Font> font) {
    if (!font)
        return;
    /* Ahead of the installed faces (and any earlier document fonts), so
     * find() picks it first. */
    auto first_installed = std::find_if(m_faces.begin(), m_faces.end(),
                                        [](const Entry &e) { return !e.document; });
    m_faces.insert(first_installed, loaded_entry(family, style, std::move(font), true));
}

void FontLibrary::clear_document_fonts() {
    m_faces.erase(std::remove_if(m_faces.begin(), m_faces.end(),
                                 [](const Entry &e) { return e.document; }),
                  m_faces.end());
}

const char *FontLibrary::style_name(bool bold, bool italic) {
    return bold ? (italic ? "Bold Italic" : "Bold") : (italic ? "Italic" : "Regular");
}

bool FontLibrary::has_family(const std::string &family) const {
    for (const Entry &e : m_faces)
        if (e.family == family)
            return true;
    return false;
}

bool FontLibrary::has_installed_family(const std::string &family) const {
    for (const Entry &e : m_faces)
        if (!e.document && e.family == family)
            return true;
    return false;
}

std::vector<std::string> FontLibrary::families() const {
    std::vector<std::string> out;
    for (const Entry &e : m_faces)
        if (std::find(out.begin(), out.end(), e.family) == out.end())
            out.push_back(e.family);
    return out;
}

std::vector<FaceDesc> FontLibrary::faces(const std::string &family) const {
    std::vector<FaceDesc> out;
    for (const Entry &e : m_faces)
        if (e.family == family && !e.failed)
            out.push_back({e.style, e.weight, e.italic});
    std::stable_sort(out.begin(), out.end(), [](const FaceDesc &a, const FaceDesc &b) {
        if (a.weight != b.weight) return a.weight < b.weight;
        if (a.italic != b.italic) return !a.italic && b.italic;
        return a.style < b.style;
    });
    return out;
}

bool FontLibrary::describe(const std::string &family, const std::string &style,
                           int &weight, bool &italic) const {
    for (const Entry &e : m_faces)
        if (e.family == family && e.style == style) {
            weight = e.weight;
            italic = e.italic;
            return true;
        }
    return false;
}

const FontLibrary::Entry *FontLibrary::best(const std::string &family, int weight, bool italic,
                                            const std::string &width,
                                            const std::string &prefer) const {
    const Entry *chosen = nullptr;
    int chosen_score = 0;
    for (const Entry &e : m_faces) {
        if (e.family != family || e.failed)
            continue;
        int score = std::abs(e.weight - weight);
        if (e.italic != italic)
            score += 1000;
        const std::string w = width_of(e.style);
        if (!width.empty()) {
            if (w != width)
                score += 300;
        } else if (!w.empty()) {
            score += 40;
        }
        if (weight >= 600 && e.weight < 550)
            score += 120;
        if (weight <= 450 && e.weight >= 600)
            score += 120;
        if (!prefer.empty() && e.italic && e.style.find(prefer) == std::string::npos)
            score += 15;
        if (!chosen || score < chosen_score) {
            chosen = &e;
            chosen_score = score;
        }
    }
    return chosen;
}

const Font *FontLibrary::face_for(const std::string &family, bool bold, bool italic) const {
    const char *want = style_name(bold, italic);
    const Entry *exact = nullptr;
    const Entry *regular = nullptr;
    for (const Entry &e : m_faces) {
        if (e.family != family)
            continue;
        if (e.style == want) {
            exact = &e;
            break;
        }
        if (!regular && (e.style == "Regular" || e.style == "Roman" ||
                         e.style == "Book" || e.style == "Normal"))
            regular = &e;
    }
    if (exact)
        if (const Font *f = ensure(*exact))
            return f;
    if (const Entry *b = best(family, bold ? 700 : 400, italic, "", ""))
        if (const Font *f = ensure(*b))
            return f;
    if (regular)
        if (const Font *f = ensure(*regular))
            return f;
    for (const Entry &e : m_faces)
        if (e.family == family)
            if (const Font *f = ensure(e))
                return f;
    return nullptr;
}

const Font *FontLibrary::find(const std::string &family, bool bold, bool italic) const {
    if (const Font *f = face_for(family, bold, italic))
        return f;
    for (const Entry &e : m_faces)
        if (const Font *f = ensure(e))
            return f;
    return nullptr;
}

const Font *FontLibrary::find(const std::string &family, const std::string &style,
                              bool bold, bool italic) const {
    if (!style.empty())
        for (const Entry &e : m_faces)
            if (e.family == family && e.style == style)
                if (const Font *f = ensure(e))
                    return f;
    return find(family, bold, italic);
}

const Font *FontLibrary::match(const std::string &family, int weight, bool italic,
                               const std::string &width, std::string *style_out,
                               const std::string &prefer) const {
    const Entry *e = best(family, weight, italic, width, prefer);
    if (!e)
        return nullptr;
    if (style_out)
        *style_out = e->style;
    return ensure(*e);
}

std::string FontLibrary::resolved_style(const std::string &family, const std::string &face,
                                        bool bold, bool italic) const {
    if (!face.empty())
        for (const Entry &e : m_faces)
            if (e.family == family && e.style == face)
                return e.style;
    const char *want = style_name(bold, italic);
    for (const Entry &e : m_faces)
        if (e.family == family && e.style == want)
            return e.style;
    if (const Entry *e = best(family, bold ? 700 : 400, italic, "", ""))
        return e->style;
    return {};
}

} // namespace pagemade
