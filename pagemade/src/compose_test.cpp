/*
 * pagemade/compose_test.cpp — headless checks for the composer.
 *
 *   pagemade_compose_test        run the checks, exit 1 on any failure
 *   pagemade_compose_test -v     also dump the sample document's lines
 */
#include "composer/composer.h"
#include "composer/edit.h"
#include "default_fonts.h"
#include "page.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace pagemade;

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) { ++g_pass; break; }                                      \
        ++g_fail;                                                           \
        std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);         \
        std::printf(__VA_ARGS__);                                           \
        std::printf("\n");                                                  \
    } while (0)

static Story one_para(const std::string &text, const CharStyle &cs, const ParaStyle &ps) {
    Story s;
    Paragraph p;
    p.style = ps;
    p.runs.push_back({cs, text});
    s.paragraphs.push_back(p);
    return s;
}

static float line_extent(const ComposedLine &l) { return l.x_end - l.left; }

static size_t ink_glyphs(const Composition &c) {
    size_t n = 0;
    for (const ComposedLine &l : c.lines)
        for (const GlyphRun &r : l.runs)
            for (const PlacedGlyph &g : r.glyphs)
                if (r.font && !r.font->outline(g.gid).empty())
                    ++n;
    return n;
}

static const std::string kLong =
    "Desktop publishing began with a simple promise: what you see on the screen is "
    "what comes out of the printer. The page is a pasteboard where text and pictures "
    "can be picked up, moved and set down again, while the type itself is measured "
    "with the same metrics the printer will use. Every line here is broken in points, "
    "not pixels, so the column breaks the same way at any zoom.";

static void test_kerning(const FontLibrary &fonts) {
    for (const char *family : {"Serif", "Display"}) {
        CharStyle cs; cs.family = family; cs.bold = true; cs.size = 48;
        ParaStyle ps;
        std::vector<Frame> wide = {{0, 0, 2000, 200}};
        Composition on = compose(one_para("AVATAR WAVE To", cs, ps), wide, fonts);
        cs.kerning = false;
        Composition off = compose(one_para("AVATAR WAVE To", cs, ps), wide, fonts);
        CHECK(on.lines.size() == 1 && off.lines.size() == 1, "%s: one line each", family);
        if (on.lines.empty() || off.lines.empty()) continue;
        float a = line_extent(on.lines[0]), b = line_extent(off.lines[0]);
        std::printf("  kerning %-7s on %.2fpt  off %.2fpt  (%.2fpt tighter)\n", family, a, b, b - a);
        CHECK(a < b - 1.0f, "%s: kerning should tighten AV/WA/To (on %.2f, off %.2f)", family, a, b);
    }
}

static void test_ligatures(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 24;
    ParaStyle ps;
    std::vector<Frame> wide = {{0, 0, 2000, 200}};
    Composition on = compose(one_para("efficient flow", cs, ps), wide, fonts);
    cs.ligatures = false;
    Composition off = compose(one_para("efficient flow", cs, ps), wide, fonts);
    size_t a = ink_glyphs(on), b = ink_glyphs(off);
    std::printf("  ligatures: %zu glyphs with, %zu without\n", a, b);
    if (a == b)
        std::printf("  (skip: the Serif face has no fi/fl ligatures)\n");
    else
        CHECK(a < b, "ligatures should merge glyphs (%zu vs %zu)", a, b);
}

static void test_alignment(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 10;
    std::vector<Frame> col = {{100, 50, 180, 1000}};

    ParaStyle ps; ps.align = Align::Justify;
    Composition j = compose(one_para(kLong, cs, ps), col, fonts);
    CHECK(j.lines.size() > 4, "justified paragraph should wrap (%zu lines)", j.lines.size());
    int justified = 0;
    for (const ComposedLine &l : j.lines) {
        if (l.para_end || l.loose) continue;
        ++justified;
        CHECK(std::fabs(l.x_end - (l.left + l.width)) < 0.01f,
              "justified line should end at the measure (x_end %.3f, right %.3f)",
              l.x_end, l.left + l.width);
    }
    CHECK(justified > 0, "some lines should be justified");
    const ComposedLine &last = j.lines.back();
    CHECK(last.para_end && last.x_end < last.left + last.width - 1,
          "last line of a justified paragraph stays ragged");

    ps.align = Align::Right;
    Composition r = compose(one_para(kLong, cs, ps), col, fonts);
    for (const ComposedLine &l : r.lines)
        CHECK(std::fabs(l.x_end - (l.left + l.width)) < 0.01f, "right-aligned line ends at the measure");

    ps.align = Align::Center;
    Composition c = compose(one_para(kLong, cs, ps), col, fonts);
    for (const ComposedLine &l : c.lines) {
        float x0 = l.runs.front().glyphs.front().x;
        float lm = x0 - l.left, rm = l.left + l.width - l.x_end;
        CHECK(std::fabs(lm - rm) < 1.0f, "centered line margins %.2f / %.2f", lm, rm);
    }

    ps.align = Align::Left;
    Composition left = compose(one_para(kLong, cs, ps), col, fonts);
    for (const ComposedLine &l : left.lines)
        CHECK(l.x_end <= l.left + l.width + 0.01f && !l.tight, "ragged lines stay inside the measure");
}

