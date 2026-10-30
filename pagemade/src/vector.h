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
};

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
};

class VectorDecoder {
public:
    virtual ~VectorDecoder() = default;
    virtual const char *id() const = 0;
    virtual bool sniff(const uint8_t *data, size_t n) const = 0;
    virtual bool decode(const uint8_t *data, size_t n, VectorScene *out,
                        std::string *error) const = 0;
    virtual std::vector<std::pair<std::string, std::string>> types() const { return {}; }
};

class VectorLoader {
public:
    void add(std::unique_ptr<VectorDecoder> decoder);
    const VectorDecoder *match(const uint8_t *data, size_t n) const;
    bool decode(const uint8_t *data, size_t n, VectorScene *out, std::string *error) const;

private:
    std::vector<std::unique_ptr<VectorDecoder>> m_decoders;
};

/* The SVG decoder, linked from plugin/svg.cpp. */
void register_svg_decoder(VectorLoader &loader);
VectorLoader &vector_loader();
std::vector<std::pair<std::string, std::string>> vector_file_types();

/* "placed name" or "placed name (gradients, clips skipped)". */
std::string vector_place_note(const std::string &name, const std::vector<std::string> &skipped);

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
    bool add(uint32_t id, std::vector<uint8_t> bytes, std::string *error);
    /* The same with a scene the caller already decoded. */
    bool add(uint32_t id, std::vector<uint8_t> bytes, VectorScene scene);
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
 * the live area. */
void vector_frame(const PageSetup &setup, size_t page, float gw, float gh, const Point *at,
                  float &x, float &y, float &w, float &h);

class ImageStore;

struct VectorPlacement {
    bool ok = false;
    std::string error;
    std::string note;
    uint32_t asset = 0;                  // the first graphic; 0 when there is none
    ItemId item = 0;                     // one placed piece (a member of `group`)
    std::vector<ItemId> items;           // every piece, back to front
    uint32_t group = 0;                  // set when two or more pieces were placed
};

/* One graphic on `page`, or a group when the file embeds pictures and
 * `images` is set. Each embedded picture is placed on its own, and the
 * drawing before and after it stays a graphic, so Ungroup separates the
 * photo. Text stays outlines in those graphics. Without `images`, or with
 * no picture this slice can separate, this is one graphic, as before.
 * The caller records undo. `source` is the file: URI (empty when pasted);
 * a group stores each piece itself and does not keep that link. */
VectorPlacement place_svg_bytes(PageDoc &doc, VectorStore &store, std::vector<uint8_t> bytes,
                                const std::string &name, const std::string &source,
                                size_t page, const Point *at, ImageStore *images = nullptr);

/* Glyph outlines for scene.texts, in graphic-box space. A run already
 * baked is left alone. No font: the run stays empty and is not retried. */
void realize_vector_text(VectorScene &scene, const FontLibrary &fonts);

/* The graphic, mapped into the frame and clipped to it: shapes, baked
 * text and pictures, in paint order. Text is empty until realize_vector_text. */
void append_placed_vector(DrawList &out, const PageDoc &doc, const Item &it,
                          const VectorScene &scene);

} // namespace pagemade
