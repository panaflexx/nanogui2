/*
 * pagemade/composer/story.h — the text a composer flows: a Story is a list
 * of paragraphs, each a list of styled runs.
 *
 * Attributes follow PageMaker's Type Specifications, Paragraph
 * Specifications, Indents/Tabs, Hyphenation and Spacing Attributes
 * dialogs. All lengths are points.
 *
 * Inside a run, '\n' or U+2028 is a line break that stays in the same
 * paragraph (PageMaker's Shift+Return), '\t' advances to the next tab
 * stop, and U+00AD is a discretionary hyphen.
 */
#pragma once

#include <string>
#include <vector>

namespace pagemade {

/* Stand-in until colors become named swatches (RGB / CMYK / spot). */
struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};

struct CharStyle {
    std::string family = "Serif";
    bool  bold = false;
    bool  italic = false;
    float size = 12.f;
    float leading = 0.f;         // 0 = auto (ParaStyle::autoleading percent of size)
    float tracking = 0.f;        // 1/1000 em added after every glyph
    float hscale = 100.f;        // horizontal scale ("Set width"), percent
    float baseline_shift = 0.f;  // positive = up
    bool  kerning = true;        // pair kerning from GPOS / kern
    bool  ligatures = true;      // fi, fl, ...
    Color color;
};

enum class Align { Left, Center, Right, Justify, ForceJustify };

enum class TabAlign { Left, Center, Right, Decimal };

struct TabStop {
    float       pos = 0.f;       // from the text block's left edge
    TabAlign    align = TabAlign::Left;
    std::string leader;          // fill character(s), e.g. "." or "_"; empty = none
};

struct ParaStyle {
    std::string name;            // Styles palette name ("Body text", "Subhead 1", ...)
    Align align = Align::Left;
    float left_indent = 0.f;
    float right_indent = 0.f;
    float first_indent = 0.f;    // relative to left_indent; negative = hanging
    float space_before = 0.f;    // not applied at the top of a text block
    float space_after = 0.f;
    float autoleading = 120.f;   // percent of point size

    /* Spacing attributes (PageMaker defaults). Both are percentages of the
     * font's space width. Justified lines move word spaces between min and
     * max first, then letter spacing between its min and max. Ragged lines
     * use the desired values. */
    float word_min = 75.f, word_desired = 100.f, word_max = 150.f;
    float letter_min = -5.f, letter_desired = 0.f, letter_max = 25.f;

    /* Indents/Tabs. Stops are kept sorted by position; past the last one,
     * default stops repeat every default_tab from the block's left edge.
     * Lines that contain a tab are set flush left. */
    std::vector<TabStop> tabs;
    float default_tab = 36.f;

    /* Hyphenation. Discretionary hyphens (U+00AD) always apply; with
     * hyphenate on, words without them are hyphenated from the patterns.
     * hyphen_limit caps consecutive hyphenated lines (0 = no limit). In
     * ragged text a word is only hyphenated when the line would otherwise
     * end more than hyphen_zone short of the right edge. */
    bool  hyphenate = true;
    int   hyphen_limit = 0;
    float hyphen_zone = 36.f;
};

struct Run {
    CharStyle   style;
    std::string text;            // UTF-8
};

struct Paragraph {
    ParaStyle        style;
    std::vector<Run> runs;
};

struct Story {
    std::vector<Paragraph> paragraphs;
};

inline bool operator==(const Color &a, const Color &b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

inline bool operator==(const CharStyle &a, const CharStyle &b) {
    return a.family == b.family && a.bold == b.bold && a.italic == b.italic &&
           a.size == b.size && a.leading == b.leading && a.tracking == b.tracking &&
           a.hscale == b.hscale && a.baseline_shift == b.baseline_shift &&
           a.kerning == b.kerning && a.ligatures == b.ligatures && a.color == b.color;
}
inline bool operator!=(const CharStyle &a, const CharStyle &b) { return !(a == b); }

} // namespace pagemade