static void test_threading(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 10; cs.leading = 12;
    ParaStyle ps; ps.align = Align::Justify;
    Story story = one_para(kLong + " " + kLong, cs, ps);

    std::vector<Frame> two = {{0, 0, 150, 60}, {170, 0, 150, 1000}};
    Composition c = compose(story, two, fonts);
    CHECK(!c.overset, "story should fit in two blocks");
    size_t prev = 0;
    for (const ComposedLine &l : c.lines) {
        const Frame &f = two[l.frame];
        CHECK(l.frame >= prev, "lines go to blocks in order");
        CHECK(l.top + l.leading <= f.y + f.h + 0.01f, "slug inside its block");
        CHECK(l.left >= f.x - 0.01f && l.left + l.width <= f.x + f.w + 0.01f, "measure inside its block");
        prev = l.frame;
    }
    CHECK(c.lines.front().frame == 0 && c.lines.back().frame == 1, "text continues into the second block");
    int in_first = 0;
    for (const ComposedLine &l : c.lines) in_first += l.frame == 0;
    CHECK(in_first == 5, "60pt block holds five 12pt slugs (got %d)", in_first);

    std::vector<Frame> small = {{0, 0, 150, 60}};
    Composition o = compose(story, small, fonts);
    CHECK(o.overset, "a too-small block oversets");
    CHECK(o.overset_byte > 0, "overset position is recorded");

    std::vector<Frame> none;
    CHECK(compose(story, none, fonts).overset, "no blocks: everything is overset");
}

static void test_breaks(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 12;
    ParaStyle ps;
    std::vector<Frame> wide = {{0, 0, 500, 500}};
    Composition c = compose(one_para("first line\nsecond line", cs, ps), wide, fonts);
    CHECK(c.lines.size() == 2, "forced line break makes two lines (%zu)", c.lines.size());
    if (c.lines.size() == 2)
        CHECK(!c.lines[0].para_end && c.lines[1].para_end, "break stays in the paragraph");

    std::vector<Frame> narrow = {{0, 0, 30, 500}};
    Composition w = compose(one_para("Supercalifragilistic", cs, ps), narrow, fonts);
    CHECK(w.lines.size() > 2, "an overlong word is split (%zu lines)", w.lines.size());
    size_t glyphs = 0;
    for (const ComposedLine &l : w.lines) {
        size_t n = 0;
        for (const GlyphRun &r : l.runs) n += r.glyphs.size();
        CHECK(n >= 1, "every split line keeps a glyph");
        glyphs += n;
    }
    CHECK(glyphs == std::strlen("Supercalifragilistic"), "no glyphs lost in the split (%zu)", glyphs);

    Story empty;
    empty.paragraphs.resize(3);
    Composition e = compose(empty, wide, fonts);
    CHECK(e.lines.size() == 3, "empty paragraphs still take a line each");
    if (!e.lines.empty())
        CHECK(std::fabs(e.lines[1].top - e.lines[0].top - 14.4f) < 0.01f,
              "empty line uses autoleading (120%% of 12pt)");

    /* Narrower than "page-layout" but wider than "page-" and "layout". */
    Composition hy = compose(one_para("page-layout", cs, ps), {{0, 0, 45, 500}}, fonts);
    CHECK(hy.lines.size() == 2, "hyphenated compound breaks once (%zu lines)", hy.lines.size());
    if (hy.lines.size() == 2) {
        const std::string t = "page-layout";
        CHECK(t.substr(hy.lines[0].byte_start, hy.lines[0].byte_end) == "page-" &&
              hy.lines[1].byte_start == 5,
              "break falls after the hyphen, not inside a word");
    }
}

