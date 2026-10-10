/*
 * pagemade/composer/composer.h — flow a Story through a chain of frames.
 *
 * compose() shapes every run with HarfBuzz, finds break opportunities
 * with the Unicode line breaking algorithm (UAX #14), breaks lines
 * greedily (one line at a time, as PageMaker did) with pattern
 * hyphenation, resolves tab stops, justifies with the paragraph's spacing
 * attributes, and threads the story through the frames in order.
 *
 * Wherever a line starts or ends at a point HarfBuzz marks unsafe to
 * break (a ligature or kerning pair would be cut), the text on that side
 * is shaped again on its own, so each line carries exactly the glyphs it
 * would have if it were the whole text. Hyphenated fragments are always
 * shaped again, with the hyphen.
 *
 * The result is glyph ids with absolute positions in page points, which
 * every backend draws as-is: the screen at any zoom, SVG and PDF all see
 * the same line breaks.
 *
 * Leading is proportional: each line's slug is as tall as its largest
 * leading, times the paragraph's line spacing, with the baseline two thirds
 * of the way down.
 */
#pragma once

#include "composer/font.h"
#include "composer/story.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pagemade {

class Hyphenator;

/* A text block's rectangle in page points (y down). */
struct Frame {
    float x = 0, y = 0, w = 0, h = 0;
};

struct PlacedGlyph {
    enum Flags : uint8_t {
        Inserted  = 1,   // not from the text: a hyphen at a break, or a tab leader
        Invisible = 2,   // placed for carets only: tabs, breaks, soft hyphens
        Hanging   = 4,   // trailing space past the end of the line
    };
    uint32_t gid = 0;
    uint32_t cluster = 0;   // byte offset into the paragraph's text
    float    x = 0, y = 0;  // pen position on the baseline, page points
    float    adv = 0;       // distance to the next glyph's pen position
    uint8_t  flags = 0;
};

/* Consecutive glyphs on one line that share font, size and color. */
struct GlyphRun {
    const Font *font = nullptr;
    float size = 12.f;
    float hscale = 100.f;
    Color color;
    bool underline = false;
    bool strike = false;
    std::vector<PlacedGlyph> glyphs;
};

/* Underline and strike for one run, in that order. y is the rule's center. */
struct RuleSpan {
    float x0 = 0, y = 0, x1 = 0, thickness = 0;
};

inline void glyph_run_rules(const GlyphRun &run, std::vector<RuleSpan> &out) {
    out.clear();
    if (!run.font || (!run.underline && !run.strike))
        return;
    float x0 = 0, x1 = 0, base = 0;
    bool span = false;
    for (const PlacedGlyph &g : run.glyphs) {
        if (g.flags & PlacedGlyph::Invisible)
            continue;
        const float right = g.x + std::max(0.f, g.adv);
        if (!span) { x0 = g.x; x1 = right; base = g.y; span = true; }
        else { x0 = std::min(x0, g.x); x1 = std::max(x1, right); }
    }
    if (!span || !(x1 > x0))
        return;
    const float sy = run.size / (float) run.font->units_per_em();
    auto add = [&](float design_y, float design_thick) {
        RuleSpan r;
        r.x0 = x0;
        r.x1 = x1;
        r.y = base - design_y * sy;
        r.thickness = std::max(0.25f, design_thick * sy);
        out.push_back(r);
    };
    if (run.underline)
        add(run.font->underline_position(), run.font->underline_thickness());
    if (run.strike)
        add(run.font->strike_position(), run.font->strike_thickness());
}

struct ComposedLine {
    size_t   frame = 0;
    size_t   para = 0;
    uint32_t byte_start = 0, byte_end = 0;   // range in the paragraph's text
    float    left = 0, width = 0;  // the measure after indents
    float    top = 0, leading = 0, baseline = 0;
    float    natural = 0;          // content width at desired spacing
    float    x_start = 0;          // pen x of the first glyph, after alignment
    float    x_end = 0;            // pen x after the last glyph
    bool     para_end = false;
    bool     hyphenated = false;   // ends with an inserted hyphen
    bool     loose = false;        // spacing went past word/letter max
    bool     tight = false;        // content overflows even at minimum spacing
    std::vector<GlyphRun> runs;
};

struct Composition {
    std::vector<ComposedLine> lines;
    bool     overset = false;      // text left over (PageMaker's red windowshade arrow)
    size_t   overset_para = 0;
    uint32_t overset_byte = 0;
    double   compose_ms = 0;
};

Composition compose(const Story &story, const std::vector<Frame> &frames,
                    const FontLibrary &fonts, const Hyphenator *hyphenator = nullptr);

/* Concatenated run texts of a paragraph; clusters index into this. */
std::string paragraph_text(const Paragraph &p);

} // namespace pagemade
