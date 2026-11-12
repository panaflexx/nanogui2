/*
 * pagemade/vector.h — placed vector graphics (SVG and whatever follows).
 *
 * A placed graphic is an asset, like a picture. The page item only names
 * it and says how its box sits in the frame. The file bytes, and a scene
 * parsed from them, live in a VectorStore beside the document, so an undo
 * snapshot does not copy the file. Screen and PDF both draw that scene.
 *
 * The scene is already in the graphic's own box, in points, y down: the
 * decoder has applied the view box and every group transform. Placement
 * then maps that box into the frame the way a picture's rectangle is
 * mapped, and the frame clips it.
 *
 * A format is a VectorDecoder registered on the loader. The SVG decoder
 * lives under plugin/ and is linked in; nothing is loaded from a directory
 * at run time. The page, the draw list and PDF never ask which decoder ran.
 */
#pragma once

#include "drawlist.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <functional>
#include <optional>
#include <vector>

namespace pagemade {

enum class PaintKind { None, Solid, Linear, Radial };

/* A paint. For a gradient, `color` is the stop nearest the middle: the
 * fallback where a gradient can't be drawn. The geometry is as in
 * DrawGradient, with `xf` mapping gradient space to the graphic's box. */
struct VectorPaint {
    PaintKind kind = PaintKind::None;
    Color color{0, 0, 0, 1};
    std::vector<GradientStop> stops;
    Transform xf;
    Point focal{0, 0};
    Spread spread = Spread::Pad;
    bool none() const { return kind == PaintKind::None; }
    bool gradient() const { return kind == PaintKind::Linear || kind == PaintKind::Radial; }
};

struct VectorStroke {
    VectorPaint paint;
    float width = 1.f;                   // points, in the graphic's box
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    float miter_limit = 4.f;
    std::vector<float> dash;
    float dash_offset = 0;
    bool none() const { return paint.none() || width <= 0.f; }
};

/* One run of SVG text, in user units, before VectorText::xf maps it into
 * the graphic's box. realize_vector_text bakes the glyphs into `ink`
 * (graphic-box space) so screen and PDF draw the same outlines. */
struct VectorTextRun {
    std::string text;
    std::vector<std::string> families;
    float size = 16.f;
    int weight = 400;
    bool italic = false;
    bool small_caps = false;
    bool underline = false;
    bool strike = false;
    float letter_spacing = 0.f;
    Color fill{0, 0, 0, 1};
    bool has_x = false;
    bool has_y = false;
    float x = 0, y = 0, dx = 0, dy = 0;
    /* Filled by realize_vector_text. Mutable so a const scene (the store's
     * scene(), a PDF snapshot) can be baked once and reused. */
    mutable Path ink;
    mutable std::shared_ptr<const std::vector<uint8_t>> windings;
    mutable bool ink_set = false;
};

struct VectorText {
    Transform xf;
    std::vector<VectorTextRun> runs;
    int clip = -1;                       // index into VectorScene::clips, or -1
    /* The runs sit where the file put each one (a PDF places its text
     * piece by piece). Unlike SVG words placed one by one, that says
     * nothing about justification. */
    bool placed_runs = false;
};

/* An SVG <image>. x, y, w, h are user units; xf maps them into the box.
 * `bytes` is the decoded picture when href was a data URI. */
struct VectorImage {
    Transform xf;
    float x = 0, y = 0, w = 0, h = 0;
    std::string href;
    std::string preserve;                // preserveAspectRatio
    float opacity = 1.f;
    int clip = -1;                       // index into VectorScene::clips, or -1
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    int px_w = 0, px_h = 0;              // the picture's own size (upright); 0 unknown
};

/* Where an <image> draws its pixels, in its user space: its viewport
 * (x, y, w, h) fitted by preserveAspectRatio (default xMidYMid meet).
 * With "slice" the result overflows the viewport, which clips it. */
void vector_image_view(const VectorImage &im, float &x, float &y, float &w, float &h);

/* One shape in the graphic's box (points, y down). */
struct VectorShape {
    Path path;
    FillRule rule = FillRule::NonZero;
    VectorPaint fill;
    VectorStroke stroke;
    int clip = -1;                       // index into VectorScene::clips, or -1
    /* While decoding, a stand-in for VectorScene::images. Gone before the
     * scene is returned. */
    int image = -1;
    std::shared_ptr<const std::vector<uint8_t>> windings;   // fill_windings(path)
};

/* Painter order. A paste draws a picture between two runs of text; the
 * three vectors alone would paint every shape first. */
enum class VectorSlot { Shape, Text, Image };

struct VectorSlotRef {
    VectorSlot slot = VectorSlot::Shape;
    int index = 0;
};

struct VectorScene {
    float width = 0, height = 0;         // the graphic's box, points
    /* The finished page inside the box and how far its bleed reaches (a
     * PDF's TrimBox and BleedBox), in the box's space. A width of 0: not
     * given, the whole box. A PDF made for print is often a sheet larger
     * than its page, with crop marks out past the bleed. */
    float trim_x = 0, trim_y = 0, trim_w = 0, trim_h = 0;
    float bleed_x = 0, bleed_y = 0, bleed_w = 0, bleed_h = 0;
    /* The decoder that made it ("svg", "pdf"): placement splits an SVG's
     * embedded pictures into frames, but keeps a PDF page whole. */
    std::string format;
    std::vector<VectorShape> shapes;
    /* Clip paths in the graphic's box, shared by the shapes that name them. */
    std::vector<ClipLayers> clips;
    std::vector<VectorText> texts;
    std::vector<VectorImage> images;
    /* Shape, text and image indices in the order the file paints them.
     * Empty on a scene built by hand: shapes, then text, then images. */
    std::vector<VectorSlotRef> order;
    /* Features this slice did not draw, each named once ("gradients",
     * "clips"), in the order they were first seen. */
    std::vector<std::string> skipped;
    /* What was drawn differently from the file, not left out ("fonts not
     * embedded, drawn in installed faces") */
    std::vector<std::string> remarks;
};

class VectorDecoder {
public:
    virtual ~VectorDecoder() = default;
    virtual const char *id() const = 0;
    virtual bool sniff(const uint8_t *data, size_t n) const = 0;
    virtual bool decode(const uint8_t *data, size_t n, VectorScene *out,
                        std::string *error) const = 0;
    virtual std::vector<std::pair<std::string, std::string>> types() const { return {}; }
    /* Formats with pages (PDF): how many, and one of them. Others have one. */
    virtual int page_count(const uint8_t *, size_t) const { return 1; }
    virtual bool decode_page(const uint8_t *data, size_t n, int page, VectorScene *out,
                             std::string *error) const {
        return page == 0 && decode(data, n, out, error);
    }
};

class VectorLoader {
public:
    void add(std::unique_ptr<VectorDecoder> decoder);
    const VectorDecoder *match(const uint8_t *data, size_t n) const;
    /* `page` (0-based) of a format with pages; the only page otherwise */
    bool decode(const uint8_t *data, size_t n, VectorScene *out, std::string *error, int page = 0) const;
    /* Pages in the file: 1 for formats without pages, 0 when it isn't readable */
    int page_count(const uint8_t *data, size_t n) const;
    /* Every decoder's file types (extension, description), for Place. */
    std::vector<std::pair<std::string, std::string>> types() const;

private:
    std::vector<std::unique_ptr<VectorDecoder>> m_decoders;
};

/* The loader with the built-in decoders (plugin/plugin.h). */
VectorLoader &vector_loader();
std::vector<std::pair<std::string, std::string>> vector_file_types();

/* Many pages decoded at once on worker threads: each job is a file's bytes
 * and the page to read. A large PDF of photos takes most of a second a
 * page. `tick(done, total)` runs on the calling thread a few times a
 * second (for a progress bar); returning false stops the work, and this
 * returns false. A page that can't be read leaves its slot empty and its
 * error in `errors` (when given). The decoders keep no shared state. */
struct VectorDecodeJob {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    int page = 0;
};
bool decode_vector_pages(const std::vector<VectorDecodeJob> &jobs, std::vector<std::optional<VectorScene>> &out,
                         std::vector<std::string> *errors = nullptr,
                         const std::function<bool(size_t done, size_t total)> &tick = {});

/* "placed name" or "placed name (gradients, clips skipped)". */
std::string vector_place_note(const std::string &name, const std::vector<std::string> &skipped,
                              const std::vector<std::string> &remarks = {});

class FontLibrary;

/* The session's vector files, keyed by VectorAsset::id. Ids come from
 * take_id and only move forward, including across undo. */
class VectorStore {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    struct Entry {
        std::string path;
        Bytes source;
        VectorScene scene;
        bool loaded() const { return source && !source->empty(); }
    };

