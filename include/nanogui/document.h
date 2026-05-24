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

struct NANOGUI_EXPORT Style {
    float    fontSize  = 18.0f;
    NVGcolor fgColor   = NVGcolor{ { { 0.f, 0.f, 0.f, 1.f } } };
    NVGcolor bgColor   = NVGcolor{ { { 0.f, 0.f, 0.f, 0.f } } };
    bool     bold      = false;
    bool     italic    = false;
    bool     underline = false;
    bool     monospace = false;
};

// ---------------------------------------------------------------------------
// Text: a single styled run inside a paragraph.
// ---------------------------------------------------------------------------

class NANOGUI_EXPORT Text {
public:
    std::string content;
    Style       style;

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

    Paragraph() = default;

    void addText(std::string text, const Style& style = Style{}) {
        runs.emplace_back(std::move(text), style);
    }
    void addText(const Text& t) { runs.push_back(t); }

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
                                     float originX, float startY);

    /// Discard any cached font measurements (call after font reloads).
    void               clearMeasurementCaches();

private:
    static uint64_t makeKey(const char* face, float size);

    std::unordered_map<uint64_t, float>    m_spaceCache;
    std::unordered_map<uint64_t, VMetrics> m_metricsCache;

    struct Word {
        const Text* run;
        std::string text;
        float       advance;
        float       leftBearing;
        float       visualRight;
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
