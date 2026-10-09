/*
 * pagemade/composer/font.h — OpenType faces for the composer.
 *
 * A Font wraps one HarfBuzz face loaded from a file or from memory.
 * Metrics are in font design units; multiply by size / units_per_em()
 * for points. Nothing here is hinted or rounded to pixels: the composer
 * lays out in points so a line breaks the same way at every zoom and in
 * every output (screen, SVG, PDF).
 *
 * Glyph outlines are cached in design units (y up) for backends that
 * draw glyphs as paths.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_subset_input_t;

namespace pagemade {

/* One outline command in design units. Move/Line use (x, y); Quad adds
 * one control point (cx1, cy1); Cubic adds two. */
struct PathCmd {
    enum Kind : uint8_t { Move, Line, Quad, Cubic, Close };
    Kind  kind;
    float cx1, cy1, cx2, cy2, x, y;
};

struct GlyphOutline {
    std::vector<PathCmd> cmds;
    /* One entry per contour, in Move order. True when the contour winds
     * against the glyph's largest contour, i.e. it is a counter (the hole
     * in an 'o'). TrueType and CFF use opposite conventions, so this is
     * measured rather than assumed. */
    std::vector<bool> hole;
    bool empty() const { return cmds.empty(); }
};

class Font {
public:
    /* nullptr if the file can't be read or isn't a font. */
    static std::shared_ptr<Font> load_file(const std::string &path, unsigned index = 0);
    /* `data` must outlive the Font (used for fonts compiled into the binary). */
    static std::shared_ptr<Font> load_memory(const void *data, size_t size, unsigned index = 0);
    ~Font();

    Font(const Font &) = delete;
    Font &operator=(const Font &) = delete;

    const std::string &name() const { return m_name; }
    int   units_per_em() const { return m_upem; }
    float ascender() const   { return m_ascender; }
    float descender() const  { return m_descender; }   // negative
    float cap_height() const { return m_cap_height; }
    float x_height() const   { return m_x_height; }

    hb_font_t *hb() const { return m_font; }
    hb_face_t *hb_face() const { return m_face; }   // for PDF embedding
    bool is_cff() const;                            // CFF outlines vs glyf
    /* The font's PostScript name (name id 6), or a sanitized fallback. */
    std::string postscript_name() const;
    uint32_t space_glyph() const { return m_space_gid; }
    float advance(uint32_t gid) const;   // design units

    const GlyphOutline &outline(uint32_t gid) const;
    /* Union of the outlines' bounding boxes, design units (y up). */
    void glyph_bounds(uint32_t gid, float &x0, float &y0, float &x1, float &y1) const;

private:
    Font() = default;
    static std::shared_ptr<Font> from_blob(hb_blob_t *blob, unsigned index);

    hb_blob_t  *m_blob = nullptr;
    hb_face_t  *m_face = nullptr;
    hb_font_t  *m_font = nullptr;
    std::string m_name;
    int         m_upem = 1000;
    float       m_ascender = 800, m_descender = -200;
    float       m_cap_height = 700, m_x_height = 500;
    uint32_t    m_space_gid = 0;
    mutable std::unordered_map<uint32_t, GlyphOutline> m_outlines;
};

/* Family + style -> Font. Styles are "Regular", "Bold", "Italic" and
 * "Bold Italic". find() falls back to the family's Regular face, then to
 * the first face registered, so a missing font never stops composition. */
class FontLibrary {
public:
    void add(const std::string &family, const std::string &style,
             std::shared_ptr<Font> font);
    bool add_file(const std::string &family, const std::string &style,
                  const std::string &path);
    bool has_family(const std::string &family) const;
    /* Families in the order they were added. */
    std::vector<std::string> families() const;
    const Font *find(const std::string &family, bool bold, bool italic) const;
    bool empty() const { return m_faces.empty(); }

private:
    struct Entry { std::string family, style; std::shared_ptr<Font> font; };
    std::vector<Entry> m_faces;
};

} // namespace pagemade
