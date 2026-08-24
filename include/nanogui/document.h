/*
    nanogui/document.h -- Rich-text document model (Style / Text /
                          Paragraph / Document) suitable for laying
                          out markup or driving a text/code editor.

    This is the same data model used by the nanovg-colorfont-atlas
    demo, promoted to a reusable header so it can back both a
    Markdown / rich-text renderer and the TextEditor widget.

    All offsets are in BYTES; this matches tree-sitter's TSPoint
    convention used elsewhere in the project.
*/
#pragma once

#include <nanogui/common.h>
#include <nanovg.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// Style: per-run typographic attributes (size, color, bold/italic/underline,
// monospace, optional background fill).
// ---------------------------------------------------------------------------

enum class TextAlignment { Left, Center, Right, Justify };

enum class WhiteSpace { Normal, Nowrap, Pre, PreWrap };

struct NANOGUI_EXPORT Style {
    float    fontSize  = 18.0f;
    NVGcolor fgColor   = NVGcolor{ { { 0.f, 0.f, 0.f, 1.f } } };
    NVGcolor bgColor   = NVGcolor{ { { 0.f, 0.f, 0.f, 0.f } } };
    bool     bold      = false;
    bool     italic    = false;
    bool     underline = false;
    bool     strike    = false;   ///< line-through (sale prices, <s>/<strike>)
    bool     allCaps   = false;   ///< text-transform:uppercase (CTA LEARN MORE)
    bool     monospace = false;
    bool     displayNone = false; ///< HTML display:none (skip when walking)
    bool     superscript = false; ///< vertical-align: super/top (price cents)
    bool     verticalMiddle = false; ///< vertical-align:middle — center in line box
    float    lineHeight = 0.0f;   ///< 0 = normal (font metrics), otherwise absolute px
    float    letterSpacing = 0.0f;///< extra px between glyphs (CTA tracking)
    float    opacity = 1.0f;      ///< 0..1 mul for fg/bg (CSS opacity)
    float    padX = 0.0f;         ///< extra fill padding (CTA pills)
    float    padY = 0.0f;
    float    borderWidth = 0.0f;  ///< CSS border (Learn More outline)
    NVGcolor borderColor = NVGcolor{ { { 0.f, 0.f, 0.f, 0.f } } };
    WhiteSpace whiteSpace = WhiteSpace::Normal;
};

// ---------------------------------------------------------------------------
// Text: a single styled run inside a paragraph.
// ---------------------------------------------------------------------------

class NANOGUI_EXPORT Text {
public:
    std::string content;
    Style       style;

    /* Inline image (an icon sitting next to text, e.g. a notification
     * "pill") instead of a text run.  When `isImageRun` is set, `content`
     * is ignored for measurement/painting and the run behaves as a
     * single "word" of size image_w x image_h, bottom-aligned to the
     * line's baseline.  `image` is the resolved NVG texture id and is
     * frequently 0 (not loaded yet) — `isImageRun` is what marks this as
     * an image slot; it does NOT mean the texture is ready (mirrors
     * Paragraph::isImage, which is likewise independent of `image`'s
     * value so an unresolved photo still reserves its layout box and
     * draws a placeholder). A block-level standalone image should still
     * use Paragraph::isImage instead — that gets its own line and can be
     * center/right-aligned as a whole; this is for content that must
     * stay in the surrounding text flow. */
    bool        isImageRun = false;
    int         image      = 0;
    float       image_w    = 0.0f;
    float       image_h    = 0.0f;
    std::string image_src;  ///< original <img src>, for async texture rebind

    Text() = default;
    Text(std::string text, const Style& s = Style{})
        : content(std::move(text)), style(s) {}
};

// ---------------------------------------------------------------------------
// Paragraph: ordered runs sharing a baseline / alignment / indent.
// ---------------------------------------------------------------------------

class NANOGUI_EXPORT Paragraph {
public:
    std::vector<Text> runs;
    float             leftIndent      = 0.0f;
    float             firstLineIndent = 0.0f;
    TextAlignment     alignment       = TextAlignment::Left;
    // Horizontal-rule paragraph: skips text layout and draws a line instead.
    bool              isRule          = false;
    NVGcolor          ruleColor       = NVGcolor{ { { 0.65f, 0.65f, 0.70f, 1.f } } };
    float             ruleThickness   = 1.0f;
    // Bullet list item ("- " in markdown): draws a marker before the first
    // line; text is expected to be indented via leftIndent.
    bool              isBullet        = false;
    // Image block paragraph: skips text layout and draws a full-width
    // (aspect-preserving) image instead.  `image` is an NVG image id
    // owned by the caller; image_w/image_h are the intrinsic pixel size.
    bool              isImage         = false;
    int               image           = 0;
    float             image_w         = 0.0f;
    float             image_h         = 0.0f;
    /* Original <img src>, used to bind a texture after an async fetch. */
    std::string       image_src;