static void test_edit(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 12;
    ParaStyle ps;

    /* Insert in the middle; the caret lands after the new text. */
    {
        Story s = one_para("Hello world", cs, ps);
        TextPos p = insert_text(s, {0, 5}, " brave new", style_at(s, {0, 5}));
        CHECK(paragraph_text(s.paragraphs[0]) == "Hello brave new world",
              "insert in the middle: %s", paragraph_text(s.paragraphs[0]).c_str());
        CHECK(p.para == 0 && p.byte == 15, "caret after the inserted text");
        CHECK(s.paragraphs[0].runs.size() == 1, "same-style insert merges into one run");
    }

    /* '\n' in inserted text splits paragraphs, which inherit the style. */
    {
        Story s = one_para("Hello world", cs, ps);
        ps.align = Align::Right;
        s.paragraphs[0].style = ps;
        TextPos p = insert_text(s, {0, 5}, "\n", style_at(s, {0, 5}));
        CHECK(s.paragraphs.size() == 2, "return splits the paragraph");
        CHECK(paragraph_text(s.paragraphs[0]) == "Hello" &&
              paragraph_text(s.paragraphs[1]) == " world", "split keeps both halves");
        CHECK(s.paragraphs[1].style.align == Align::Right, "new paragraph inherits the style");
        CHECK(p.para == 1 && p.byte == 0, "caret starts the new paragraph");
    }

    /* Erase across paragraphs joins them. */
    {
        Story s = one_para("first", cs, ps);
        s.paragraphs.push_back(s.paragraphs[0]);
        s.paragraphs[1].runs[0].text = "second";
        TextPos p = erase(s, {0, 3}, {1, 3});
        CHECK(s.paragraphs.size() == 1, "erase joins the paragraphs");
        CHECK(paragraph_text(s.paragraphs[0]) == "firond",
              "erase across paragraphs: %s", paragraph_text(s.paragraphs[0]).c_str());
        CHECK(p.para == 0 && p.byte == 3, "erase returns the start");
    }

    /* Restyle splits runs; restyling back merges them again. */
    {
        Story s = one_para("Hello world", cs, ps);
        restyle(s, {0, 6}, {0, 9}, [](CharStyle &c) { c.bold = true; });
        CHECK(s.paragraphs[0].runs.size() == 3, "restyle splits runs (%zu)",
              s.paragraphs[0].runs.size());
        CHECK(s.paragraphs[0].runs[1].text == "wor" && s.paragraphs[0].runs[1].style.bold,
              "the middle run took the style");
        restyle(s, {0, 0}, {0, 11}, [](CharStyle &c) { c.bold = false; });
        CHECK(s.paragraphs[0].runs.size() == 1, "equal neighbors merge again");
        CHECK(copy_text(s, {0, 0}, {0, 5}) == "Hello", "copy_text clips the range");
    }

    /* Grapheme steps never split a combining sequence. */
    {
        Story s = one_para("e\xCC\x81x y", cs, ps);   // e + U+0301, x, space, y
        TextPos p = next_grapheme(s, {0, 0});
        CHECK(p.byte == 3, "e + combining accent is one step (%u)", p.byte);
        CHECK(prev_grapheme(s, p).byte == 0, "and one step back");
        CHECK(next_grapheme(s, p).byte == 4, "then the plain letter");
        CHECK(prev_grapheme(s, {0, 0}).byte == 0, "start of story stays put");
        CHECK(next_grapheme(s, {0, 6}).byte == 6, "end of story stays put");
    }

    /* Word movement and double-click selection. */
    {
        Story s = one_para("foo bar baz", cs, ps);
        CHECK(next_word(s, {0, 0}).byte == 3, "next word ends after 'foo'");
        CHECK(next_word(s, {0, 3}).byte == 7, "over the space to 'bar'");
        CHECK(prev_word(s, {0, 11}).byte == 8, "prev word starts at 'baz'");
        auto w = word_at(s, {0, 5});
        CHECK(w.first.byte == 4 && w.second.byte == 7, "word_at finds 'bar'");
        w = word_at(s, {0, 3});
        CHECK(w.first.byte == 0 && w.second.byte == 3, "word_at after a word picks it");
    }

    /* Caret <-> hit testing round trips on composed lines. */
    {
        const std::string text =
            "Every line here is broken in points not pixels so the column "
            "breaks the same way at any zoom level you choose.";
        Story s = one_para(text, cs, ps);
        std::vector<Frame> col = {{40, 30, 180, 600}};
        Composition c = compose(s, col, fonts);
        CHECK(c.lines.size() > 3, "sample wraps (%zu lines)", c.lines.size());

        for (uint32_t byte : {(uint32_t) 0, (uint32_t) 17, (uint32_t) 42,
                              (uint32_t) (text.size() / 2), (uint32_t) text.size()}) {
            Caret at = caret_at(c, s, {0, byte});
            CHECK(at.valid(), "caret for byte %u is on a line", byte);
            if (!at.valid()) continue;
            const ComposedLine &l = c.lines[at.line];
            CHECK(at.top == l.top && at.bottom == l.top + l.leading, "caret spans the slug");
            TextPos back = pos_at_x(c, s, at.line, at.x);
            CHECK(back.para == 0 && back.byte == byte,
                  "byte %u: x -> position round trips (got %u)", byte, back.byte);
            float mid = l.top + l.leading * 0.5f;
            TextPos hit = hit_test(c, s, col, at.x + 0.01f, mid);
            CHECK(hit.para == 0 && hit.byte == byte,
                  "byte %u: page point -> position round trips (got %u)", byte, hit.byte);
        }

        /* Home/End: the end of a wrapped line stops before the space. */
        size_t first = 0;
        TextPos e = line_end(c, s, first);
        const ComposedLine &l0 = c.lines[first];
        CHECK(e.byte <= l0.byte_end && e.byte > l0.byte_start, "line end inside the line");
        CHECK(text[e.byte - 1] != ' ', "line end strips the hanging space");
        CHECK(line_start(c, first).byte == l0.byte_start, "line start is the line's first byte");

        /* A point below the text, past the right edge, lands at the end. */
        TextPos past = hit_test(c, s, col, 10000, 30 + 590);
        CHECK(past.byte == text.size(), "click past the text hits the end (%u)", past.byte);
    }
}

