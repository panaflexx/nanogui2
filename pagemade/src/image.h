/*
 * pagemade/image.h — pictures as assets, with a loader other formats can join.
 *
 * A placed picture is not the file. The publication keeps an ImageAsset
 * (identity, where the file came from, and the metadata below) and the page
 * item only names that asset. The same record can sit on more than one page,
 * and a future asset library can move it to another document without knowing
 * which page it was on. The bytes themselves live in an ImageStore, beside
 * the document, so an undo snapshot does not copy every pixel.
 *
 * Two views of those bytes:
 *   source   — the file as placed (PNG, JPEG, and later TIFF, HEIC, …).
 *              PDF export embeds this, so a RIP samples the original, not
 *              whatever the screen happened to be showing.
 *   display  — an upright RGBA bitmap no larger than the composer asked
 *              for. The screen texture is this. Building a bigger one never
 *              replaces the source.
 *
 * A new format is an ImageDecoder registered on the loader. PNG and JPEG
 * are built in. TIFF, HEIC and the rest sniff their own magic, fill the
 * same ImageMetadata, and decode to the same upright RGBA; nothing in the
 * page, the file or the PDF path asks which decoder ran.
 */
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pagemade {

/* What every decoder reports. Pixel sizes are the file's; width_px and
 * height_px are after EXIF orientation, which is the size placement uses.
 * ppi is 0 when the file doesn't say — placement then treats one pixel as
 * one point. icc is the raw profile, kept for a later color-managed export
 * and not applied on screen. */
enum class ColorModel { Unknown, Gray, RGB, CMYK };

struct ImageMetadata {
    std::string format;                 // decoder id: "png", "jpeg", "tiff", …
    int width_px = 0, height_px = 0;    // oriented
    int stored_width_px = 0, stored_height_px = 0;
    int bit_depth = 8;                  // per channel, as stored
    int channels = 0;                   // as stored, before a palette expands
    ColorModel model = ColorModel::Unknown;
    bool alpha = false;
    double ppi_x = 0, ppi_y = 0;
    int orientation = 1;                // EXIF 1..8; 1 is upright already
    std::vector<uint8_t> icc;
    /* Camera and authoring tags for display ("Camera", "Taken", "Exposure",
     * …), in a fixed order. Not saved with the document: probing the file
     * again brings them back. */
    std::vector<std::pair<std::string, std::string>> exif;
};

const char *color_model_name(ColorModel m);
ColorModel color_model_from(const std::string &s);

/* Points across `px` pixels at `ppi`. Unspecified resolution is 72, so a
 * pixel is a point until the file says otherwise. */
float image_print_points(int px, double ppi);

/* Maps the PDF image unit square — (0, 0) at the lower left, the file's
 * first row at the top — onto the upright placement rectangle (x, y, w, h)
 * in y-down space. The same orientation the display view bakes into pixels,
 * so a JPEG can be embedded unchanged and still come out upright. */
void image_pdf_matrix(int orientation, float x, float y, float w, float h, float m[6]);

/* The display view of encoded bytes: decoded upright and shrunk so the long
 * edge is at most `long_edge` (0 = full size). Safe on any thread. */
bool make_display(const uint8_t *data, size_t n, int long_edge,
                  std::vector<uint8_t> *rgba, int *w, int *h, std::string *error);

/* Read a whole file. */
bool read_file(const std::string &path, std::vector<uint8_t> *out, std::string *error);

/* Full upright decode: straight RGBA, top row first, width * height * 4. */
struct DecodedImage {
    ImageMetadata meta;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

/* One format. sniff() is the magic-byte test and must not claim another
 * format's files. probe() fills metadata without keeping a raster.
 * decode() returns the upright 8-bit view; the source bytes stay with the
 * caller. Register with ImageLoader::add; the first decoder whose sniff
 * matches is used. */
class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;
    virtual const char *id() const = 0;
    virtual bool sniff(const uint8_t *data, size_t n) const = 0;
    virtual bool probe(const uint8_t *data, size_t n, ImageMetadata *meta,
                       std::string *error) const = 0;
    virtual bool decode(const uint8_t *data, size_t n, DecodedImage *out,
                        std::string *error) const = 0;
};

class ImageLoader {
public:
    void add(std::unique_ptr<ImageDecoder> decoder);
    const ImageDecoder *match(const uint8_t *data, size_t n) const;
    bool probe(const uint8_t *data, size_t n, ImageMetadata *meta, std::string *error) const;
    bool decode(const uint8_t *data, size_t n, DecodedImage *out, std::string *error) const;

private:
    std::vector<std::unique_ptr<ImageDecoder>> m_decoders;
};

/* PNG and JPEG. Further formats call add() on their own loader, or on
 * image_loader() at startup. */
void register_builtin_decoders(ImageLoader &loader);
ImageLoader &image_loader();

/* The session's picture bytes, keyed by ImageAsset::id.
 * Ids come from take_id() and only move forward, including across undo:
 * a placement brought back still finds the bytes it was placed with.
 *
 * A linked picture can be registered without its bytes (add_link) and
 * loaded later: synchronously with load(), or by a loader thread that reads
 * and decodes on its own and hands the result to install(). unload() lets
 * the bytes go again; the path stays, so it can be read back. The source is
 * shared and never changed in place, so another thread can keep reading a
 * copy of the pointer while the store moves on. */
class ImageStore {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    struct Entry {
        std::string path;                // linked file to (re)load from; "" if none
        Bytes source;                    // null while not loaded
        ImageMetadata meta;
        int proxy_edge = 0;             // longest edge of `proxy`; 0 if none yet
        int proxy_w = 0, proxy_h = 0;
        std::vector<uint8_t> proxy;      // upright straight RGBA

        bool loaded() const { return source && !source->empty(); }
        const uint8_t *data() const { return loaded() ? source->data() : nullptr; }
        size_t size() const { return loaded() ? source->size() : 0; }
    };

    ImageStore();
    ImageStore(const ImageStore &) = delete;
    ImageStore &operator=(const ImageStore &) = delete;
    ImageStore(ImageStore &&) noexcept;
    ImageStore &operator=(ImageStore &&) noexcept;

    void clear();
    /* Bumps when the set of bytes is replaced, so a GPU cache can drop
     * textures that belonged to the previous contents. */
    uint64_t generation() const { return m_gen; }

    uint32_t take_id();
    void note_id(uint32_t id);           // an id that arrived from a file

    /* Probe, keep the source, and remember `id`. False leaves the store
     * unchanged apart from the id counter. */
    bool add(uint32_t id, std::vector<uint8_t> bytes, ImageMetadata *meta, std::string *error);
    bool load_file(uint32_t id, const std::string &path, ImageMetadata *meta, std::string *error);

    /* A linked picture, not read yet; `meta` is the document's copy. */
    void add_link(uint32_t id, const std::string &path, const ImageMetadata &meta);
    /* Read a linked picture now (Save, Print, Export). True if loaded. */
    bool load(uint32_t id, std::string *error);
    /* Bytes read and probed elsewhere, with an optional display view. The
     * proxy only replaces a smaller one. */
    void install(uint32_t id, Bytes source, const ImageMetadata &meta,
                 std::vector<uint8_t> proxy, int proxy_w, int proxy_h);
    /* A larger display view decoded elsewhere from the loaded bytes. */
    void set_proxy(uint32_t id, std::vector<uint8_t> proxy, int w, int h);
    /* Drop a linked picture's bytes and display view (keeps path and meta).
     * Pictures without a path keep their bytes: there is no file to reload. */
    bool unload(uint32_t id);
    /* Bytes held for pictures that could be unloaded (linked ones). */
    size_t unloadable_bytes() const;

    const Entry *find(uint32_t id) const;
    /* The current display view without decoding (it may be smaller than
     * wanted, or missing while not loaded). */
    bool cached_display(uint32_t id, const uint8_t **rgba, int *w, int *h) const;

    /* The display view. `long_edge` is how many pixels the composer wants
     * on the long side (a power of two is the usual request). The proxy is
     * rebuilt only when a larger one is asked for, and never past the
     * upright source. */
    bool display(uint32_t id, int long_edge, const uint8_t **rgba, int *w, int *h);

private:
    std::map<uint32_t, Entry> m_entries;
    uint32_t m_next_id = 0;
    uint64_t m_gen = 0;
    void bump();
};

} // namespace pagemade