    Paragraph() = default;

    void addText(std::string text, const Style& style = Style{}) {
        runs.emplace_back(std::move(text), style);
    }
    void addText(const Text& t) { runs.push_back(t); }

    /// Split the run containing byte offset `col` so a run boundary exists
    /// at `col`; returns the index of the run starting at `col`.
    size_t      split_run_at(size_t col);

    /// Merge adjacent runs with identical styles and drop empty runs.
    void        coalesce_runs();

    /// Concatenate every run's content into a single string.
    std::string plain_text() const;

    /// Byte length of `plain_text()` without allocating.
    size_t      byte_length() const;
};

// ---------------------------------------------------------------------------
// Document: ordered paragraphs plus measurement caches and a NanoVG
// renderer that supports wrapping, justification, monospace blocks,
// underline, per-run background, and a debug overlay.
// ---------------------------------------------------------------------------

class NANOGUI_EXPORT Document {
public:
    std::vector<std::unique_ptr<Paragraph>> paragraphs;
    float contentWidth     = 700.0f; // wrap width in pixels
    float paragraphSpacing = 12.0f;  // gap between paragraphs
    float lineSpacing      = 4.0f;   // extra gap between lines in a paragraph
    bool  debugDraw        = false;  // overlay word/line debug visualisations
    /// When true, draw()/drawParagraph() run layout only and emit NO paint
    /// calls (nvgText/rect/stroke are skipped; pure measurement such as
    /// nvgTextBounds still runs).  Needed because this NanoVG fork records
    /// paint calls into a frame list — painting outside nvgBeginFrame leaks
    /// the packets into the next frame at stale coordinates.  Measuring
    /// widgets (e.g. HtmlDocument's height-for-width probes) set this
    /// around their probe draw and read last_drawn_height.
    bool  layout_only      = false;
    mutable float last_drawn_height = 0.0f; ///< Set by draw(); total content height in pixels.

    Paragraph* addParagraph();
    Paragraph* addParagraph(std::string text, const Style& style = Style{});

    /// Insert a fresh empty paragraph before `index` (or at end if oor).
    Paragraph* insertParagraph(size_t index);

    /// Remove paragraph at `index`. Returns false if oor.
    bool       removeParagraph(size_t index);

    /// Total plain-text length (sum of paragraph byte_length() plus '\n'
    /// separators), useful for editors that map a single linear offset
    /// into (paragraph, run, col).
    size_t     total_byte_length() const;

    /// Render the document using `originX/originY` as the top-left of
    /// the text block. Behaves the same as the renderer in
    /// nanovg-colorfont-atlas.cpp.
    void       draw(NVGcontext* ctx, float originX, float originY);

    /// Widest hard-broken line if the text were never soft-wrapped (CSS
    /// "max-content" width): the sum of word advances + inter-word
    /// spacing between explicit '\n's, maxed across paragraphs.  Image
    /// paragraphs contribute their own width.  Pure measurement — reads
    /// paragraphs/runs and the word/space caches, touches no layout
    /// state (contentWidth, m_rich_layout, ...), so it's safe to call
    /// from preferred_size() without disturbing a real draw()'s cache.
    float      measure_natural_width(NVGcontext* ctx);

    // -------------------------------------------------------------------
    // Lower-level helpers exposed so editor widgets can do their own
    // line-by-line layout (e.g. monospace code mode with no wrap).
    // -------------------------------------------------------------------

    struct VMetrics { float ascender, descender, lineh; };

    /// Return the NanoVG font face name used for the given style.
    static const char* faceForStyle(const Style& s);

    /// Configure `ctx` for the given style (sets size and face).
    void               applyFont(NVGcontext* ctx, const Style& s) const;

    /// Cached metrics lookup (ascender / descender / line height).
    VMetrics           metricsFor(NVGcontext* ctx, const Style& s);

    /// Cached width of a single space in the given style.
    float              spaceWidthFor(NVGcontext* ctx, const Style& s);

    /// Measure a single word / glyph cluster at origin (0,0).
    void               measureWord(NVGcontext* ctx, const Style& s,
                                   const char* begin, const char* end,
                                   float& advance, float& leftBearing,
                                   float& visualRight);

    /// Render one paragraph using the wrapping layout; returns the
    /// y-coordinate where the next paragraph should start.
    float              drawParagraph(NVGcontext* ctx, const Paragraph& para,
                                     float originX, float startY,
                                     size_t para_idx = SIZE_MAX);

    /// Discard any cached font measurements (call after font reloads).
    void               clearMeasurementCaches();

    /// Mark rich layout dirty (content / width / font change). Next draw reflows.
    void               markLayoutDirty() { m_layout_dirty = true; }

    /// True if layout must be rebuilt before drawing or hit-testing.
    bool               layoutDirty() const { return m_layout_dirty; }

