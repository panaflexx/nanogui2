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
 * draw glyphs as paths. A returned outline stays at that address for the
 * life of the Font, so a caller may hold it across later outline() calls.
 */
#pragma once

#include <cstdint>
#include <deque>
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
    /* The Font keeps the bytes (fonts embedded in a publication). */
    static std::shared_ptr<Font> load_bytes(std::vector<char> bytes, const std::string &name,
                                            unsigned index = 0);
    ~Font();

    Font(const Font &) = delete;
    Font &operator=(const Font &) = delete;

    const std::string &name() const { return m_name; }
    /* The file the font came from; empty when it was loaded from memory. */
    const std::string &path() const { return m_path; }
    /* The whole font file, for embedding. */
    const char *data(size_t *size) const;
    int   units_per_em() const { return m_upem; }
    float ascender() const   { return m_ascender; }
    float descender() const  { return m_descender; }   // negative
    float cap_height() const { return m_cap_height; }
    float x_height() const   { return m_x_height; }
    /* Design units, y up. Underline position is typically negative. */
    float underline_position() const { return m_ul_pos; }
    float underline_thickness() const { return m_ul_thick; }
    float strike_position() const { return m_st_pos; }
    float strike_thickness() const { return m_st_thick; }
    /* True when the face has an OpenType small-caps feature. */
    bool has_small_caps() const;
    /* Scale for synthesized small caps (x-height over cap height). */
    float small_cap_scale() const;

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
    std::string m_path;
    int         m_upem = 1000;
    float       m_ascender = 800, m_descender = -200;
    float       m_cap_height = 700, m_x_height = 500;
    uint32_t    m_space_gid = 0;
    float       m_ul_pos = -100, m_ul_thick = 50;
    float       m_st_pos = 250, m_st_thick = 50;
    mutable bool m_smcp_known = false, m_smcp = false;
    /* The deque owns the outlines; the map only points at them. Inserting
     * another glyph rehashes the map and does not move the outlines. */
    mutable std::deque<GlyphOutline> m_outline_nodes;
    mutable std::unordered_map<uint32_t, GlyphOutline *> m_outlines;
};

/* One cut of a family, whether or not its file has been loaded yet. */
struct FaceDesc {
    std::string style;
    int  weight = 400;           // 100–900
    bool italic = false;
};

/* Family + style -> Font. The built-in families use "Regular", "Bold",
 * "Italic" and "Bold Italic". Installed faces keep the style name from the
 * system ("Light", "Condensed Bold") and are loaded the first time they
 * are shaped. find() falls back within the family, then to the first face
 * registered, so a missing font never stops composition.
 *
 * Document fonts (embedded in the open publication) come before installed
 * ones, so the publication looks the same on any computer; opening another
 * publication clears them. */
class FontLibrary {
public:
    void add(const std::string &family, const std::string &style,
             std::shared_ptr<Font> font);
    bool add_file(const std::string &family, const std::string &style,
                  const std::string &path);
    /* Remember a face and load it on first use. A family+style already
     * registered (the built-in Serif/Sans/Display cuts) is left as it is. */
    void add_catalog(const std::string &family, const std::string &style,
                     const std::string &path, unsigned index, int weight, bool italic);
    void add_document_font(const std::string &family, const std::string &style,
                           std::shared_ptr<Font> font);
    void clear_document_fonts();
    bool has_family(const std::string &family) const;
    /* Installed faces only, not document fonts. */
    bool has_installed_family(const std::string &family) const;
    /* Families in the order they were added. */
    std::vector<std::string> families() const;
    /* Cuts of one family, lightest first, roman before italic. */
    std::vector<FaceDesc> faces(const std::string &family) const;
    /* Weight and italic of a named cut. False when the family has no such style. */
    bool describe(const std::string &family, const std::string &style,
                  int &weight, bool &italic) const;
    const Font *find(const std::string &family, bool bold, bool italic) const;
    /* `style` empty: bold and italic. Otherwise that cut, then bold/italic. */
    const Font *find(const std::string &family, const std::string &style,
                     bool bold, bool italic) const;
    /* The requested style of this family, or the closest weight. Null when
     * the family has nothing — never a face from some other family. */
    const Font *face_for(const std::string &family, bool bold, bool italic) const;
    /* Closest cut. `width` is a token such as "Condensed" kept when the
     * weight changes; empty prefers a face with no width token. `prefer`
     * ("Italic", "Oblique") breaks ties. Writes the chosen style name. */
    const Font *match(const std::string &family, int weight, bool italic,
                      const std::string &width, std::string *style_out = nullptr,
                      const std::string &prefer = {}) const;
    /* The style name find() would use, or empty when the family is missing. */
    std::string resolved_style(const std::string &family, const std::string &face,
                               bool bold, bool italic) const;
    bool empty() const { return m_faces.empty(); }
    static const char *style_name(bool bold, bool italic);
    /* "Condensed", "Narrow", "Expanded", … or empty. */
    static std::string width_of(const std::string &style);

private:
    struct Entry {
        std::string family, style, path;
        unsigned index = 0;
        int  weight = 400;
        bool italic = false;
        bool document = false;
        mutable std::shared_ptr<Font> font;
        mutable bool failed = false;
    };
    const Font *ensure(const Entry &e) const;
    const Entry *best(const std::string &family, int weight, bool italic,
                      const std::string &width, const std::string &prefer) const;
    Entry loaded_entry(const std::string &family, const std::string &style,
                       std::shared_ptr<Font> font, bool document) const;
    std::vector<Entry> m_faces;
};

} // namespace pagemade
