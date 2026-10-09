/*
 * pagemade/composer/font.cpp — HarfBuzz-backed fonts and the font library.
 */
#include "composer/font.h"

#include <hb.h>
#include <hb-ot.h>

#include <algorithm>
#include <cmath>

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

void FontLibrary::add(const std::string &family, const std::string &style,
                      std::shared_ptr<Font> font) {
    if (font)
        m_faces.push_back({family, style, std::move(font)});
}

bool FontLibrary::add_file(const std::string &family, const std::string &style,
                           const std::string &path) {
    auto f = Font::load_file(path);
    if (!f)
        return false;
    add(family, style, std::move(f));
    return true;
}

void FontLibrary::add_document_font(const std::string &family, const std::string &style,
                                    std::shared_ptr<Font> font) {
    if (!font)
        return;
    /* Ahead of the installed faces (and any earlier document fonts), so
     * find() picks it first. */
    auto first_installed = std::find_if(m_faces.begin(), m_faces.end(),
                                        [](const Entry &e) { return !e.document; });
    m_faces.insert(first_installed, Entry{family, style, std::move(font), true});
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

const Font *FontLibrary::face_for(const std::string &family, bool bold, bool italic) const {
    const char *style = style_name(bold, italic);
    const Font *regular = nullptr;
    for (const Entry &e : m_faces) {
        if (e.family != family)
            continue;
        if (e.style == style)
            return e.font.get();
        if (e.style == "Regular")
            regular = e.font.get();
    }
    return regular;
}

const Font *FontLibrary::find(const std::string &family, bool bold, bool italic) const {
    if (const Font *f = face_for(family, bold, italic))
        return f;
    return m_faces.empty() ? nullptr : m_faces.front().font.get();
}

} // namespace pagemade