static void test_deterministic(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    for (const TextFlow &f : doc.flows) {
        Composition a = compose(f.story, f.frames, fonts);
        Composition b = compose(f.story, f.frames, fonts);
        bool same = a.lines.size() == b.lines.size();
        for (size_t i = 0; same && i < a.lines.size(); ++i)
            for (size_t r = 0; same && r < a.lines[i].runs.size(); ++r)
                for (size_t g = 0; same && g < a.lines[i].runs[r].glyphs.size(); ++g) {
                    const PlacedGlyph &x = a.lines[i].runs[r].glyphs[g];
                    const PlacedGlyph &y = b.lines[i].runs[r].glyphs[g];
                    same = x.gid == y.gid && x.x == y.x && x.y == y.y;
                }
        CHECK(same, "composing twice gives identical glyph positions");
    }
}

static void dump_sample(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    for (size_t fi = 0; fi < doc.flows.size(); ++fi) {
        const TextFlow &flow = doc.flows[fi];
        Composition c = compose(flow.story, flow.frames, fonts);
        std::printf("\nflow %zu: %zu lines, %s, %.3f ms\n", fi, c.lines.size(),
                    c.overset ? "OVERSET" : "fits", c.compose_ms);
        for (const ComposedLine &l : c.lines) {
            std::string t = paragraph_text(flow.story.paragraphs[l.para])
                                .substr(l.byte_start, l.byte_end - l.byte_start);
            std::printf("  b%zu p%-2zu base %6.2f  x %6.2f..%6.2f/%6.2f %s%s| %s\n",
                        l.frame, l.para, l.baseline, l.left, l.x_end, l.left + l.width,
                        l.loose ? "L" : " ", l.tight ? "T" : " ", t.c_str());
        }
    }
}

int main(int argc, char **argv) {
    bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    FontLibrary fonts;
    register_default_fonts(fonts);
    if (fonts.empty()) {
        std::printf("no fonts found\n");
        return 1;
    }
    for (const char *fam : {"Serif", "Sans", "Display"})
        if (const Font *f = fonts.find(fam, false, false))
            std::printf("  %-7s -> %s (upem %d)\n", fam, f->name().c_str(), f->units_per_em());

    test_kerning(fonts);
    test_ligatures(fonts);
    test_alignment(fonts);
    test_threading(fonts);
    test_breaks(fonts);
    test_edit(fonts);
    test_deterministic(fonts);
    if (verbose)
        dump_sample(fonts);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
