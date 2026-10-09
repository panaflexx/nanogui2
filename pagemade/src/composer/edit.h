/*
 * pagemade/composer/edit.h — positions in a story, edits, and the mapping
 * between positions and composed lines (carets and hit testing).
 *
 * A TextPos is a paragraph index plus a byte offset into that paragraph's
 * text (paragraph_text()). Edits keep runs tidy: neighbors with the same
 * style merge and empty runs go, except that an empty paragraph keeps one
 * empty run so it remembers its style.
 *
 * Movement is by grapheme cluster and by word (UAX #29, via libunibreak),
 * so a caret never lands inside "é" written as e + combining accent, or
 * inside an emoji sequence.
 */
#pragma once

#include "composer/composer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace pagemade {

struct TextPos {
    size_t   para = 0;
    uint32_t byte = 0;
};

inline bool operator==(TextPos a, TextPos b) { return a.para == b.para && a.byte == b.byte; }
inline bool operator!=(TextPos a, TextPos b) { return !(a == b); }
inline bool operator<(TextPos a, TextPos b) {
    return a.para < b.para || (a.para == b.para && a.byte < b.byte);
}
inline bool operator<=(TextPos a, TextPos b) { return !(b < a); }

/* ---- Edits -------------------------------------------------------------- */

uint32_t  para_length(const Paragraph &p);
TextPos   story_end(const Story &s);
TextPos   clamp(const Story &s, TextPos p);
/* Style for typing at p: the character before it, or after it at the
 * start of a paragraph. */
CharStyle style_at(const Story &s, TextPos p);

/* Insert UTF-8 text; '\n' (or "\r\n") starts a new paragraph. Returns the
 * position after the inserted text. */
TextPos insert_text(Story &s, TextPos p, const std::string &utf8, const CharStyle &style);
/* Erase [a, b) (either order), joining paragraphs. Returns the start. */
TextPos erase(Story &s, TextPos a, TextPos b);
/* Split the paragraph at p; the new one inherits the paragraph style. */
TextPos split_paragraph(Story &s, TextPos p);
void    restyle(Story &s, TextPos a, TextPos b, const std::function<void(CharStyle &)> &fn);
/* Paragraphs joined with '\n'. */
std::string copy_text(const Story &s, TextPos a, TextPos b);
void    normalize(Paragraph &p);

/* ---- Movement ----------------------------------------------------------- */

TextPos next_grapheme(const Story &s, TextPos p);   // steps over paragraph ends
TextPos prev_grapheme(const Story &s, TextPos p);
TextPos next_word(const Story &s, TextPos p);       // to the end of the next word
TextPos prev_word(const Story &s, TextPos p);       // to the start of the previous word
/* The word (or run of spaces / punctuation) around p, for double-click. */
std::pair<TextPos, TextPos> word_at(const Story &s, TextPos p);

/* ---- Composition <-> positions ------------------------------------------ */

struct Caret {
    size_t line = SIZE_MAX;
    float  x = 0, top = 0, bottom = 0;
    bool   valid() const { return line != SIZE_MAX; }
};

/* Invalid when p is in overset text. */
Caret   caret_at(const Composition &c, const Story &s, TextPos p);
float   x_at(const ComposedLine &l, const std::string &text, uint32_t byte);
/* The position on `line` nearest x. Never returns the end of a line that
 * continues on the next one (that position is drawn on the next line). */
TextPos pos_at_x(const Composition &c, const Story &s, size_t line, float x);
/* The position nearest a page point, looking in the frame under it (or
 * the nearest frame). */
TextPos hit_test(const Composition &c, const Story &s, const std::vector<Frame> &frames,
                 float x, float y);
TextPos line_start(const Composition &c, size_t line);
/* End of the line's visible text: before hanging spaces and a break. */
TextPos line_end(const Composition &c, const Story &s, size_t line);

} // namespace pagemade