    VectorStore() = default;
    VectorStore(const VectorStore &) = delete;
    VectorStore &operator=(const VectorStore &) = delete;
    VectorStore(VectorStore &&) noexcept;
    VectorStore &operator=(VectorStore &&) noexcept;

    void clear();
    uint32_t take_id();
    void note_id(uint32_t id);

    /* Decode, keep the bytes and the scene. False leaves the store
     * unchanged apart from an id already taken by the caller. */
    bool add(uint32_t id, std::vector<uint8_t> bytes, std::string *error, int page = 0);
    /* The same with a scene the caller already decoded. */
    bool add(uint32_t id, std::vector<uint8_t> bytes, VectorScene scene);
    /* Bytes another entry may hold too: each page of one PDF placed (or
     * imported) on its own keeps a single copy of the file. */
    bool add(uint32_t id, Bytes bytes, VectorScene scene);
    bool add(uint32_t id, Bytes bytes, std::string *error, int page = 0);
    void set_path(uint32_t id, const std::string &path);

    /* Shared source bytes and a copy of each scene. PDF export takes this
     * on the UI thread and gives it to the worker. scene() and find()
     * still return pointers into this store, for the thread that mutates
     * it. */
    VectorStore snapshot() const;

    const Entry *find(uint32_t id) const;
    Bytes source_of(uint32_t id) const;
    const VectorScene *scene(uint32_t id) const;