    /// Shift the cached rich layout by (dx, dy) without re-measuring.
    /// Used when only the draw origin moved (widget x,y), not wrap width.
    void               translate_rich_layout(float dx, float dy);

    // -----------------------------------------------------------------------
    // Rich-text layout cache: populated during draw(); used by TextEditor for
    // caret / selection rendering and mouse / keyboard hit-testing.
    // -----------------------------------------------------------------------

    /// Per-word geometry captured during layout (used for hit-test + replay).
    struct WordLayout {
        size_t byte_start;   ///< byte offset in paragraph plain_text (inclusive)
        size_t byte_end;     ///< exclusive
        float  x;            ///< drawn x position
        float  advance;      ///< advance width (text advance, not visual bounds)
        float  leftBearing = 0.f;
        float  visualRight = 0.f;
        Style  style;        ///< style snapshot for cheap re-draw without re-layout
        std::string text;    ///< word / glyph cluster text
        int    image = 0;    ///< NVG image id for "\x01IMAGE" sentinel words
    };

    /// Geometry of one visual line (possibly a soft-wrapped portion of a paragraph).
    struct RichLine {
        size_t               para_idx;    ///< paragraph index
        size_t               byte_start;  ///< byte range in para plain_text
        size_t               byte_end;
        float                y_top;       ///< top of row (pre-scroll, in draw-space)
        float                y_bottom;    ///< bottom of row (includes lineSpacing)
        float                baseline;    ///< NVG baseline y
        float                x_start;     ///< x where first word begins
        std::vector<WordLayout> words;

        /// Unified monospace code-block background (from blockWidth pass).
        /// Replayed on the fast path so blocks don't flash once then vanish.
        bool     mono_bg       = false;
        float    mono_bg_x     = 0.f;
        float    mono_bg_y     = 0.f;
        float    mono_bg_w     = 0.f;
        float    mono_bg_h     = 0.f;
        float    mono_bg_radius = 0.f;
        NVGcolor mono_bg_color = NVGcolor{ { { 0.f, 0.f, 0.f, 0.f } } };
        float    mono_bg_border_w = 0.f;
        NVGcolor mono_bg_border_color = NVGcolor{ { { 0.f, 0.f, 0.f, 0.f } } };

        /// Bullet marker for list-item paragraphs (drawn as a filled circle,
        /// font-independent). Replayed on the fast path.
        bool     bullet       = false;
        float    bullet_cx    = 0.f;
        float    bullet_cy    = 0.f;
        float    bullet_r     = 0.f;
        NVGcolor bullet_color = NVGcolor{ { { 0.f, 0.f, 0.f, 1.f } } };
    };

    /// Caret geometry returned by richCaretInfo().
    struct CaretInfo {
        float x        = 0.f;
        float y_top    = 0.f;
        float y_bottom = 0.f;
        bool  valid    = false;
    };

    /// All visual lines from the most recent draw() call.
    mutable std::vector<RichLine> m_rich_layout;
    mutable float                 m_layout_origin_x = 0.f;
    mutable float                 m_layout_origin_y = 0.f;

    /// Return (para_idx, byte_col) for a point in draw-space coordinates.
    /// Uses m_rich_layout populated by the last draw() call.
    std::pair<size_t,size_t> richHitTest(NVGcontext* ctx, float x, float y) const;

    /// Return caret geometry (draw-space) for (para_idx, byte_col).
    CaretInfo richCaretInfo(NVGcontext* ctx, size_t para_idx, size_t byte_col) const;

    /// Return index into m_rich_layout for the visual line containing the position,
    /// or m_rich_layout.size() if not found.
    size_t richLineIndex(size_t para_idx, size_t byte_col) const;

private:
    static uint64_t makeKey(const char* face, float size);

    // Layout validity (avoid reflowing whole document every mouse-drag frame).
    mutable bool  m_layout_dirty = true;
    mutable float m_laid_content_width = -1.f;
    mutable float m_laid_origin_x = 0.f;
    mutable float m_laid_origin_y = 0.f;
    mutable float m_laid_height = 0.f;

    std::unordered_map<uint64_t, float>    m_spaceCache;
    std::unordered_map<uint64_t, VMetrics> m_metricsCache;

    struct Word {
        const Text* run;
        std::string text;
        float       advance;
        float       leftBearing;
        float       visualRight;
        size_t      byte_start = 0;   ///< byte offset in paragraph plain_text
        size_t      byte_end   = 0;   ///< exclusive
        bool        spaceBefore = false; ///< source text has whitespace before this word
    };
    struct LayoutLine {
        std::vector<Word> words;
        float advanceWidth = 0.0f;
        float ascent       = 0.0f;
        float descent      = 0.0f;
        bool  hardBreak    = false;
    };
};

NAMESPACE_END(nanogui)