    /* Bake SVG text into outlines. Call on the UI thread before drawing
     * and before snapshot(), so the PDF worker does not shape. */
    void realize(const FontLibrary &fonts);

private:
    std::map<uint32_t, Entry> m_entries;
    uint32_t m_next_id = 0;
};

/* Where a graphic of `gw` by `gh` points lands on `page`. Wider than the
 * column under the drop (the first column when `at` is null) shrinks to
 * that column's width; taller than the live area shrinks to that, and the
 * aspect stays. The top-left is the margin, or `at`, and is kept inside
 * the live area. `to_column` false (a paste) fits the live area's width
 * instead of a column's, so a page-wide drawing keeps its size. */
void vector_frame(const PageSetup &setup, size_t page, float gw, float gh, const Point *at,
                  float &x, float &y, float &w, float &h, bool to_column = true);

/* The family a run's text resolves to in `fonts`: the first of its
 * families the library has, else the pagemade family a well-known name
 * stands for ("P052" is Serif), else Serif. And the face for the run's
 * weight and slant there. Drawing (realize_vector_text) and Paste as Text
 * use the same answer, so text measures the way it is drawn. */
std::string vector_run_family(const FontLibrary *fonts, const VectorTextRun &run);
const Font *vector_run_face(const FontLibrary &fonts, const VectorTextRun &run);

/* Glyph outlines for scene.texts, in graphic-box space. A run already
 * baked is left alone. No font: the run stays empty and is not retried. */
void realize_vector_text(VectorScene &scene, const FontLibrary &fonts);

/* The graphic, mapped into the frame and clipped to it: shapes, baked
 * text and pictures, in paint order. Text is empty until realize_vector_text. */
void append_placed_vector(DrawList &out, const PageDoc &doc, const Item &it,
                          const VectorScene &scene);

} // namespace pagemade
