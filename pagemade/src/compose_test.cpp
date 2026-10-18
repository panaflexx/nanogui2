/*
 * pagemade/compose_test.cpp — headless checks for the composer.
 *
 *   pagemade_compose_test        run the checks, exit 1 on any failure
 *   pagemade_compose_test -v     also dump the sample document's lines
 */
#include "composer/composer.h"
#include "composer/hyphenator.h"
#include "composer/edit.h"
#include "default_fonts.h"
#include "font_menu.h"
#include "system_fonts.h"
#include "drawlist.h"
#include "geometry.h"
#include "image.h"
#include "image_color.h"
#include "page.h"
#include "setup_adjust.h"
#include "swatch_library.h"
#include "pdf.h"
#include "docfile.h"
#include "uri.h"
#include "vector.h"
#include "nanovg_exif.h"
#include "bg_worker.h"
#ifdef PAGEMADE_HAVE_HEIF
#include <libheif/heif.h>
#endif

#include <miniz.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../../ext/glfw/deps/stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nanogui/fscompat.h>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef PAGEMADE_DATA_DIR
#define PAGEMADE_DATA_DIR "pagemade/resources"
#endif

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

/* Line spacing multiplies the slug, auto or fixed leading alike. */
static void test_line_spacing(const FontLibrary &fonts) {
    CharStyle cs;
    ParaStyle ps;
    const std::vector<Frame> wide = {{0, 0, 200, 1000}};
    auto pitch = [&](float spacing, float leading) {
        CharStyle c = cs;
        c.leading = leading;
        ParaStyle p = ps;
        p.line_spacing = spacing;
        Composition comp = compose(one_para(kLong, c, p), wide, fonts);
        if (comp.lines.size() < 3)
            return -1.f;
        const ComposedLine &l = comp.lines[1];
        CHECK(std::fabs((l.baseline - l.top) - l.leading * (2.f / 3.f)) < 1e-3f,
              "baseline two thirds down the spaced slug");
        return comp.lines[2].top - comp.lines[1].top;
    };
    CHECK(std::fabs(pitch(1.f, 0) - 14.4f) < 0.01f, "single spacing: auto leading (%g)", pitch(1.f, 0));
    CHECK(std::fabs(pitch(2.f, 0) - 28.8f) < 0.01f, "double spacing doubles auto leading (%g)",
          pitch(2.f, 0));
    CHECK(std::fabs(pitch(0.5f, 0) - 7.2f) < 0.01f, "half spacing (%g)", pitch(0.5f, 0));
    CHECK(std::fabs(pitch(1.5f, 20.f) - 30.f) < 0.01f, "1.5 times fixed 20pt leading (%g)",
          pitch(1.5f, 20.f));
    CHECK(std::fabs(pitch(9.f, 0) - 14.4f * kMaxLineSpacing) < 0.01f, "clamped to %g",
          kMaxLineSpacing);

    /* A 100pt frame holds three double-spaced 28.8pt lines, then oversets. */
    ParaStyle dbl = ps;
    dbl.line_spacing = 2.f;
    Composition c = compose(one_para(kLong, cs, dbl), {{0, 0, 200, 100}}, fonts);
    CHECK(c.lines.size() == 3 && c.overset, "double spacing fits 3 lines in 100pt (%zu)",
          c.lines.size());

    /* An empty paragraph's line is spaced too. */
    Story empty;
    empty.paragraphs.resize(2);
    empty.paragraphs[0].style.line_spacing = 2.f;
    Composition e = compose(empty, wide, fonts);
    CHECK(e.lines.size() == 2 && std::fabs(e.lines[1].top - 28.8f) < 0.01f,
          "empty paragraph line is double spaced");
}

/* The Paragraph panel's attributes: last-line alignment of justified
 * text, extra spacing and the last-line indent. */
static void test_paragraph_attributes(const FontLibrary &fonts) {
    auto close = [](float x, float y) { return std::fabs(x - y) < 1e-3f; };
    CharStyle cs;
    const std::vector<Frame> wide = {{0, 0, 200, 1000}};
    auto last_line = [&](Align a, float last_indent = 0) {
        ParaStyle ps;
        ps.align = a;
        ps.last_indent = last_indent;
        Composition c = compose(one_para(kLong, cs, ps), wide, fonts);
        return c.lines.empty() ? ComposedLine() : c.lines.back();
    };
    const ComposedLine l = last_line(Align::Justify);
    const ComposedLine c = last_line(Align::JustifyCenter);
    const ComposedLine r = last_line(Align::JustifyRight);
    const ComposedLine f = last_line(Align::ForceJustify);
    CHECK(close(l.x_start, 0), "justify: last line starts left (%g)", l.x_start);
    CHECK(std::fabs((c.x_start + c.x_end) * 0.5f - 100.f) < 0.05f,
          "justify center: last line centered (%g..%g)", c.x_start, c.x_end);
    CHECK(std::fabs(r.x_end - 200.f) < 0.05f, "justify right: last line ends right (%g)", r.x_end);
    CHECK(std::fabs(f.x_end - 200.f) < 0.05f && close(f.x_start, 0), "force justify: full width");
    CHECK(std::fabs(r.x_end - r.x_start - (l.x_end - l.x_start)) < 0.05f,
          "the last line keeps its natural width (%g vs %g)", r.x_end - r.x_start,
          l.x_end - l.x_start);
    {
        ParaStyle ps;
        ps.align = Align::JustifyCenter;
        Composition jc = compose(one_para(kLong, cs, ps), wide, fonts);
        ps.align = Align::Justify;
        Composition j = compose(one_para(kLong, cs, ps), wide, fonts);
        bool same = jc.lines.size() == j.lines.size();
        for (size_t i = 0; same && i + 1 < j.lines.size(); ++i)
            same = close(jc.lines[i].x_start, j.lines[i].x_start) &&
                   close(jc.lines[i].x_end, j.lines[i].x_end);
        CHECK(same, "justify center sets every other line like justify");
    }

    /* Last-line indent: from the left indent; a last line too long for it
     * gives up words to a new last line. */
    const ComposedLine li = last_line(Align::Left, 30);
    CHECK(std::fabs(li.x_start - 30.f) < 0.01f && std::fabs(li.left - 30.f) < 0.01f,
          "last line indented 30pt (%g)", li.x_start);
    {
        ParaStyle ps;
        Composition plain = compose(one_para(kLong, cs, ps), wide, fonts);
        ps.last_indent = 190;            // leaves 10pt: no last line fits
        Composition in = compose(one_para(kLong, cs, ps), wide, fonts);
        CHECK(in.lines.size() > plain.lines.size(),
              "a long last line breaks again for the indent (%zu vs %zu lines)",
              in.lines.size(), plain.lines.size());
        bool only_last = true;
        for (size_t i = 0; i + 1 < in.lines.size(); ++i)
            only_last &= close(in.lines[i].left, 0);
        CHECK(only_last, "only the last line is indented");
    }

    /* Extra spacing adds points after line spacing; it may be negative. */
    auto pitch = [&](float spacing, float extra) {
        ParaStyle ps;
        ps.line_spacing = spacing;
        ps.extra_spacing = extra;
        Composition comp = compose(one_para(kLong, cs, ps), wide, fonts);
        return comp.lines.size() < 2 ? -1.f : comp.lines[1].top - comp.lines[0].top;
    };
    CHECK(std::fabs(pitch(1, 3) - 17.4f) < 0.01f, "extra 3pt (%g)", pitch(1, 3));
    CHECK(std::fabs(pitch(2, -4) - 24.8f) < 0.01f, "double, minus 4pt (%g)", pitch(2, -4));
    CHECK(pitch(1, -100) == 0.f, "spacing never goes below zero (%g)", pitch(1, -100));
}

/* Drop caps: placed on the text's cap height and line N's baseline, the
 * lines they reach moved over, sizable, and still editable text. */
/* Keep options and frame breaks: two text blocks of five 14.4pt lines. */
static void test_keeps(const FontLibrary &fonts) {
    CharStyle cs;
    const std::vector<Frame> two = {{0, 0, 200, 72}, {0, 100, 200, 72}};
    auto para = [&](const std::string &t, const ParaStyle &ps = ParaStyle()) {
        Paragraph p;
        p.style = ps;
        p.runs.push_back({cs, t});
        return p;
    };
    /* (frame, lines) per paragraph, in order of blocks. */
    auto layout = [&](const std::vector<Paragraph> &ps) {
        Story s;
        s.paragraphs = ps;
        const Composition c = compose(s, two, fonts);
        std::vector<std::vector<std::pair<size_t, int>>> out(ps.size());
        for (const ComposedLine &l : c.lines) {
            auto &b = out[l.para];
            if (b.empty() || b.back().first != l.frame)
                b.push_back({l.frame, 1});
            else
                ++b.back().second;
        }
        return out;
    };
    const Paragraph three = para("a\nb\nc");

    {   /* Without rules: 3 + 2 at the bottom, 1 carried over. */
        const auto l = layout({three, para("x\ny\nz")});
        CHECK(l[1].size() == 2 && l[1][0].second == 2 && l[1][1].second == 1,
              "no rules: two lines, then a widow");
    }
    {   ParaStyle ps;
        ps.widows = 2;
        const auto l = layout({three, para("x\ny\nz", ps)});
        CHECK(l[1].size() == 2 && l[1][0] == std::make_pair(size_t(0), 1) &&
              l[1][1] == std::make_pair(size_t(1), 2), "widows 2: one line carried with it");
        ps.orphans = 2;
        const auto m = layout({three, para("x\ny\nz", ps)});
        CHECK(m[1].size() == 1 && m[1][0].first == 1, "widows and orphans 2: all moves on");
    }
    {   ParaStyle ps;
        ps.orphans = 3;
        const auto l = layout({three, para(kLong, ps)});
        CHECK(!l[1].empty() && l[1][0].first == 1, "orphans 3: two lines at the bottom move on");
        ps.orphans = 2;
        const auto m = layout({three, para(kLong, ps)});
        CHECK(!m[1].empty() && m[1][0] == std::make_pair(size_t(0), 2), "orphans 2: two is enough");
    }
    {   ParaStyle ps;
        ps.keep_together = true;
        const auto l = layout({three, para("x\ny\nz", ps)});
        CHECK(l[1].size() == 1 && l[1][0] == std::make_pair(size_t(1), 3), "keep together");
        ps.space_before = 30;
        const auto m = layout({three, para("x\ny\nz", ps)});
        CHECK(m[1].size() == 1 && m[1][0].first == 1, "space before is dropped at the top");
    }
    {   /* A heading at the bottom of a block goes with the paragraph after it. */
        ParaStyle head;
        head.keep_with_next = true;
        const auto l = layout({para("one"), para("h\ne\na\nd", head), para("body")});
        CHECK(l[1].size() == 1 && l[1][0].first == 1 && l[2][0].first == 1,
              "keep with next moves the heading");
        const auto m = layout({para("one"), para("h", head), para("s\nu\nb", head),
                               para("body")});
        CHECK(m[1][0].first == 1 && m[2][0].first == 1 && m[3][0].first == 1,
              "a chain of headings moves together");
        const auto k = layout({para("h\ne\na\nd\ns", head), para("body")});
        CHECK(k[0][0].first == 0, "a heading already at the top stays");
    }
    {   ParaStyle ps;
        ps.start_new_frame = true;
        const auto l = layout({para("one"), para("two", ps)});
        CHECK(l[1][0].first == 1, "start in the next text block");
        const auto m = layout({para("two", ps)});
        CHECK(m[0][0].first == 0, "...unless it already starts at the top of one");
    }
    {   const auto l = layout({para("one\x0Ctwo"), para("after")});
        CHECK(l[0].size() == 2 && l[0][0].first == 0 && l[0][1].first == 1 &&
              l[1][0].first == 1, "a frame break carries the text to the next block");
    }
}

/* Smart quotes, fixed spaces, the non-breaking hyphen, and No break. */
static void test_special_chars(const FontLibrary &fonts) {
    CHECK(smart_quote('"', 0) == 0x201C && smart_quote('"', ' ') == 0x201C &&
          smart_quote('"', 'o') == 0x201D && smart_quote('"', '.') == 0x201D,
          "double quotes open after a space or at the start, close after text");
    CHECK(smart_quote('\'', 'n') == 0x2019 && smart_quote('\'', '(') == 0x2018 &&
          smart_quote('\'', 0x201C) == 0x2018 && smart_quote('\'', 0x2014) == 0x2018,
          "single quotes: apostrophe after a letter, opening after ( “ or a dash");
    CHECK(smart_quote('x', ' ') == 'x', "other characters pass through");
    {
        Story s = one_para("ab\u00E9", CharStyle(), ParaStyle());
        CHECK(char_before(s, {0, 0}) == 0 && char_before(s, {0, 2}) == 'b' &&
              char_before(s, {0, 4}) == 0xE9, "char_before");
    }

    CharStyle cs;
    ParaStyle ps;
    const std::vector<Frame> wide = {{0, 0, 2000, 200}};
    auto width = [&](const std::string &t) {
        Composition c = compose(one_para(t, cs, ps), wide, fonts);
        return c.lines.empty() ? 0.f : c.lines[0].x_end - c.lines[0].x_start;
    };
    /* An em space is an em wide whether or not the face has the glyph. */
    const float em = width("a\u2003b") - width("ab");
    CHECK(std::fabs(em - cs.size) < 0.6f, "em space ~ one em (%g)", em);
    const float hair = width("a\u200Ab") - width("ab");
    CHECK(hair > 0.f && hair < cs.size * 0.2f, "hair space is narrow (%g)", hair);
    CHECK(std::fabs(width("a\u2011b") - width("a-b")) < 0.01f,
          "a non-breaking hyphen looks like a hyphen");

    /* Justified: word spaces stretch, the em space keeps its width. */
    {
        ParaStyle j;
        j.align = Align::Justify;
        Composition c = compose(one_para("aa bb\u2003cc dd ee ff gg hh ii jj kk ll mm nn oo pp qq",
                                         cs, j), {{0, 0, 150, 500}}, fonts);
        float em_adv = -1;
        for (const GlyphRun &r : c.lines.empty() ? std::vector<GlyphRun>() : c.lines[0].runs)
            for (size_t i = 0; i + 1 < r.glyphs.size(); ++i)
                if (r.glyphs[i].cluster == 5)
                    em_adv = r.glyphs[i + 1].x - r.glyphs[i].x;
        CHECK(c.lines.size() > 1 && std::fabs(em_adv - em) < 0.6f,
              "the em space is not stretched (%g vs %g)", em_adv, em);
    }

    /* No break: a phrase that would break stays on one line. */
    {
        CharStyle nb = cs;
        nb.no_break = true;
        Paragraph p;
        p.runs = {{cs, "aaaa bbbb cccc "}, {nb, "New York City"}, {cs, " dddd"}};
        Story st;
        st.paragraphs = {p};
        const std::string text = paragraph_text(p);
        const uint32_t a = (uint32_t) text.find("New"), b = a + 13;
        for (float w : {80.f, 110.f, 140.f}) {
            Composition c = compose(st, {{0, 0, w, 500}}, fonts);
            bool split = false;
            for (const ComposedLine &l : c.lines)
                split |= l.byte_end > a && l.byte_end < b;
            CHECK(!split, "No break keeps \"New York City\" together at %gpt", w);
        }
        /* Without it the phrase breaks somewhere at one of those widths. */
        p.runs[1].style.no_break = false;
        st.paragraphs = {p};
        bool any = false;
        for (float w : {80.f, 110.f, 140.f}) {
            Composition c = compose(st, {{0, 0, w, 500}}, fonts);
            for (const ComposedLine &l : c.lines)
                any |= l.byte_end > a && l.byte_end < b;
        }
        CHECK(any, "without No break it does break");
    }
}

/* Bullets and numbering. */
static void test_lists(const FontLibrary &fonts) {
    ParaStyle n;
    n.list = ListKind::Number;
    CHECK(list_marker(n, 3) == "3.", "arabic");
    n.number_format = NumberFormat::LowerAlpha;
    CHECK(list_marker(n, 1) == "a." && list_marker(n, 26) == "z." && list_marker(n, 28) == "ab.",
          "letters (%s)", list_marker(n, 28).c_str());
    n.number_format = NumberFormat::UpperRoman;
    n.number_suffix = ")";
    CHECK(list_marker(n, 1994) == "MCMXCIV)", "roman (%s)", list_marker(n, 1994).c_str());
    n.number_format = NumberFormat::LowerRoman;
    CHECK(list_marker(n, 4) == "iv)", "lower roman");
    ParaStyle b;
    b.list = ListKind::Bullet;
    CHECK(list_marker(b, 7) == "\u2022" && list_marker(ParaStyle(), 1).empty(), "bullet, none");

    /* Counting: consecutive numbered paragraphs, a restart, a break. */
    {
        ParaStyle num;
        num.list = ListKind::Number;
        ParaStyle again = num;
        again.list_start = 10;
        Story s;
        const ParaStyle plain;
        for (const ParaStyle *ps : std::initializer_list<const ParaStyle *>{&num, &num, &num, &again, &num, &plain, &num})
            s.paragraphs.push_back(one_para("x", CharStyle(), *ps).paragraphs[0]);
        const std::vector<int> got = list_numbers(s);
        const std::vector<int> want = {1, 2, 3, 10, 11, 0, 1};
        CHECK(got == want, "numbers count, restart and break");
    }

    /* The marker hangs at the first-line indent; the text sits at the
     * left indent; the caret skips the marker. */
    CharStyle cs;
    ParaStyle li;
    li.list = ListKind::Number;
    li.left_indent = 18;
    li.first_indent = -18;
    Story st = one_para("Item text that runs onto a second line of the list", cs, li);
    const Composition c = compose(st, {{0, 0, 150, 500}}, fonts);
    CHECK(c.lines.size() >= 2, "two lines (%zu)", c.lines.size());
    if (c.lines.size() >= 2 && !c.lines[0].runs.empty()) {
        const GlyphRun &m = c.lines[0].runs.front();
        CHECK(!m.glyphs.empty() && (m.glyphs[0].flags & PlacedGlyph::Inserted) &&
              std::fabs(m.glyphs[0].x) < 0.5f, "the marker at the hanging indent (%g)",
              m.glyphs.empty() ? -1.f : m.glyphs[0].x);
        CHECK(std::fabs(c.lines[0].left - 18.f) < 0.01f && std::fabs(c.lines[1].left - 18.f) < 0.01f,
              "text at the left indent on every line");
        const Caret k = caret_at(c, st, {0, 0});
        CHECK(k.valid() && std::fabs(k.x - 18.f) < 0.5f, "the caret starts at the text (%g)", k.x);
    }
    /* No hanging indent: the text follows the marker. */
    {
        ParaStyle flat;
        flat.list = ListKind::Bullet;
        const Composition f = compose(one_para("Item", cs, flat), {{0, 0, 150, 500}}, fonts);
        CHECK(!f.lines.empty() && f.lines[0].left > 4.f, "text after a flush marker (%g)",
              f.lines.empty() ? -1.f : f.lines[0].left);
    }
    /* A drop cap takes the marker's place. */
    {
        ParaStyle both = li;
        both.drop_lines = 2;
        const Composition d = compose(one_para(kLong, cs, both), {{0, 0, 200, 500}}, fonts);
        bool inserted = false;
        if (!d.lines.empty())
            for (const PlacedGlyph &g : d.lines[0].runs.front().glyphs)
                inserted |= (g.flags & PlacedGlyph::Inserted) != 0;
        CHECK(!inserted, "no marker with a drop cap");
    }
}

static bool near_f(float a, float b) { return std::fabs(a - b) < 1e-3f; }

static void test_drop_caps(const FontLibrary &fonts) {
    CharStyle cs;
    const std::vector<Frame> wide = {{0, 0, 200, 1000}};
    const std::string text = "Hello there. " + kLong;
    auto make = [&](int lines, float scale = 100, int chars = 1,
                    const std::string &t = std::string()) {
        ParaStyle ps;
        ps.drop_lines = lines;
        ps.drop_scale = scale;
        ps.drop_chars = chars;
        return compose(one_para(t.empty() ? text : t, cs, ps), wide, fonts);
    };
    auto beside = [](const Composition &c) {
        size_t n = 0;
        while (n < c.lines.size() && c.lines[n].left > 0.5f)
            ++n;
        return n;
    };

    const Composition c = make(3);
    CHECK(c.lines.size() > 4, "a long paragraph (%zu lines)", c.lines.size());
    if (c.lines.size() < 4 || c.lines[0].runs.empty())
        return;
    const GlyphRun &cap = c.lines[0].runs.front();
    CHECK(cap.glyphs.size() == 1 && cap.glyphs[0].cluster == 0 && cap.size > cs.size * 2.5f,
          "the first run is the enlarged cap (%zu glyphs, %.1fpt)", cap.glyphs.size(), cap.size);
    CHECK(c.lines[0].byte_start == 0, "the first line holds the cap's characters");
    CHECK(beside(c) == 3, "three lines move over (%zu)", beside(c));
    CHECK(near_f(c.lines[0].left, c.lines[2].left) && c.lines[3].left < 0.01f,
          "the same indent, then the full measure");
    const PlacedGlyph &h = cap.glyphs[0];
    CHECK(std::fabs(h.y - c.lines[2].baseline) < 0.01f, "the cap sits on line 3's baseline");
    {
        const Font *f = cap.font;
        float x0, y0, x1, y1;
        f->glyph_bounds(h.gid, x0, y0, x1, y1);
        const float em = cap.size / (float) f->units_per_em();
        const float text_cap = f->cap_height() / (float) f->units_per_em() * cs.size;
        CHECK(std::fabs((h.y - y1 * em) - (c.lines[0].baseline - text_cap)) < 0.2f,
              "its top is the first line's cap height (%g vs %g)", h.y - y1 * em,
              c.lines[0].baseline - text_cap);
        CHECK(std::fabs(h.x + x0 * em) < 0.01f, "its ink starts on the left edge (%g)",
              h.x + x0 * em);
        CHECK(c.lines[0].left > h.x + x1 * em, "the text clears its ink");
    }
    /* The rest of the first word follows the cap on the first line. */
    CHECK(c.lines[0].runs.size() > 1 && c.lines[0].runs[1].glyphs.front().cluster == 1,
          "the word goes on after the cap");

    CHECK(beside(make(3, 160)) > 3, "a larger cap moves more lines over (%zu)",
          beside(make(3, 160)));
    CHECK(beside(make(3, 50)) < 3, "a smaller cap fewer (%zu)", beside(make(3, 50)));
    CHECK(beside(make(0)) == 0, "drop_lines 0: no drop cap");
    const Composition two = make(2);
    CHECK(beside(two) == 2 && two.lines[0].runs.front().size < cap.size,
          "a two-line cap is smaller");

    /* Several characters; a space ends the cap, and the word after it
     * starts the first line. */
    const Composition three = make(3, 100, 5, "The quick brown fox " + kLong);
    if (!three.lines.empty() && three.lines[0].runs.size() > 1) {
        CHECK(three.lines[0].runs.front().glyphs.size() == 3, "\"The\" is the cap (%zu)",
              three.lines[0].runs.front().glyphs.size());
        CHECK(three.lines[0].runs[1].glyphs.front().cluster == 4,
              "the first line starts at \"quick\"");
    }

    /* A paragraph shorter than its cap pushes the next one below it. */
    {
        ParaStyle ps;
        ps.drop_lines = 3;
        Story s = one_para("Hi", cs, ps);
        s.paragraphs.push_back(one_para("Next", cs, ParaStyle()).paragraphs[0]);
        const Composition sc = compose(s, wide, fonts);
        CHECK(sc.lines.size() == 2, "two lines (%zu)", sc.lines.size());
        if (sc.lines.size() == 2) {
            const PlacedGlyph &g = sc.lines[0].runs.front().glyphs[0];
            CHECK(sc.lines[1].top >= g.y - 0.01f && sc.lines[1].left < 0.01f,
                  "the next paragraph starts below the cap, at the margin");
        }
    }

    /* The cap is text: the caret before it is at its left. */
    {
        Story s = one_para(text, cs, [] { ParaStyle p; p.drop_lines = 3; return p; }());
        const Composition sc = compose(s, wide, fonts);
        const Caret k = caret_at(sc, s, {0, 0});
        CHECK(k.valid() && k.line == 0 && std::fabs(k.x - sc.lines[0].runs[0].glyphs[0].x) < 0.01f,
              "a caret at the start sits before the cap");
        const Caret k1 = caret_at(sc, s, {0, 1});
        CHECK(k1.valid() && k1.x > sc.lines[0].left - 0.5f, "after the cap, at the text");
    }
}

/* Named paragraph styles. */
static void test_styles() {
    const std::vector<StyleDef> defs = default_styles();
    std::vector<std::string> names;
    for (const StyleDef &d : defs) {
        names.push_back(d.name);
        CHECK(d.para.name == d.name, "%s: the paragraph carries the name", d.name.c_str());
    }
    for (const char *want : {"Normal", "Body text", "Headline", "Subhead 1", "Subhead 2",
                             "Caption", "Hanging indent", "Pull quote"})
        CHECK(std::find(names.begin(), names.end(), want) != names.end(), "default style %s", want);
    CHECK(names.front() == "Normal", "Normal comes first");
    CHECK(std::find(names.begin(), names.end(), "Drop cap") != names.end(), "a Drop cap style");

    PageDoc doc;
    const StyleDef *body = doc.find_style("Body text");
    const StyleDef *head = doc.find_style("Headline");
    CHECK(body && head && !doc.find_style("Nope"), "find_style");
    if (!body || !head)
        return;

    /* Body text with an italic word, set as a headline: the italic stays
     * emphasis, the rest takes the headline's type; color is kept. */
    CharStyle plain;
    plain.family = "Sans";
    plain.size = 9;
    plain.underline = true;
    plain.color = {1, 0, 0, 1};
    CharStyle em = plain;
    em.italic = true;
    Paragraph p;
    p.style.align = Align::Right;
    p.style.left_indent = 7;
    p.runs = {{plain, "The quick "}, {em, "brown"}, {plain, " fox jumps over the dog"}};
    CHECK(base_run(p) == &p.runs[2], "the longest run stands for the paragraph");
    apply_style(p, *head);
    CHECK(p.style.name == "Headline" && p.style.align == head->para.align &&
          p.style.left_indent == head->para.left_indent, "paragraph attributes replaced");
    CHECK(p.runs[0].style.family == head->type.family && p.runs[0].style.size == head->type.size &&
          p.runs[0].style.bold == head->type.bold && !p.runs[0].style.italic,
          "plain runs take the style's type");
    CHECK(p.runs[1].style.italic != head->type.italic && p.runs[1].style.bold == head->type.bold,
          "the italic word stays emphasized");
    CHECK(p.runs[0].style.underline && p.runs[0].style.color.r == 1.f,
          "underline and color are kept");
    CHECK(!style_overridden(p, *head), "a fresh paragraph has no overrides");
    p.style.space_after = 99;
    CHECK(style_overridden(p, *head), "a changed attribute is an override");
    p.style.space_after = head->para.space_after;
    p.runs[2].style.size = 50;
    CHECK(style_overridden(p, *head), "changed type is an override");

    /* Character styles set only their fields; paragraph styles keep them. */
    {
        const std::vector<CharStyleDef> cdefs = default_char_styles();
        const CharStyleDef *em = nullptr, *accent = nullptr, *sc = nullptr;
        for (const CharStyleDef &d : cdefs) {
            if (d.name == "Emphasis") em = &d;
            if (d.name == "Accent") accent = &d;
            if (d.name == "Small caps") sc = &d;
        }
        CHECK(em && accent && sc, "default character styles");
        if (em && accent && sc) {
            CharStyle c;
            c.family = "Sans";
            c.size = 9;
            c.face = "Light";
            apply_char_style(c, *em);
            CHECK(c.italic && c.family == "Sans" && c.size == 9 && c.face.empty() &&
                  c.char_style == "Emphasis", "Emphasis sets italic only");
            CHECK(!char_style_overridden(c, *em), "fresh: no override");
            c.size = 20;
            CHECK(!char_style_overridden(c, *em), "a field the style leaves alone is no override");
            c.italic = false;
            CHECK(char_style_overridden(c, *em), "its own field is");
            apply_char_style(c, *accent);
            CHECK(c.swatch == accent->type.swatch && c.bold && c.char_style == "Accent",
                  "Accent sets a swatch");

            /* A style made from a red italic word over plain text sets
             * just those two things. */
            CharStyle base;
            CharStyle word = base;
            word.italic = true;
            word.swatch = 8;
            word.tint = 60;
            const CharStyleDef mine = char_style_from("Mine", word, base);
            CHECK(mine.fields == (kCharItalic | kCharColor), "only what differs (%x)", mine.fields);
            CHECK(char_style_from("Plain", base, base).fields != 0, "plain text: the whole type");

            /* A paragraph style keeps a run's character style. */
            Paragraph q;
            CharStyle tagged;
            apply_char_style(tagged, *sc);
            q.runs = {{CharStyle(), "plain words here "}, {tagged, "SMALL"}};
            apply_style(q, *body, cdefs);
            CHECK(q.runs[1].style.caps == Caps::Small && q.runs[1].style.char_style == "Small caps" &&
                  q.runs[1].style.size == body->type.size, "Small caps survives Body text");
        }
    }

    StyleDef made = style_from("Mine", p);
    CHECK(made.name == "Mine" && made.para.name == "Mine" && made.type.size == 50 &&
          !made.type.underline && made.type.color == Color(),
          "style_from takes the base run's type, without ornament");
}

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

    /* A break at the end of the paragraph opens a line for the caret. */
    {
        Story s = one_para("hello\n", cs, ps);
        Composition t = compose(s, wide, fonts);
        CHECK(t.lines.size() == 2, "a trailing newline opens a line (%zu)", t.lines.size());
        if (t.lines.size() == 2) {
            CHECK(!t.lines[0].para_end && t.lines[1].para_end, "the new line ends the paragraph");
            CHECK(t.lines[1].byte_start == 6 && t.lines[1].byte_end == 6, "the new line is empty");
            Caret at = caret_at(t, s, {0, 6});
            CHECK(at.valid() && at.line == 1, "the caret sits on the new line");
        }
        Story u = one_para("hello\xE2\x80\xA8", cs, ps);
        Composition cu = compose(u, wide, fonts);
        CHECK(cu.lines.size() == 2, "a trailing Shift+Return opens a line (%zu)", cu.lines.size());
        if (cu.lines.size() == 2) {
            Caret at = caret_at(cu, u, {0, 8});
            CHECK(at.valid() && at.line == 1, "the caret sits after Shift+Return");
        }
        Story twice = one_para("hello\n\n", cs, ps);
        CHECK(compose(twice, wide, fonts).lines.size() == 3,
              "two trailing breaks open two lines");
    }

    /* End stops before every break the composer hangs, including VT, FF and NEL. */
    {
        /* Two blocks: a form feed also carries the text to the next one. */
        std::vector<Frame> blocks = wide;
        blocks.push_back({wide[0].x, wide[0].y + wide[0].h + 10, wide[0].w, wide[0].h});
        auto end_before = [&](const char *text, const char *label) {
            Story s = one_para(text, cs, ps);
            Composition t = compose(s, blocks, fonts);
            CHECK(t.lines.size() == 2, "%s breaks the line (%zu)", label, t.lines.size());
            if (t.lines.size() < 1)
                return;
            TextPos e = line_end(t, s, 0);
            CHECK(e.byte == 5, "End stops before a trailing %s (%u)", label, e.byte);
        };
        end_before("hello\vworld", "VT");
        end_before("hello\fworld", "FF");
        end_before("hello\xC2\x85world", "NEL");
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

/* Alignment is a paragraph attribute. A range that is only some of a
 * paragraph's lines is split off so the rest keeps its alignment, without
 * the indent and spacing a typed Return would add. */
static void test_align_ranges(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 10;
    ParaStyle ps;
    ps.align = Align::Justify;
    ps.name = "Body text";
    ps.first_indent = 12.f;
    ps.space_before = 4.f;
    ps.space_after = 6.f;

    {
        Story s = one_para("abcdefghijklmnopqrstuvwxyz", cs, ps);
        TextPos caret{0, 15};
        set_align_ranges(s, {{{0, 10}, {0, 20}}}, Align::Center, &caret, nullptr);
        CHECK(s.paragraphs.size() == 3, "middle span splits into three (%zu)", s.paragraphs.size());
        if (s.paragraphs.size() == 3) {
            CHECK(paragraph_text(s.paragraphs[0]) == "abcdefghij", "head text");
            CHECK(paragraph_text(s.paragraphs[1]) == "klmnopqrst", "middle text");
            CHECK(paragraph_text(s.paragraphs[2]) == "uvwxyz", "tail text");
            CHECK(s.paragraphs[0].style.align == Align::Justify, "head stays justified");
            CHECK(s.paragraphs[1].style.align == Align::Center, "middle is centered");
            CHECK(s.paragraphs[2].style.align == Align::Justify, "tail stays justified");
            CHECK(s.paragraphs[0].style.first_indent == 12.f, "head keeps the first indent");
            CHECK(s.paragraphs[0].style.space_before == 4.f, "head keeps space before");
            CHECK(s.paragraphs[0].style.space_after == 0.f, "the artificial break has no space after");
            CHECK(s.paragraphs[1].style.first_indent == 0.f, "a continuation is not a first line");
            CHECK(s.paragraphs[1].style.space_before == 0.f &&
                  s.paragraphs[1].style.space_after == 0.f, "the middle has no paragraph spacing");
            CHECK(s.paragraphs[1].style.name == "Body text", "the split keeps the style name");
            CHECK(s.paragraphs[2].style.first_indent == 0.f &&
                  s.paragraphs[2].style.space_before == 0.f, "the tail is a continuation");
            CHECK(s.paragraphs[2].style.space_after == 6.f, "space after stays at the real end");
            CHECK(caret.para == 1 && caret.byte == 5, "caret follows into the new paragraph (%zu,%u)",
                  caret.para, caret.byte);
        }
    }

    {
        Story s = one_para("abcdefghijklmnopqrstuvwxyz", cs, ps);
        set_align_ranges(s, {{{0, 0}, {0, 26}}}, Align::Center);
        CHECK(s.paragraphs.size() == 1, "a whole paragraph is not split");
        CHECK(s.paragraphs[0].style.align == Align::Center, "a whole paragraph changes in place");
        CHECK(s.paragraphs[0].style.first_indent == 12.f &&
              s.paragraphs[0].style.space_after == 6.f, "a whole paragraph keeps indent and spacing");
    }

    {
        Story s = one_para("abcdefghijklmnopqrstuvwxyz", cs, ps);
        set_align_ranges(s, {{{0, 0}, {0, 10}}, {{0, 10}, {0, 26}}}, Align::Right);
        CHECK(s.paragraphs.size() == 1, "adjacent spans that cover the paragraph merge");
        CHECK(s.paragraphs[0].style.align == Align::Right, "the merged span changes alignment");
    }

    {
        Story s = one_para("abcdefghijklmnopqrstuvwxyz", cs, ps);
        set_align_ranges(s, {{{0, 10}, {0, 20}}}, Align::Justify);
        CHECK(s.paragraphs.size() == 1 && s.paragraphs[0].style.align == Align::Justify,
              "the alignment it already has does not split it");
    }

    {
        Story s = one_para("", cs, ps);
        set_align_ranges(s, {{{0, 0}, {0, 0}}}, Align::Center);
        CHECK(s.paragraphs.size() == 1 && s.paragraphs[0].style.align == Align::Center,
              "an empty paragraph takes the alignment");
    }

    {
        Story s = one_para("abcdefghijklmnopqrstuvwxyz", cs, ps);
        TextPos anchor{0, 3}, caret{0, 17};
        set_align_ranges(s, {{{0, 0}, {0, 5}}, {{0, 15}, {0, 20}}}, Align::Center,
                         &caret, &anchor);
        CHECK(s.paragraphs.size() == 4, "two separate spans split into four (%zu)",
              s.paragraphs.size());
        if (s.paragraphs.size() == 4) {
            CHECK(paragraph_text(s.paragraphs[0]) == "abcde", "first span");
            CHECK(paragraph_text(s.paragraphs[1]) == "fghijklmno", "gap keeps the old alignment");
            CHECK(paragraph_text(s.paragraphs[2]) == "pqrst", "second span");
            CHECK(paragraph_text(s.paragraphs[3]) == "uvwxyz", "tail after both spans");
            CHECK(s.paragraphs[0].style.align == Align::Center &&
                  s.paragraphs[2].style.align == Align::Center, "both spans change");
            CHECK(s.paragraphs[1].style.align == Align::Justify &&
                  s.paragraphs[3].style.align == Align::Justify, "the gaps stay justified");
            CHECK(anchor.para == 0 && anchor.byte == 3, "anchor in the first span stays (%zu,%u)",
                  anchor.para, anchor.byte);
            CHECK(caret.para == 2 && caret.byte == 2, "caret in the later span follows both splits (%zu,%u)",
                  caret.para, caret.byte);
        }
    }

    /* Lines in one column of a threaded story. The other column, and a
     * second story, keep the alignment they had. */
    {
        ParaStyle head = ps;
        head.align = Align::Center;
        head.name = "Headline";
        head.first_indent = 0;
        Story headline = one_para("The headline sits in its own box", cs, head);
        Story body = one_para(kLong, cs, ps);
        const std::string original = paragraph_text(body.paragraphs[0]);
        std::vector<Frame> cols = {{0, 0, 160, 48}, {180, 0, 160, 800}};
        Composition before = compose(body, cols, fonts);
        CHECK(before.lines.size() > 2 && before.lines.front().frame == 0 &&
              before.lines.back().frame == 1, "the story threads into the second column");
        for (size_t i = 1; i < before.lines.size(); ++i)
            if (before.lines[i].para == before.lines[i - 1].para)
                CHECK(before.lines[i].byte_start == before.lines[i - 1].byte_end,
                      "wrapped lines meet (%u then %u)",
                      before.lines[i - 1].byte_end, before.lines[i].byte_start);

        std::vector<std::pair<TextPos, TextPos>> in_first;
        for (const ComposedLine &l : before.lines)
            if (l.frame == 0)
                in_first.push_back({{l.para, l.byte_start}, {l.para, l.byte_end}});
        CHECK(!in_first.empty(), "the first column has lines");
        set_align_ranges(body, in_first, Align::Center);

        std::string all;
        bool any_center = false, any_justify = false;
        for (const Paragraph &p : body.paragraphs) {
            all += paragraph_text(p);
            any_center = any_center || p.style.align == Align::Center;
            any_justify = any_justify || p.style.align == Align::Justify;
        }
        CHECK(all == original, "the column split keeps the story text");
        CHECK(any_center && any_justify, "only the selected column's lines change alignment");
        CHECK(headline.paragraphs.size() == 1 &&
              headline.paragraphs[0].style.align == Align::Center,
              "another story is left alone");
        CHECK(body.paragraphs.back().style.align == Align::Justify &&
              body.paragraphs.back().style.space_after == 6.f,
              "the text past the column keeps justification and its space after");
        CHECK(body.paragraphs.front().style.align == Align::Center &&
              body.paragraphs.front().style.first_indent == 12.f,
              "the selected column keeps the paragraph's first indent");

        Composition after = compose(body, cols, fonts);
        int centered = 0;
        for (const ComposedLine &l : after.lines) {
            if (body.paragraphs[l.para].style.align != Align::Center)
                continue;
            ++centered;
            if (l.runs.empty() || l.runs.front().glyphs.empty())
                continue;
            float x0 = l.runs.front().glyphs.front().x;
            float lm = x0 - l.left, rm = l.left + l.width - l.x_end;
            CHECK(std::fabs(lm - rm) < 1.0f, "a recentered line is centered (%.2f / %.2f)", lm, rm);
        }
        CHECK(centered > 0, "the selected lines compose centered");
    }
}

static void test_pdf(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    std::vector<Composition> comps;
    for (const StoryEntry &se : doc.stories)
        comps.push_back(compose(se.story, doc.thread_frames(se), fonts));
    const std::string path = "/tmp/pagemade_test.pdf";
    CHECK(export_pdf(path, doc, comps), "export_pdf writes the sample document");
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return;
    std::string bytes;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        bytes.append(buf, n);
    std::fclose(f);
    CHECK(bytes.rfind("%PDF-1.7", 0) == 0, "PDF header");
    CHECK(bytes.size() > 1000 &&
          bytes.compare(bytes.size() - 6, 6, "%%EOF\n") == 0, "PDF trailer");
    CHECK(bytes.find("/Type0") != std::string::npos, "a composite font is embedded");
    CHECK(bytes.find("Identity-H") != std::string::npos, "identity encoding");
    CHECK(bytes.find("/Count 1 >>") != std::string::npos, "one page");
    doc.insert_page(1);
    doc.pages[1].hidden = true;
    CHECK(export_pdf(path, doc, comps), "export with a hidden page");
    f = std::fopen(path.c_str(), "rb");
    bytes.clear();
    if (f) {
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            bytes.append(buf, n);
        std::fclose(f);
    }
    CHECK(bytes.find("/Count 1 >>") != std::string::npos, "a hidden page is not printed");
    doc.pages[1].hidden = false;
    CHECK(export_pdf(path, doc, comps), "export with two pages");
    f = std::fopen(path.c_str(), "rb");
    bytes.clear();
    if (f) {
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            bytes.append(buf, n);
        std::fclose(f);
    }
    CHECK(bytes.find("/Count 2 >>") != std::string::npos, "a shown page is printed");
    std::printf("  pdf: %zu bytes\n", bytes.size());
}

static void test_deterministic(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    for (const StoryEntry &se : doc.stories) {
        Composition a = compose(se.story, doc.thread_frames(se), fonts);
        Composition b = compose(se.story, doc.thread_frames(se), fonts);
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

static bool near_pt(Point a, Point b) { return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f; }

static void test_model(const FontLibrary &fonts) {
    /* Transforms: B applies first in A * B; inverse and rotate_about. */
    const float q = 3.14159265f / 2;
    Transform t = Transform::translate(10, 0) * Transform::rotate(q);
    CHECK(near_pt(t.apply({1, 0}), {10, 1}), "translate * rotate turns, then moves");
    CHECK(near_pt(t.inverse().apply(t.apply({3, -7})), {3, -7}), "inverse undoes the transform");
    Transform r = Transform::rotate_about(Transform::translate(5, 5), {20, 20}, 0.3f);
    CHECK(near_pt(r.apply(Transform::translate(5, 5).inverse().apply({20, 20})), {20, 20}),
          "rotate_about keeps the center fixed");
    CHECK(std::fabs(r.rotation() - 0.3f) < 1e-5f, "rotation() reads the angle back");

    /* The sample: ids, threads, item spaces. */
    PageDoc doc = sample_document();
    CHECK(doc.stories.size() == 3 && doc.pages.size() == 1 && doc.pages[0].items.size() == 6,
          "sample has 3 stories and 6 items (%zu, %zu)", doc.stories.size(),
          doc.pages[0].items.size());
    std::vector<uint32_t> ids;
    for (const Item &it : doc.pages[0].items) ids.push_back(it.id);
    for (const StoryEntry &se : doc.stories) ids.push_back(se.id);
    std::sort(ids.begin(), ids.end());
    CHECK(ids.front() > 0 && std::adjacent_find(ids.begin(), ids.end()) == ids.end() &&
          ids.back() < doc.next_id, "ids are unique, nonzero and below next_id");

    StoryEntry &body = doc.stories[1];
    CHECK(body.thread.size() == 2, "body threads two blocks");
    size_t ti = 99;
    CHECK(doc.story_of(body.thread[1], &ti) == &body && ti == 1, "story_of finds the thread slot");
    std::vector<Frame> fr = doc.thread_frames(body);
    const Item *col2 = doc.find_item(body.thread[1]);
    CHECK(fr.size() == 2 && fr[1].x == 0 && fr[1].y == 0 && fr[1].w == col2->w && fr[1].h == col2->h,
          "thread frames are in each block's own space");

    /* Stacking order is independent of reading order. */
    const Composition before = compose(body.story, doc.thread_frames(body), fonts);
    const ItemId col1 = body.thread[0];
    doc.bring_to_front(col1);
    CHECK(doc.pages[0].items.back().id == col1, "bring_to_front puts the block on top");
    doc.send_to_back(col1);
    CHECK(doc.pages[0].items.front().id == col1, "send_to_back puts it at the bottom");
    {
        PageDoc stack = sample_document();
        auto &items = stack.pages[0].items;
        const ItemId a = items[0].id, b = items[1].id, c = items[2].id, d = items[3].id;
        const std::vector<ItemId> thread = stack.stories[1].thread;
        stack.restack({b, c}, +1);
        CHECK(items[0].id == a && items[1].id == d && items[2].id == b && items[3].id == c,
              "bring forward moves the group up one, together");
        stack.restack({b, c}, -1);
        CHECK(items[1].id == b && items[2].id == c && items[3].id == d,
              "send backward puts the group back");
        CHECK(stack.stories[1].thread == thread, "restack does not touch reading order");
    }
    {
        PageDoc pages = sample_document();
        const size_t stories = pages.stories.size();
        const ItemId kept = pages.pages[0].items.front().id;
        CHECK(pages.insert_page(1) == 1 && pages.pages.size() == 2 && pages.pages[1].items.empty(),
              "insert_page adds a blank page");
        pages.pages[1].hidden = true;
        pages.move_page(1, 0);
        CHECK(pages.pages[0].hidden && pages.pages[0].items.empty() &&
              pages.pages[1].items.front().id == kept, "move_page reorders");
        CHECK(pages.remove_page(0) && pages.pages.size() == 1 && !pages.pages[0].hidden &&
              pages.stories.size() == stories, "removing a blank page keeps the stories");
        CHECK(pages.remove_page(0) == false, "the last page stays");
        pages.insert_page(1);
        CHECK(pages.remove_page(0) && pages.pages.size() == 1 && pages.pages[0].items.empty() &&
              pages.stories.empty(), "removing a page removes its items and their stories");
    }
    {
        PageDoc dup = sample_document();
        dup.pages[0].guides.push_back(Guide{});
        const size_t items = dup.pages[0].items.size();
        const size_t stories = dup.stories.size();
        dup.insert_page(1);
        CHECK(dup.duplicate_pages(0, 1) == 1 && dup.pages.size() == 3 &&
              dup.pages[1].items.size() == items && dup.pages[2].items.empty() &&
              dup.pages[1].guides.size() == 1, "duplicate_pages inserts the copy after");
        CHECK(dup.pages[1].uid != dup.pages[0].uid, "a copy is a new page");
        bool fresh = true, threaded = true;
        for (size_t i = 0; i < items; ++i) {
            const Item &o = dup.pages[0].items[i], &c = dup.pages[1].items[i];
            fresh = fresh && c.id != o.id && c.layer == o.layer && c.xf == o.xf &&
                    (o.group == 0) == (c.group == 0) && (o.group == 0 || c.group != o.group);
            if (o.is_text()) {
                const StoryEntry *os = dup.story_of(o.id), *cs = dup.story_of(c.id);
                threaded = threaded && os && cs && os->id != cs->id;
            }
        }
        CHECK(fresh, "copies get new ids and groups, same layer and place");
        CHECK(threaded && dup.stories.size() == stories * 2, "copied frames thread copied stories");
        CHECK(dup.duplicate_pages(1, 2) == 3 && dup.pages.size() == 5 &&
              dup.pages[3].items.size() == items && dup.pages[4].items.empty(),
              "a spread duplicates as a pair");
        CHECK(dup.duplicate_pages(9, 1) == SIZE_MAX && dup.pages.size() == 5,
              "past the end duplicates nothing");
    }
    const Composition after = compose(body.story, doc.thread_frames(body), fonts);
    CHECK(after.lines.size() == before.lines.size() && body.thread[0] == col1,
          "rearranging doesn't change the thread or the composition");

    /* Snapshots are independent copies (undo relies on it). */
    PageDoc snap = doc;
    doc.find_item(col1)->xf = Transform::translate(1, 2);
    CHECK(!(snap.find_item(col1)->xf == doc.find_item(col1)->xf), "a copy is a snapshot");

    /* Threading edits. */
    ItemId mid = doc.add_text_frame(0, body.id, 100, 50, Transform::translate(0, 0), 1);
    CHECK(body.thread.size() == 3 && body.thread[1] == mid, "add_text_frame threads at the index");
    doc.remove_item(mid);
    CHECK(body.thread.size() == 2 && !doc.find_item(mid), "remove_item unthreads the block");
    doc.remove_item(body.thread[1]);
    CHECK(doc.find_story(body.id) && doc.find_story(body.id)->thread.size() == 1,
          "removing one of two blocks keeps the story");
    const StoryId head = doc.stories[0].id;
    doc.remove_item(doc.stories[0].thread[0]);
    CHECK(!doc.find_story(head), "removing a story's last block removes the story");

    /* Explicit-frame hit testing: frames share no coordinate space. */
    PageDoc d2 = sample_document();
    const StoryEntry &b2 = d2.stories[1];
    Composition c = compose(b2.story, d2.thread_frames(b2), fonts);
    size_t first_in_1 = SIZE_MAX;
    for (size_t i = 0; i < c.lines.size(); ++i)
        if (c.lines[i].frame == 1) { first_in_1 = i; break; }
    if (first_in_1 != SIZE_MAX) {
        TextPos p = hit_test_frame(c, b2.story, 1, 0, 1);
        CHECK(p.para == c.lines[first_in_1].para && p.byte == c.lines[first_in_1].byte_start,
              "hit_test_frame(1, top-left) is the first position in block 1");
    }
}

static void test_tabs(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    const StoryEntry &side = doc.stories[2];     // the sidebar: right tab at 200, dot leader
    Composition c = compose(side.story, doc.thread_frames(side), fonts);
    int checked = 0;
    for (const ComposedLine &l : c.lines) {
        if (side.story.paragraphs[l.para].style.name != "Contents")
            continue;
        int leaders = 0;
        for (const GlyphRun &r : l.runs)
            for (const PlacedGlyph &g : r.glyphs)
                leaders += (g.flags & PlacedGlyph::Inserted) != 0;
        CHECK(std::fabs(l.x_end - 200.f) < 0.01f, "right tab sets the page number flush at 200 (%.2f)",
              l.x_end);
        CHECK(leaders > 5, "dot leader fills the tab (%d dots)", leaders);
        ++checked;
    }
    CHECK(checked == 4, "four contents lines (%d)", checked);

    CharStyle cs; cs.size = 10;
    ParaStyle ps;
    ps.tabs = {{100, TabAlign::Left, ""}, {200, TabAlign::Decimal, ""}};
    Composition t = compose(one_para("a\tb\t12.50", cs, ps), {{0, 0, 300, 100}}, fonts);
    float bx = -1, dot = -1;
    for (const GlyphRun &r : t.lines[0].runs)
        for (const PlacedGlyph &g : r.glyphs) {
            if (g.cluster == 2) bx = g.x;
            if (g.cluster == 6) dot = g.x;
        }
    CHECK(std::fabs(bx - 100) < 0.01f, "left tab: text starts at the stop (%.2f)", bx);
    CHECK(std::fabs(dot - 200) < 0.01f, "decimal tab: the point sits on the stop (%.2f)", dot);
    Composition d = compose(one_para("a\tb", cs, ParaStyle()), {{0, 0, 300, 100}}, fonts);
    float dflt = -1;
    for (const GlyphRun &r : d.lines[0].runs)
        for (const PlacedGlyph &g : r.glyphs)
            if (g.cluster == 2) dflt = g.x;
    CHECK(std::fabs(dflt - 36) < 0.01f, "default stops every half inch (%.2f)", dflt);
}

static void test_hyphenation(const FontLibrary &fonts) {
    Hyphenator hy;
    const std::string dir = std::string(PAGEMADE_DATA_DIR) + "/hyphenation/hyph-en-us";
    CHECK(hy.load(dir + ".pat.txt", dir + ".hyp.txt"), "patterns load from %s", dir.c_str());
    auto pts = hy.points(utf8_to_u32("hyphenation"));
    CHECK(pts == std::vector<size_t>({2, 6}), "hy-phen-ation (%zu points)", pts.size());
    pts = hy.points(utf8_to_u32("table"));
    CHECK(pts == std::vector<size_t>({2}), "exceptions file: ta-ble (%zu points)", pts.size());
    Hyphenator missing;
    CHECK(!missing.load(dir + ".pat.txt", dir + ".no-such-exceptions"),
          "a missing exceptions file fails to load");
    Hyphenator patterns_only;
    CHECK(patterns_only.load(dir + ".pat.txt"), "an empty exceptions path is optional");

    CharStyle cs; cs.size = 10;
    ParaStyle ps; ps.align = Align::Justify;
    Story s = one_para(kLong + " " + kLong, cs, ps);
    std::vector<Frame> col = {{0, 0, 120, 2000}};
    Composition with = compose(s, col, fonts, &hy);
    int hyph = 0;
    const std::string text = paragraph_text(s.paragraphs[0]);
    for (size_t i = 0; i < with.lines.size(); ++i) {
        const ComposedLine &l = with.lines[i];
        if (!l.hyphenated) continue;
        ++hyph;
        const PlacedGlyph &last = l.runs.back().glyphs.back();
        CHECK((last.flags & PlacedGlyph::Inserted) && last.cluster == l.byte_end,
              "a hyphenated line ends with an inserted hyphen");
        CHECK(i + 1 < with.lines.size() && with.lines[i + 1].byte_start == l.byte_end,
              "the word continues on the next line");
    }
    CHECK(hyph > 0, "a narrow justified column hyphenates (%d lines)", hyph);
    Composition without = compose(s, col, fonts, nullptr);
    int none = 0;
    for (const ComposedLine &l : without.lines) none += l.hyphenated;
    CHECK(none == 0, "no hyphenator, no automatic hyphens");

    ps.hyphen_limit = 1;
    Composition lim = compose(one_para(kLong + " " + kLong, cs, ps), col, fonts, &hy);
    bool twice = false;
    for (size_t i = 1; i < lim.lines.size(); ++i)
        twice |= lim.lines[i].hyphenated && lim.lines[i - 1].hyphenated;
    CHECK(!twice, "hyphen_limit 1: never two hyphenated lines in a row");

    /* Fragments are shaped again. "efficient" shapes as e + ffi-ligature +
     * ...; a break at ef-fi falls inside that ligature, so it only works if
     * "ef-" and "ficient" are shaped on their own: two glyphs before the
     * hyphen, and the fi ligature starting the next line. */
    CharStyle big; big.size = 30;
    bool found = false;
    for (float w = 20; w < 120 && !found; w += 1) {
        Composition e = compose(one_para("efficient", big, ParaStyle()), {{0, 0, w, 500}},
                                fonts, &hy);
        if (e.lines.size() < 2 || !e.lines[0].hyphenated || e.lines[0].byte_end != 2)
            continue;
        found = true;
        size_t ink0 = 0;
        for (const GlyphRun &r : e.lines[0].runs)
            for (const PlacedGlyph &g : r.glyphs)
                ink0 += !(g.flags & PlacedGlyph::Inserted);
        const auto &g1 = e.lines[1].runs.front().glyphs;
        CHECK(ink0 == 2, "\"ef-\": one glyph per letter (%zu)", ink0);
        CHECK(g1.size() >= 2 && g1[0].cluster == 2 && g1[1].cluster == 4,
              "\"ficient\" starts with the fi ligature (clusters %u, %u)",
              g1.empty() ? 0u : g1[0].cluster, g1.size() < 2 ? 0u : g1[1].cluster);
    }
    if (!found)
        std::printf("  (skip: no width breaks \"efficient\" at ef-fi)\n");
}


static bool near_color(Color c, float r, float g, float b, float a) {
    return std::fabs(c.r - r) < 1e-4f && std::fabs(c.g - g) < 1e-4f &&
           std::fabs(c.b - b) < 1e-4f && std::fabs(c.a - a) < 1e-4f;
}

static void test_swatches() {
    PageDoc doc;
    CHECK(near_color(doc.resolve({kBlack, 100}), 0, 0, 0, 1), "[Black] at 100%%");
    CHECK(near_color(doc.resolve({kBlack, 40}), 0.6f, 0.6f, 0.6f, 1), "a 40%% tint screens toward paper");
    CHECK(near_color(doc.resolve({kPaper, 100}), 1, 1, 1, 1), "[Paper] is white");
    CHECK(doc.resolve({kNoPaint, 100}).a == 0, "no paint is transparent");
    const SwatchId id = doc.add_swatch("Ochre", {0.8f, 0.6f, 0.2f, 1});
    CHECK(id > 9 && doc.find_swatch(id) && doc.find_swatch(id)->name == "Ochre",
          "added swatches get new ids (%u)", id);
}

static float polyline_length(const Polyline &p) {
    float len = 0;
    for (size_t i = 1; i < p.pts.size(); ++i)
        len += std::hypot(p.pts[i].x - p.pts[i - 1].x, p.pts[i].y - p.pts[i - 1].y);
    return len;
}

static void test_geometry() {
    Bounds b = path_bounds(rect_path(0, 0, 100, 50));
    CHECK(b.x0 == 0 && b.y0 == 0 && b.x1 == 100 && b.y1 == 50, "rect outline fills its box");
    Path rr = rect_path(0, 0, 100, 50, 10);
    b = path_bounds(rr);
    CHECK(b.x0 == 0 && b.y0 == 0 && b.x1 == 100 && b.y1 == 50, "rounded rect stays in its box");
    int cubics = 0;
    for (const PathCmd &c : rr) cubics += c.kind == PathCmd::Cubic;
    CHECK(cubics == 4, "rounded rect has four corner curves");

    /* Flattened ellipse points sit on the ellipse (the cubic arcs are
     * within 0.03% of a true quarter ellipse). */
    float worst = 0;
    for (const Polyline &pl : flatten(ellipse_path(50, 50, 50, 25), 0.05f))
        for (const Point &q : pl.pts) {
            const float nx = (q.x - 50) / 50, ny = (q.y - 50) / 25;
            worst = std::max(worst, std::fabs(std::sqrt(nx * nx + ny * ny) - 1));
        }
    CHECK(worst < 0.003f, "flattened ellipse error %.5f", worst);
    const size_t coarse = flatten(ellipse_path(0, 0, 100, 100), 1.f)[0].pts.size();
    const size_t fine = flatten(ellipse_path(0, 0, 100, 100), 0.01f)[0].pts.size();
    CHECK(fine > coarse * 4, "a tighter tolerance means more segments (%zu vs %zu)", fine, coarse);

    Path hex = polygon_path(100, 100, 6, 0);
    CHECK(hex.size() == 7 && hex.back().kind == PathCmd::Close, "hexagon: six vertices, closed");
    CHECK(std::fabs(hex[0].x - 50) < 1e-4f && std::fabs(hex[0].y) < 1e-4f, "first vertex at the top");
    Path star = polygon_path(100, 100, 5, 50);
    CHECK(star.size() == 11, "five-point star: ten vertices");
    const float r1 = std::hypot(star[1].x - 50, star[1].y - 50);
    CHECK(std::fabs(r1 - 25) < 1e-3f, "50%% inset puts inner vertices at half radius (%.3f)", r1);

    /* Dashing a straight 10-unit line. */
    std::vector<Polyline> line{{{{0, 0}, {10, 0}}, false}};
    auto d = dash(line, {2, 1});
    float on = 0;
    for (const Polyline &pl : d) on += polyline_length(pl);
    CHECK(d.size() == 4 && std::fabs(on - 7) < 1e-4f, "2-on 1-off over 10: 4 dashes, 7 on (%zu, %.3f)",
          d.size(), on);
    auto dots = dash(line, {0, 2});
    int singles = 0;
    for (const Polyline &pl : dots) singles += pl.pts.size() == 1;
    CHECK(dots.size() == 6 && singles == 6, "dots every 2 over 10: six dots (%zu)", dots.size());
    auto off = dash(line, {2, 1}, 1);
    CHECK(!off.empty() && std::fabs(polyline_length(off[0]) - 1) < 1e-4f, "offset 1 shortens the first dash");
    std::vector<Polyline> square{{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true}};
    auto sd = dash(square, {5, 5});
    CHECK(sd.size() == 4, "a closed path dashes all the way round (%zu)", sd.size());
    CHECK(dash_pattern(LineStyle::Solid, 1).empty() && line_style_round_caps(LineStyle::Dotted),
          "solid has no pattern; dots are round");

    Item it;
    it.w = 10;
    it.h = 20;
    it.xf = Transform::translate(100, 100) * Transform::rotate(3.14159265f / 2);
    Bounds ib = item_bounds(it);
    CHECK(std::fabs((ib.x1 - ib.x0) - 20) < 1e-3f && std::fabs((ib.y1 - ib.y0) - 10) < 1e-3f,
          "a quarter turn swaps the box's extents");
}

static void test_drawlist(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    std::vector<Composition> comps;
    for (const StoryEntry &se : doc.stories)
        comps.push_back(compose(se.story, doc.thread_frames(se), fonts));
    DrawList list = build_page(doc, 0, comps);

    size_t runs = 0;
    for (const Composition &c : comps)
        for (const ComposedLine &l : c.lines)
            runs += l.runs.size();
    size_t glyph_ops = 0, fills = 0, strokes = 0;
    for (const DrawOp &op : list) {
        glyph_ops += std::holds_alternative<DrawGlyphs>(op);
        fills += std::holds_alternative<DrawFill>(op);
        strokes += std::holds_alternative<DrawStroke>(op);
    }
    CHECK(glyph_ops == runs, "one glyph op per composed run (%zu of %zu)", glyph_ops, runs);
    CHECK(fills == 1 && strokes == 2, "rule: stroke; tint box: fill and stroke (%zu, %zu)", fills, strokes);
    CHECK(std::holds_alternative<DrawGlyphs>(list.front()), "the headline is at the bottom");

    /* The tint box comes before the sidebar text, and after it once it is
     * brought to the front. */
    const ItemId tint = doc.pages[0].items[4].id;
    auto fill_index = [](const DrawList &l) {
        for (size_t i = 0; i < l.size(); ++i)
            if (std::holds_alternative<DrawFill>(l[i])) return i;
        return l.size();
    };
    const size_t before = fill_index(list);
    CHECK(before + 2 < list.size() && std::holds_alternative<DrawGlyphs>(list.back()),
          "sidebar glyphs draw over the tint");
    doc.bring_to_front(tint);
    DrawList front = build_page(doc, 0, comps);
    CHECK(std::holds_alternative<DrawStroke>(front.back()) &&
          std::holds_alternative<DrawFill>(front[front.size() - 2]), "brought to front, the box draws last");

    Item dashed;
    dashed.w = 50;
    Shape sh;
    sh.kind = Shape::Kind::Line;
    sh.stroke.style = LineStyle::Dotted;
    sh.stroke.weight = 2;
    dashed.content = sh;
    doc.add_item(0, dashed);
    const DrawList with_dots = build_page(doc, 0, comps);
    const DrawStroke *ds = std::get_if<DrawStroke>(&with_dots.back());
    CHECK(ds && !ds->dash.empty() && ds->cap == LineCap::Round && ds->width == 2,
          "a dotted line becomes a round-capped dash pattern");
}

static void dump_sample(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    for (size_t si = 0; si < doc.stories.size(); ++si) {
        const StoryEntry &se = doc.stories[si];
        Composition c = compose(se.story, doc.thread_frames(se), fonts);
        std::printf("\nstory %zu: %zu lines, %s, %.3f ms\n", si, c.lines.size(),
                    c.overset ? "OVERSET" : "fits", c.compose_ms);
        for (const ComposedLine &l : c.lines) {
            std::string t = paragraph_text(se.story.paragraphs[l.para])
                                .substr(l.byte_start, l.byte_end - l.byte_start);
            std::printf("  b%zu p%-2zu base %6.2f  x %6.2f..%6.2f/%6.2f %s%s%s| %s\n",
                        l.frame, l.para, l.baseline, l.left, l.x_end, l.left + l.width,
                        l.loose ? "L" : " ", l.tight ? "T" : " ", l.hyphenated ? "-" : " ",
                        t.c_str());
        }
    }
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static int count_sub(const std::string &s, const std::string &sub) {
    int n = 0;
    for (size_t i = 0; (i = s.find(sub, i)) != std::string::npos; i += sub.size())
        ++n;
    return n;
}

static bool write_zip(const std::string &path, const char *mime, const std::string &json) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_file(&zip, path.c_str(), 0))
        return false;
    bool ok = true;
    if (mime)
        ok = mz_zip_writer_add_mem(&zip, "mimetype", mime, std::strlen(mime), MZ_NO_COMPRESSION);
    if (!json.empty())
        ok = ok && mz_zip_writer_add_mem(&zip, "document.json", json.data(), json.size(),
                                         MZ_DEFAULT_COMPRESSION);
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}

static void check_color(const Color &a, const Color &b, const char *what) {
    CHECK(near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a),
          "%s color", what);
}

static void check_char(const CharStyle &a, const CharStyle &b) {
    CHECK(a.family == b.family && a.face == b.face && a.bold == b.bold && a.italic == b.italic &&
          a.caps == b.caps && a.underline == b.underline && a.strike == b.strike,
          "char style face");
    CHECK(near(a.size, b.size) && near(a.leading, b.leading) && near(a.tracking, b.tracking) &&
          near(a.hscale, b.hscale) && near(a.baseline_shift, b.baseline_shift),
          "char style metrics");
    CHECK(a.kerning == b.kerning && a.ligatures == b.ligatures, "char style switches");
    check_color(a.color, b.color, "type");
    CHECK(a.swatch == b.swatch && near(a.tint, b.tint) && a.char_style == b.char_style,
          "swatch, tint and character style");
}

static void check_para(const ParaStyle &a, const ParaStyle &b) {
    CHECK(a.name == b.name && a.align == b.align, "paragraph name/align");
    CHECK(near(a.left_indent, b.left_indent) && near(a.right_indent, b.right_indent) &&
          near(a.first_indent, b.first_indent) && near(a.space_before, b.space_before) &&
          near(a.space_after, b.space_after) && near(a.autoleading, b.autoleading) &&
          near(a.line_spacing, b.line_spacing) && near(a.extra_spacing, b.extra_spacing) &&
          near(a.last_indent, b.last_indent),
          "paragraph spacing");
    CHECK(a.drop_lines == b.drop_lines && a.drop_chars == b.drop_chars &&
          near(a.drop_scale, b.drop_scale), "drop cap");
    CHECK(a.keep_together == b.keep_together && a.keep_with_next == b.keep_with_next &&
          a.start_new_frame == b.start_new_frame && a.orphans == b.orphans &&
          a.widows == b.widows, "keep options");
    CHECK(a.list == b.list && a.bullet == b.bullet && a.number_format == b.number_format &&
          a.number_suffix == b.number_suffix && a.list_start == b.list_start, "list");
    CHECK(near(a.word_min, b.word_min) && near(a.word_desired, b.word_desired) &&
          near(a.word_max, b.word_max) && near(a.letter_min, b.letter_min) &&
          near(a.letter_desired, b.letter_desired) && near(a.letter_max, b.letter_max),
          "spacing attributes");
    CHECK(a.tabs.size() == b.tabs.size(), "tab stops %zu vs %zu", a.tabs.size(), b.tabs.size());
    for (size_t i = 0; i < std::min(a.tabs.size(), b.tabs.size()); ++i)
        CHECK(near(a.tabs[i].pos, b.tabs[i].pos) && a.tabs[i].align == b.tabs[i].align &&
              a.tabs[i].leader == b.tabs[i].leader, "tab %zu", i);
    CHECK(near(a.default_tab, b.default_tab) && a.hyphenate == b.hyphenate &&
          a.hyphen_limit == b.hyphen_limit && near(a.hyphen_zone, b.hyphen_zone),
          "hyphenation");
}

static void check_doc(const PageDoc &a, const PageDoc &b) {
    CHECK(near(a.setup.width, b.setup.width) && near(a.setup.height, b.setup.height) &&
          near(a.setup.margin_top, b.setup.margin_top) &&
          near(a.setup.margin_bottom, b.setup.margin_bottom) &&
          near(a.setup.margin_inside, b.setup.margin_inside) &&
          near(a.setup.margin_outside, b.setup.margin_outside) &&
          a.setup.columns == b.setup.columns && near(a.setup.gutter, b.setup.gutter) &&
          a.setup.facing == b.setup.facing,
          "page setup");
    CHECK(a.swatches.size() == b.swatches.size(), "swatches %zu vs %zu",
          a.swatches.size(), b.swatches.size());
    for (size_t i = 0; i < std::min(a.swatches.size(), b.swatches.size()); ++i) {
        CHECK(a.swatches[i].id == b.swatches[i].id && a.swatches[i].name == b.swatches[i].name,
              "swatch %zu", i);
        check_color(a.swatches[i].rgb, b.swatches[i].rgb, "swatch");
        const Cmyk &x = a.swatches[i].cmyk, &y = b.swatches[i].cmyk;
        CHECK(x.c == y.c && x.m == y.m && x.y == y.y && x.k == y.k, "swatch cmyk %zu", i);
    }
    CHECK(a.char_styles.size() == b.char_styles.size(), "character styles %zu vs %zu",
          a.char_styles.size(), b.char_styles.size());
    for (size_t i = 0; i < std::min(a.char_styles.size(), b.char_styles.size()); ++i) {
        CHECK(a.char_styles[i].name == b.char_styles[i].name &&
              a.char_styles[i].fields == b.char_styles[i].fields, "character style %zu", i);
        check_char(a.char_styles[i].type, b.char_styles[i].type);
    }
    CHECK(a.styles.size() == b.styles.size(), "styles %zu vs %zu", a.styles.size(),
          b.styles.size());
    for (size_t i = 0; i < std::min(a.styles.size(), b.styles.size()); ++i) {
        CHECK(a.styles[i].name == b.styles[i].name, "style %zu name", i);
        check_para(a.styles[i].para, b.styles[i].para);
        check_char(a.styles[i].type, b.styles[i].type);
    }
    auto same_guides = [](const std::vector<Guide> &x, const std::vector<Guide> &y) {
        if (x.size() != y.size())
            return false;
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i].vertical != y[i].vertical || !near(x[i].pos, y[i].pos))
                return false;
        return true;
    };
    CHECK(same_guides(a.guides, b.guides), "every-page guides %zu vs %zu",
          a.guides.size(), b.guides.size());
    CHECK(a.pages.size() == b.pages.size(), "pages %zu vs %zu", a.pages.size(), b.pages.size());
    for (size_t p = 0; p < std::min(a.pages.size(), b.pages.size()); ++p) {
        CHECK(a.pages[p].hidden == b.pages[p].hidden, "page %zu hidden", p);
        CHECK(same_guides(a.pages[p].guides, b.pages[p].guides), "page %zu guides", p);
        CHECK(a.pages[p].items.size() == b.pages[p].items.size(),
              "page %zu items %zu vs %zu", p, a.pages[p].items.size(), b.pages[p].items.size());
        size_t n = std::min(a.pages[p].items.size(), b.pages[p].items.size());
        for (size_t i = 0; i < n; ++i) {
            const Item &x = a.pages[p].items[i], &y = b.pages[p].items[i];
            CHECK(x.id == y.id && x.is_text() == y.is_text() && near(x.w, y.w) && near(x.h, y.h),
                  "item %zu on page %zu", i, p);
            CHECK(near(x.xf.a, y.xf.a) && near(x.xf.b, y.xf.b) && near(x.xf.c, y.xf.c) &&
                  near(x.xf.d, y.xf.d) && near(x.xf.e, y.xf.e) && near(x.xf.f, y.xf.f),
                  "item %zu transform", i);
            CHECK(x.is_image() == y.is_image(), "item %zu picture", i);
            if (x.image() && y.image()) {
                const PlacedImage &xi = *x.image(), &yi = *y.image();
                CHECK(xi.asset == yi.asset && near(xi.x, yi.x) && near(xi.y, yi.y) &&
                      near(xi.w, yi.w) && near(xi.h, yi.h), "item %zu placement", i);
                CHECK(near(xi.opacity, yi.opacity) &&
                      xi.stroke.paint.swatch == yi.stroke.paint.swatch &&
                      near(xi.stroke.weight, yi.stroke.weight) &&
                      xi.stroke.style == yi.stroke.style, "item %zu picture style", i);
            }
            if (x.shape() && y.shape()) {
                const Shape &sx = *x.shape(), &sy = *y.shape();
                CHECK(sx.kind == sy.kind && sx.sides == sy.sides &&
                      near(sx.corner_radius, sy.corner_radius) && near(sx.star_inset, sy.star_inset),
                      "shape %zu", i);
                CHECK(sx.fill.swatch == sy.fill.swatch && near(sx.fill.tint, sy.fill.tint), "fill");
                CHECK(sx.stroke.paint.swatch == sy.stroke.paint.swatch &&
                      near(sx.stroke.paint.tint, sy.stroke.paint.tint) &&
                      near(sx.stroke.weight, sy.stroke.weight) && sx.stroke.style == sy.stroke.style,
                      "stroke");
            }
        }
    }
    CHECK(a.stories.size() == b.stories.size(), "stories %zu vs %zu",
          a.stories.size(), b.stories.size());
    for (size_t s = 0; s < std::min(a.stories.size(), b.stories.size()); ++s) {
        CHECK(a.stories[s].id == b.stories[s].id && a.stories[s].thread == b.stories[s].thread,
              "story %zu thread", s);
        const Story &x = a.stories[s].story, &y = b.stories[s].story;
        CHECK(x.paragraphs.size() == y.paragraphs.size(), "story %zu paragraphs", s);
        for (size_t p = 0; p < std::min(x.paragraphs.size(), y.paragraphs.size()); ++p) {
            check_para(x.paragraphs[p].style, y.paragraphs[p].style);
            CHECK(x.paragraphs[p].runs.size() == y.paragraphs[p].runs.size(), "runs");
            for (size_t r = 0; r < std::min(x.paragraphs[p].runs.size(), y.paragraphs[p].runs.size()); ++r) {
                CHECK(x.paragraphs[p].runs[r].text == y.paragraphs[p].runs[r].text,
                      "run text %s", x.paragraphs[p].runs[r].text.c_str());
                check_char(x.paragraphs[p].runs[r].style, y.paragraphs[p].runs[r].style);
            }
        }
    }
    CHECK(a.next_id == b.next_id, "next id %u vs %u", a.next_id, b.next_id);
    CHECK(a.next_asset == b.next_asset, "next asset %u vs %u", a.next_asset, b.next_asset);
    CHECK(a.images.size() == b.images.size(), "pictures %zu vs %zu",
          a.images.size(), b.images.size());
    for (size_t i = 0; i < std::min(a.images.size(), b.images.size()); ++i) {
        const ImageAsset &x = a.images[i], &y = b.images[i];
        CHECK(x.id == y.id && x.name == y.name && x.source == y.source, "picture %zu identity", i);
        CHECK(x.source_size == y.source_size && x.source_mtime == y.source_mtime &&
              x.hash == y.hash, "picture %zu stamp", i);
        CHECK(x.meta.format == y.meta.format && x.meta.width_px == y.meta.width_px &&
              x.meta.height_px == y.meta.height_px &&
              x.meta.stored_width_px == y.meta.stored_width_px &&
              x.meta.stored_height_px == y.meta.stored_height_px &&
              x.meta.bit_depth == y.meta.bit_depth && x.meta.channels == y.meta.channels &&
              x.meta.model == y.meta.model && x.meta.alpha == y.meta.alpha &&
              x.meta.orientation == y.meta.orientation, "picture %zu metadata", i);
        CHECK(std::fabs(x.meta.ppi_x - y.meta.ppi_x) < 1e-3 &&
              std::fabs(x.meta.ppi_y - y.meta.ppi_y) < 1e-3, "picture %zu resolution", i);
        CHECK(x.meta.icc == y.meta.icc, "picture %zu profile", i);
    }
}

static void test_uri() {
    std::string path;
    CHECK(file_uri("/a b/c.otf") == "file:///a%20b/c.otf", "file uri encodes a space");
    CHECK(file_path_from_uri("file:///a%20b/c.otf", path) && path == "/a b/c.otf",
          "file uri decodes (%s)", path.c_str());
    CHECK(file_path_from_uri("file://localhost/tmp/x", path) && path == "/tmp/x",
          "localhost is this machine");
    CHECK(file_path_from_uri("FILE:///tmp/x", path) && path == "/tmp/x", "FILE: scheme");
    CHECK(!file_path_from_uri("file://other/tmp/x", path), "a remote host is not a local path");
    CHECK(!file_path_from_uri("http://example.com/a", path), "http is not a file");
    CHECK(is_package_ref("assets/fonts/P052-Roman.otf"), "a package path is a package ref");
    CHECK(package_ref("assets/fonts", "A B.otf") == "assets/fonts/A%20B.otf", "package ref encodes");
    CHECK(package_entry("assets/fonts/A%20B.otf") == "assets/fonts/A B.otf", "package entry decodes");
    CHECK(!is_package_ref("/assets/a.otf") && !is_package_ref("file:///a") &&
          !is_package_ref("assets/../secret") && !is_package_ref("./assets/a") &&
          !is_package_ref(""),
          "absolute, scheme, and parent paths are not package refs");
}

static void test_arrange() {
    PageSetup s;
    s.width = 100;
    s.height = 200;
    std::vector<PageSlot> slots;
    float w = 0, h = 0;
    arrange_pages(s, 3, 0, PageView::One, slots, w, h);
    CHECK(slots.size() == 1 && slots[0].index == 0 && w == 100 && h == 200, "one page");
    arrange_pages(s, 3, 1, PageView::Spread, slots, w, h);
    CHECK(slots.size() == 2 && slots[0].index == 0 && slots[1].index == 1 &&
          slots[1].x == 124 && w == 224 && h == 200, "non-facing pair");
    arrange_pages(s, 3, 2, PageView::Spread, slots, w, h);
    CHECK(slots.size() == 1 && slots[0].index == 2 && slots[0].x == 0 && w == 224,
          "last page of an odd count sits on the left");
    arrange_pages(s, 1, 0, PageView::Spread, slots, w, h);
    CHECK(slots.size() == 1 && slots[0].x == 0 && w == 100, "a single page has no empty half");
    s.facing = true;
    arrange_pages(s, 5, 0, PageView::Spread, slots, w, h);
    CHECK(slots.size() == 1 && slots[0].index == 0 && slots[0].x == 100 && w == 200,
          "facing page 1 sits on the right, at the spine");
    arrange_pages(s, 5, 1, PageView::Spread, slots, w, h);
    CHECK(slots.size() == 2 && slots[0].index == 1 && slots[1].index == 2 && slots[1].x == 100,
          "facing pages 2–3 meet at the spine");
    arrange_pages(s, 5, 2, PageView::Spread, slots, w, h);
    CHECK(slots.size() == 2 && slots[0].index == 1 && slots[1].index == 2,
          "page 3 shares the 2–3 spread");
    arrange_pages(s, 4, 3, PageView::Spread, slots, w, h);
    CHECK(slots.size() == 1 && slots[0].index == 3 && slots[0].x == 0,
          "a trailing left-hand page sits on the left");
    arrange_pages(s, 4, 0, PageView::Stack, slots, w, h);
    CHECK(slots.size() == 4 && slots[0].x == 100 && slots[0].y == 0 && slots[1].x == 0 &&
          slots[1].y == 200 + kPageGap && slots[2].x == 100 && slots[2].y == slots[1].y &&
          slots[3].x == 0 && slots[3].y == 2 * (200 + kPageGap) && w == 200 &&
          h == 3 * 200 + 2 * kPageGap, "facing stack is a column of spreads");
    arrange_pages(s, 1, 0, PageView::Stack, slots, w, h);
    CHECK(slots.size() == 1 && slots[0].x == 0 && w == 100, "a lone facing page stacks alone");
    s.facing = false;
    arrange_pages(s, 3, 0, PageView::Stack, slots, w, h);
    CHECK(slots.size() == 3 && slots[2].y == 2 * (200 + kPageGap) && w == 100 &&
          h == 3 * 200 + 2 * kPageGap, "stack");
    float left = 0, right = 0;
    s.facing = false;
    s.margin_inside = 10;
    s.margin_outside = 30;
    page_side_margins(s, 1, left, right);
    CHECK(left == 10 && right == 30, "inside stays on the left");
    s.facing = true;
    page_side_margins(s, 0, left, right);
    CHECK(left == 10 && right == 30, "a right-hand page keeps inside on the left");
    page_side_margins(s, 1, left, right);
    CHECK(left == 30 && right == 10, "a left-hand page puts inside on the right");
    PageDoc doc = new_publication(s, 3);
    CHECK(doc.pages.size() == 3 && doc.setup.facing && doc.pages[0].items.empty() &&
          doc.pages[2].items.empty(), "blank publication");
    CHECK(new_publication(s, 0).pages.size() == 1, "a publication keeps one page");

    PageDoc moved;
    moved.pages.resize(2);
    Item box;
    box.w = 10;
    box.h = 20;
    box.xf = Transform::translate(3, 4);
    const ItemId id = moved.add_item(0, box);
    CHECK(moved.transfer_item(id, 1), "an item can move to another page");
    size_t pg = 99;
    const Item *got = moved.find_item(id, &pg);
    CHECK(got && pg == 1 && got->id == id && got->xf.e == 3 && got->xf.f == 4 &&
          moved.pages[0].items.empty() && moved.pages[1].items.size() == 1,
          "the move keeps the id and the transform");
    CHECK(!moved.transfer_item(id, 1), "an item already on the page stays");
    CHECK(!moved.transfer_item(id, 9), "a missing page is refused");
}

static void test_docfile(const FontLibrary &fonts) {
    test_uri();

    PageDoc doc = sample_document();
    doc.setup.facing = true;
    doc.insert_page(1);
    doc.pages[1].hidden = true;
    Item star;
    star.w = 80;
    star.h = 40;
    star.xf = Transform::rotate_about(Transform::translate(72, 100), {112, 120}, 0.2f);
    Shape sh;
    sh.kind = Shape::Kind::Polygon;
    sh.sides = 5;
    sh.star_inset = 35;
    sh.fill = {kBlack, 40};
    sh.stroke = {{kBlack, 80}, 2.f, LineStyle::DashDot};
    star.content = sh;
    doc.add_item(1, std::move(star));

    CharStyle note;
    note.family = "Sans";
    note.size = 14;
    note.baseline_shift = 2;
    note.tracking = 15;
    note.hscale = 0.9f;
    note.kerning = false;
    note.color = {0.2f, 0.3f, 0.4f, 1};
    note.swatch = 6;
    note.tint = 40;
    note.char_style = "Accent";
    ParaStyle note_ps;
    note_ps.name = "Note";
    note_ps.hyphenate = false;
    note_ps.hyphen_limit = 2;
    note_ps.hyphen_zone = 28;
    note_ps.default_tab = 48;
    note_ps.word_min = 80;
    note_ps.line_spacing = 1.5f;
    note_ps.extra_spacing = -1.5f;
    note_ps.last_indent = 24;
    note_ps.align = Align::JustifyRight;
    note_ps.drop_lines = 2;
    note_ps.drop_chars = 2;
    note_ps.drop_scale = 120;
    note_ps.keep_with_next = true;
    note_ps.keep_together = true;
    note_ps.start_new_frame = true;
    note_ps.orphans = 2;
    note_ps.widows = 3;
    note_ps.list = ListKind::Number;
    note_ps.number_format = NumberFormat::UpperRoman;
    note_ps.number_suffix = ")";
    note_ps.list_start = 4;
    note_ps.tabs.push_back({120, TabAlign::Decimal, "."});
    Story extra = one_para("He said \"hello\"\nsecond\tline\\end", note, note_ps);
    doc.add_text_frame(1, doc.add_story(std::move(extra)), 200, 80, Transform::translate(40, 200));
    doc.guides = {{true, 36.5f}, {false, 144.f}};         // ruler guides, every page
    doc.pages[1].guides = {{false, 72.25f}, {true, 300.f}}; // and one page's own

    const std::string linked = "/tmp/pagemade-doc-linked.pagemade";
    const std::string embedded = "/tmp/pagemade-doc-embedded.pagemade";
    std::string error;
    CHECK(save_document(doc, fonts, linked, SaveOptions{false}, &error),
          "save linked: %s", error.c_str());
    OpenResult opened = open_document(linked, fonts);
    CHECK(opened.ok, "open linked: %s", opened.error.c_str());
    CHECK(!opened.embed_assets && opened.missing_fonts.empty(), "linked, fonts installed");
    if (opened.ok)
        check_doc(doc, opened.doc);

    /* A family the library doesn't have must not be saved as some other face. */
    PageDoc missing = doc;
    missing.stories[0].story.paragraphs[0].runs[0].style.family = "Nope";
    std::string json = document_json(missing, fonts, {});
    CHECK(count_sub(json, "Nope") == 1, "Nope stays a family name, not an asset (%d)",
          count_sub(json, "Nope"));
    CHECK(save_document(missing, fonts, linked, {}, &error), "save with a missing family");
    OpenResult gone = open_document(linked, fonts);
    CHECK(gone.ok && gone.missing_fonts.size() == 1 && gone.missing_fonts[0] == "Nope Bold",
          "missing face is reported");

    CHECK(save_document(doc, fonts, embedded, SaveOptions{true}, &error),
          "save embedded: %s", error.c_str());
    FILE *raw = std::fopen(embedded.c_str(), "rb");
    char head[160] = {};
    size_t nread = 0;
    if (raw) {
        nread = std::fread(head, 1, sizeof head - 1, raw);
        std::fclose(raw);
    }
    CHECK(nread > 0 && std::string(head, nread).find(kPublicationMimeType) != std::string::npos,
          "mimetype is stored uncompressed at the front");
    FontLibrary bare;
    OpenResult emb = open_document(embedded, bare);
    CHECK(emb.ok && emb.embed_assets, "open embedded: %s", emb.error.c_str());
    CHECK(emb.fonts.size() >= 3 && emb.missing_fonts.empty(),
          "embedded faces travel with the file (%zu, missing %zu)",
          emb.fonts.size(), emb.missing_fonts.size());
    if (emb.ok)
        check_doc(doc, emb.doc);
    FontLibrary carried;
    for (const DocumentFont &f : emb.fonts)
        carried.add_document_font(f.family, f.style, f.font);
    if (!doc.stories.empty() && carried.face_for("Display", true, false)) {
        const StoryEntry &se = doc.stories[0];
        Composition here = compose(se.story, doc.thread_frames(se), fonts);
        Composition there = compose(se.story, doc.thread_frames(se), carried);
        CHECK(!here.lines.empty() && here.lines.size() == there.lines.size() &&
              near(here.lines[0].x_end, there.lines[0].x_end),
              "embedded fonts compose the same line");
    }

    const std::string keep = "/tmp/pagemade-doc-keep.pagemade";
    CHECK(save_document(doc, fonts, keep, {}, &error), "save the file we must not clobber");
    CHECK(!save_document(doc, fonts, "/tmp/no-such-pagemade-dir/out.pagemade", {}, &error),
          "saving into a missing directory fails");
    CHECK(std::fopen("/tmp/no-such-pagemade-dir/out.pagemade.saving", "rb") == nullptr,
          "a failed save leaves no temporary file");
    OpenResult kept = open_document(keep, fonts);
    CHECK(kept.ok, "the previous publication is still there: %s", kept.error.c_str());

    const char *repair_json =
        "{"
        "\"format\":\"pagemade\",\"version\":99,"
        "\"pages\":[{\"hidden\":true,\"items\":["
        "{\"id\":5,\"type\":\"text\",\"w\":100,\"h\":40,\"xf\":[1,0,0,1,10,20]},"
        "{\"id\":5,\"type\":\"shape\",\"shape\":\"rectangle\",\"w\":10,\"h\":10,\"xf\":[1,0,0,1,0,0]},"
        "{\"id\":0,\"type\":\"text\",\"w\":1,\"h\":1,\"xf\":[1,0,0,1,0,0]},"
        "{\"id\":7,\"type\":\"text\",\"w\":30,\"h\":20,\"xf\":[1,0,0,1,4,6]},"
        "{\"id\":8,\"type\":\"image\",\"w\":1,\"h\":1,\"xf\":[1,0,0,1,0,0]}"
        "]}],"
        "\"stories\":["
        "{\"id\":9,\"thread\":[5,99],\"paragraphs\":[{\"style\":{\"line_spacing\":9},"
        "\"runs\":[{\"text\":\"Kept\"}]}]},"
        "{\"id\":9,\"thread\":[],\"paragraphs\":[{\"runs\":[{\"text\":\"Gone\"}]}]}"
        "]}";
    const std::string repair_path = "/tmp/pagemade-doc-repair.pagemade";
    CHECK(write_zip(repair_path, kPublicationMimeType, repair_json), "write a damaged package");
    OpenResult repaired = open_document(repair_path, fonts);
    CHECK(repaired.ok, "a newer, damaged file still opens: %s", repaired.error.c_str());
    CHECK(repaired.doc.styles.size() == default_styles().size(),
          "a file from before styles gets the default set (%zu)", repaired.doc.styles.size());
    for (const StoryEntry &se : repaired.doc.stories)
        for (const Paragraph &p : se.story.paragraphs)
            if (!p.runs.empty() && p.runs.front().text == "Kept")
                CHECK(p.style.line_spacing == kMaxLineSpacing,
                      "line spacing is clamped on open (%g)", p.style.line_spacing);
    CHECK(!repaired.warnings.empty(), "damage and a newer version are reported");
    if (repaired.ok) {
        CHECK(repaired.doc.pages.size() == 1 && repaired.doc.pages[0].hidden, "the page survived");
        CHECK(repaired.doc.pages[0].items.size() == 3,
              "duplicate and zero ids drop; a picture item stays");
        int pictures = 0;
        for (const Item &it : repaired.doc.pages[0].items)
            pictures += it.is_image() ? 1 : 0;
        CHECK(pictures == 1, "the picture item is kept (%d)", pictures);
        CHECK(repaired.doc.stories.size() == 2, "the empty story drops; the stray frame gets one");
        bool kept_text = false;
        for (const StoryEntry &se : repaired.doc.stories)
            for (const Paragraph &p : se.story.paragraphs)
                for (const Run &r : p.runs)
                    kept_text |= r.text == "Kept";
        CHECK(kept_text, "the threaded story's text is kept");
    }

    const std::string bad = "/tmp/pagemade-doc-bad.pagemade";
    CHECK(write_zip(bad, "application/zip", "{\"format\":\"pagemade\",\"version\":1}"),
          "write a foreign package");
    OpenResult foreign = open_document(bad, fonts);
    CHECK(!foreign.ok, "the wrong mimetype is refused");
    CHECK(write_zip(bad, kPublicationMimeType, "{\"format\":\"other\",\"version\":1}"),
          "write a package with the wrong document");
    CHECK(!open_document(bad, fonts).ok, "a non-pagemade document.json is refused");
    CHECK(write_zip(bad, kPublicationMimeType, ""), "write a package with no document");
    CHECK(!open_document(bad, fonts).ok, "a package without document.json is refused");
    FILE *plain = std::fopen(bad.c_str(), "wb");
    if (plain) {
        std::fputs("not a zip", plain);
        std::fclose(plain);
    }
    CHECK(!open_document(bad, fonts).ok, "a file that isn't a zip is refused");
    CHECK(!open_document("/tmp/pagemade-does-not-exist.pagemade", fonts).ok,
          "a missing file is refused");

    /* Empty strings are valid JSON (a picture placed before its format was
     * known saved "format": "") */
    CHECK(write_zip(bad, kPublicationMimeType,
                    "{\"format\":\"pagemade\",\"version\":1,\"pdf\":{\"title\":\"\"},"
                    "\"assets\":[{\"kind\":\"image\",\"id\":2,\"name\":\"a.heic\","
                    "\"meta\":{\"format\":\"\",\"width\":108,\"height\":108}}],"
                    "\"pages\":[{\"items\":[]}]}"),
          "write a package with empty strings");
    OpenResult empties = open_document(bad, fonts);
    CHECK(empties.ok && empties.damage.empty(), "empty strings open cleanly: %s %s",
          empties.error.c_str(), empties.damage.c_str());

    /* A truncated document.json keeps what was written before the damage */
    CHECK(write_zip(bad, kPublicationMimeType,
                    "{\"format\":\"pagemade\",\"version\":1,"
                    "\"stories\":[{\"id\":9,\"thread\":[5],\"paragraphs\":[{\"runs\":"
                    "[{\"text\":\"Survives\"}]}]}],"
                    "\"pages\":[{\"items\":["
                    "{\"id\":5,\"type\":\"text\",\"w\":100,\"h\":40,\"xf\":[1,0,0,1,10,20]},"
                    "{\"id\":6,\"type\":\"shape\",\"shape\":\"rectan"),
          "write a truncated package");
    OpenResult cut = open_document(bad, fonts);
    CHECK(cut.ok, "a truncated document still opens: %s", cut.error.c_str());
    CHECK(cut.damage.find("line 1, column") != std::string::npos,
          "the damage says where: %s", cut.damage.c_str());
    if (cut.ok) {
        bool frame = false, text = false;
        for (const Page &pg : cut.doc.pages)
            for (const Item &it : pg.items)
                frame |= it.id == 5;
        for (const StoryEntry &se : cut.doc.stories)
            for (const Paragraph &p : se.story.paragraphs)
                for (const Run &r : p.runs)
                    text |= r.text == "Survives";
        CHECK(frame && text, "the frame and story before the damage survive");
    }
    CHECK(write_zip(bad, kPublicationMimeType, "{\"form"), "write a package cut at the start");
    OpenResult early = open_document(bad, fonts);
    CHECK(!early.ok && early.error.find("damaged") != std::string::npos,
          "damage before anything usable is refused with a reason: %s", early.error.c_str());

    std::remove(linked.c_str());
    std::remove(embedded.c_str());
    std::remove(keep.c_str());
    std::remove(repair_path.c_str());
    std::remove(bad.c_str());
}

/* A tiny uncompressed-filter PNG. `rgb` is width * height * 3, or null for black.
 * ppm is pixels per metre (0 omits pHYs). 300 ppi is 11811 ppm. */
static std::vector<uint8_t> make_png(int w, int h, const uint8_t *rgb, uint32_t ppm_x, uint32_t ppm_y) {
    auto be = [](std::vector<uint8_t> &o, uint32_t v) {
        o.push_back((uint8_t) (v >> 24));
        o.push_back((uint8_t) (v >> 16));
        o.push_back((uint8_t) (v >> 8));
        o.push_back((uint8_t) v);
    };
    auto chunk = [&](std::vector<uint8_t> &o, const char *type, const uint8_t *data, size_t n) {
        be(o, (uint32_t) n);
        const size_t at = o.size();
        o.insert(o.end(), type, type + 4);
        if (n)
            o.insert(o.end(), data, data + n);
        be(o, (uint32_t) mz_crc32(0, o.data() + at, 4 + n));
    };
    std::vector<uint8_t> raw((size_t) h * (1 + (size_t) w * 3));
    for (int y = 0; y < h; ++y) {
        uint8_t *row = raw.data() + (size_t) y * (1 + (size_t) w * 3);
        row[0] = 0;
        for (int x = 0; x < w; ++x) {
            const uint8_t *p = rgb ? rgb + ((size_t) y * w + x) * 3 : nullptr;
            row[1 + x * 3] = p ? p[0] : 0;
            row[2 + x * 3] = p ? p[1] : 0;
            row[3 + x * 3] = p ? p[2] : 0;
        }
    }
    mz_ulong zlen = mz_compressBound((mz_ulong) raw.size());
    std::vector<uint8_t> z(zlen);
    CHECK(mz_compress(z.data(), &zlen, raw.data(), (mz_ulong) raw.size()) == MZ_OK, "png deflate");
    z.resize(zlen);

    uint8_t ihdr[13] = {};
    ihdr[0] = (uint8_t) (w >> 24); ihdr[1] = (uint8_t) (w >> 16);
    ihdr[2] = (uint8_t) (w >> 8);  ihdr[3] = (uint8_t) w;
    ihdr[4] = (uint8_t) (h >> 24); ihdr[5] = (uint8_t) (h >> 16);
    ihdr[6] = (uint8_t) (h >> 8);  ihdr[7] = (uint8_t) h;
    ihdr[8] = 8;
    ihdr[9] = 2;                     // RGB
    std::vector<uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
    chunk(out, "IHDR", ihdr, 13);
    if (ppm_x || ppm_y) {
        uint8_t phys[9] = {};
        phys[0] = (uint8_t) (ppm_x >> 24); phys[1] = (uint8_t) (ppm_x >> 16);
        phys[2] = (uint8_t) (ppm_x >> 8);  phys[3] = (uint8_t) ppm_x;
        phys[4] = (uint8_t) (ppm_y >> 24); phys[5] = (uint8_t) (ppm_y >> 16);
        phys[6] = (uint8_t) (ppm_y >> 8);  phys[7] = (uint8_t) ppm_y;
        phys[8] = 1;
        chunk(out, "pHYs", phys, 9);
    }
    chunk(out, "IDAT", z.data(), z.size());
    chunk(out, "IEND", nullptr, 0);
    return out;
}

/* JPEG header only: JFIF density, EXIF orientation 6, SOF. It need not decode. */
static std::vector<uint8_t> make_jpeg_header(int stored_w, int stored_h, int dpi) {
    auto be16 = [](std::vector<uint8_t> &o, unsigned v) {
        o.push_back((uint8_t) (v >> 8));
        o.push_back((uint8_t) v);
    };
    std::vector<uint8_t> tiff = {
        'I', 'I', 42, 0, 8, 0, 0, 0,
        1, 0,
        0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0,
        0, 0, 0, 0
    };
    std::vector<uint8_t> o = {0xFF, 0xD8};
    o.insert(o.end(), {0xFF, 0xE0});
    be16(o, 16);
    o.insert(o.end(), {'J', 'F', 'I', 'F', 0, 1, 1, 1});
    be16(o, (unsigned) dpi);
    be16(o, (unsigned) dpi);
    o.insert(o.end(), {0, 0});
    o.insert(o.end(), {0xFF, 0xE1});
    be16(o, (unsigned) (tiff.size() + 8));
    o.insert(o.end(), {'E', 'x', 'i', 'f', 0, 0});
    o.insert(o.end(), tiff.begin(), tiff.end());
    o.insert(o.end(), {0xFF, 0xC0});
    be16(o, 17);
    o.push_back(8);
    be16(o, (unsigned) stored_h);
    be16(o, (unsigned) stored_w);
    o.push_back(3);
    o.insert(o.end(), {1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0});
    return o;
}

static std::vector<uint8_t> make_jpeg_rgb(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> rgb((size_t) w * h * 3);
    for (size_t i = 0; i < rgb.size(); i += 3) {
        rgb[i] = r; rgb[i + 1] = g; rgb[i + 2] = b;
    }
    struct Buf { std::vector<uint8_t> bytes; } buf;
    stbi_write_jpg_to_func([](void *ctx, void *data, int n) {
        auto *b = (Buf *) ctx;
        const auto *p = (const uint8_t *) data;
        b->bytes.insert(b->bytes.end(), p, p + n);
    }, &buf, w, h, 3, rgb.data(), 90);
    return buf.bytes;
}

static std::string read_bin(const std::string &path) {
    std::string bytes;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return bytes;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        bytes.append(buf, n);
    std::fclose(f);
    return bytes;
}

/* Content streams are Flate. Append each inflated stream so a search sees `W n`. */
static std::string pdf_plain(const std::string &pdf) {
    std::string out = pdf;
    for (size_t at = 0; (at = pdf.find("stream\n", at)) != std::string::npos; ) {
        const size_t data = at + 7;
        const size_t dict = pdf.rfind("<<", at);
        at = data;
        if (dict == std::string::npos)
            continue;
        if (pdf.find("/FlateDecode", dict) > data && pdf.find("/FlateDecode", dict) != std::string::npos)
            continue;
        if (pdf.find("/FlateDecode", dict) == std::string::npos)
            continue;
        const size_t lp = pdf.find("/Length ", dict);
        if (lp == std::string::npos || lp > data)
            continue;
        const int len = std::atoi(pdf.c_str() + lp + 8);
        if (len <= 0 || data + (size_t) len > pdf.size())
            continue;
        mz_ulong dest_len = (mz_ulong) len * 16 + 1024;
        std::vector<unsigned char> dest(dest_len);
        if (mz_uncompress(dest.data(), &dest_len,
                          (const unsigned char *) pdf.data() + data, (mz_ulong) len) != MZ_OK)
            continue;
        out.append((const char *) dest.data(), dest_len);
        at = data + (size_t) len;
    }
    return out;
}

/* `/Width 8` must not count as `/Width 80`. */
static bool pdf_has_number(const std::string &pdf, const std::string &key, int value) {
    const std::string token = key + std::to_string(value);
    for (size_t at = 0; (at = pdf.find(token, at)) != std::string::npos; ) {
        const size_t end = at + token.size();
        if (end >= pdf.size() || !std::isdigit((unsigned char) pdf[end]))
            return true;
        at = end;
    }
    return false;
}

/* ImageStore keeps the file bytes behind a shared pointer. */
static bool source_is(const ImageStore::Entry *e, const std::vector<uint8_t> &bytes) {
    return e && e->source && *e->source == bytes;
}

class TiffStub : public ImageDecoder {
public:
    const char *id() const override { return "tiff"; }
    bool sniff(const uint8_t *d, size_t n) const override {
        return n >= 4 && d[0] == 'I' && d[1] == 'I' && d[2] == '*' && d[3] == 0;
    }
    bool probe(const uint8_t *, size_t, ImageMetadata *meta, std::string *) const override {
        if (meta) meta->format = id();
        return true;
    }
    bool decode(const uint8_t *, size_t, DecodedImage *, std::string *error) const override {
        if (error) *error = "tiff stub does not decode";
        return false;
    }
};

/* A ligature that ends its run must map to every character it covers. */
static void test_tounicode(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 24;
    ParaStyle ps;
    std::vector<Frame> wide = {{0, 0, 400, 80}};
    Composition one = compose(one_para("fi", cs, ps), wide, fonts);
    size_t visible = 0;
    if (!one.lines.empty())
        for (const GlyphRun &r : one.lines[0].runs)
            for (const PlacedGlyph &g : r.glyphs)
                if (!(g.flags & PlacedGlyph::Invisible))
                    ++visible;
    if (visible != 1) {
        std::printf("  (skip: the Serif face does not ligate fi)\n");
        return;
    }

    auto exported = [&](Story story) {
        PageDoc doc;
        StoryId id = doc.add_story(std::move(story));
        doc.add_text_frame(0, id, 400, 80, Transform::translate(36, 36));
        std::vector<Composition> comps;
        comps.push_back(compose(doc.stories[0].story, doc.thread_frames(doc.stories[0]), fonts));
        const std::string path = "/tmp/pagemade_tounicode.pdf";
        CHECK(export_pdf(path, doc, comps), "export the ligature");
        return pdf_plain(read_bin(path));
    };

    const std::string alone = exported(one_para("fi", cs, ps));
    CHECK(alone.find("<00660069>") != std::string::npos,
          "a final fi ligature maps to both characters");

    Story split;
    Paragraph p;
    p.runs.push_back({cs, "fi"});
    CharStyle bold = cs;
    bold.bold = true;
    p.runs.push_back({bold, "sh"});
    split.paragraphs.push_back(p);
    const std::string both = exported(std::move(split));
    CHECK(both.find("<00660069>") != std::string::npos, "fi at the end of a run maps to fi");
    CHECK(both.find("<0066006900730068>") == std::string::npos,
          "fi stops before the next run");
}

/* Translucent color becomes an ExtGState. Opaque text does not. */
static void test_pdf_alpha(const FontLibrary &fonts) {
    auto exported = [&](float alpha) {
        CharStyle cs; cs.size = 12; cs.color.a = alpha;
        PageDoc doc;
        StoryId id = doc.add_story(one_para("Hi", cs, ParaStyle()));
        doc.add_text_frame(0, id, 200, 40, Transform::translate(36, 36));
        std::vector<Composition> comps;
        comps.push_back(compose(doc.stories[0].story, doc.thread_frames(doc.stories[0]), fonts));
        const std::string path = "/tmp/pagemade_alpha.pdf";
        CHECK(export_pdf(path, doc, comps), "export text at alpha %.3f", alpha);
        return pdf_plain(read_bin(path));
    };
    const std::string solid = exported(1.f);
    CHECK(solid.find("/ExtGState") == std::string::npos, "opaque text has no ExtGState");
    const std::string half = exported(0.5f);
    CHECK(half.find("/ExtGState") != std::string::npos, "half alpha uses an ExtGState");
    CHECK(half.find("/ca 0.5") != std::string::npos && half.find("/CA 0.5") != std::string::npos,
          "fill and stroke alpha are both 0.5");
    CHECK(half.find("/A500 gs\n") != std::string::npos, "half alpha selects /A500");
}

/* outline() may be held across later lookups. The cache must not move it. */
static void test_outline_cache(const FontLibrary &fonts) {
    CharStyle cs; cs.size = 12;
    Composition c = compose(one_para(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        cs, ParaStyle()), {{0, 0, 4000, 80}}, fonts);
    CHECK(!c.lines.empty() && !c.lines[0].runs.empty() && !c.lines[0].runs[0].glyphs.empty(),
          "the alphabet composes");
    if (c.lines.empty() || c.lines[0].runs.empty() || c.lines[0].runs[0].glyphs.empty())
        return;
    const Font *face = c.lines[0].runs[0].font;
    const uint32_t gid = c.lines[0].runs[0].glyphs[0].gid;
    const GlyphOutline &first = face->outline(gid);
    const GlyphOutline *p = &first;
    const size_t n = first.cmds.size();
    for (const GlyphRun &r : c.lines[0].runs)
        for (const PlacedGlyph &g : r.glyphs)
            face->outline(g.gid);
    CHECK(&face->outline(gid) == p, "an outline stays put while other glyphs are cached");
    CHECK(p->cmds.size() == n && n > 0, "the cached outline keeps its commands (%zu)", n);
}

/* BackgroundWorker: keyed jobs replace queued ones; drop_if takes them out. */
static void test_background_worker() {
    BackgroundWorker worker;
    std::mutex mu;
    std::condition_variable cv;
    bool go = false;
    std::vector<int> ran;
    std::atomic<int> done{0};
    auto job = [&](int n) {
        return [&, n] {
            std::lock_guard<std::mutex> lock(mu);
            ran.push_back(n);
            ++done;
        };
    };
    worker.post([&] {                    // hold the thread while the queue fills
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return go; });
    });
    worker.post(job(1), "a");
    worker.post(job(2), "a");            // replaces 1
    worker.post(job(3), "b");
    worker.post(job(4), "c");
    worker.drop_if([](const std::string &key) { return key == "c"; });
    CHECK(worker.queued("a") && worker.queued("b") && !worker.queued("c"),
          "queued jobs by key after replace and drop");
    {
        std::lock_guard<std::mutex> lock(mu);
        go = true;
    }
    cv.notify_all();
    for (int i = 0; i < 200 && done < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::lock_guard<std::mutex> lock(mu);
    CHECK(ran == std::vector<int>({2, 3}), "the newest job per key ran, in order");
}

static bool near_px(const uint8_t *p, int r, int g, int b, int tol) {
    return std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
}

static const float kP3Red[2] = {0.680f, 0.320f}, kP3Green[2] = {0.265f, 0.690f},
                   kP3Blue[2] = {0.150f, 0.060f}, kD65[2] = {0.3127f, 0.3290f};

#ifdef PAGEMADE_HAVE_HEIF
/* A solid w x h AVIF (lossless AV1; there is no HEVC encoder to count on),
 * tagged with `icc`, or with P3 nclx numbers when `icc` is empty. Empty
 * when no AV1 encoder is installed. */
static std::vector<uint8_t> make_avif(int w, int h, uint8_t r, uint8_t g, uint8_t b,
                                      const std::vector<uint8_t> &icc, int thumb = 0) {
    std::vector<uint8_t> out;
    heif_init(nullptr);
    heif_context *ctx = heif_context_alloc();
    heif_encoder *enc = nullptr;
    if (heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &enc).code != heif_error_Ok) {
        heif_context_free(ctx);
        return out;
    }
    heif_encoder_set_lossless(enc, 1);
    heif_encoder_set_parameter_string(enc, "chroma", "444");
    heif_image *img = nullptr;
    heif_image_create(w, h, heif_colorspace_RGB, heif_chroma_interleaved_RGB, &img);
    heif_image_add_plane(img, heif_channel_interleaved, w, h, 8);
    int stride = 0;
    uint8_t *px = heif_image_get_plane(img, heif_channel_interleaved, &stride);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *q = px + y * stride + x * 3;
            q[0] = r; q[1] = g; q[2] = b;
        }
    if (!icc.empty()) {
        heif_image_set_raw_color_profile(img, "prof", icc.data(), icc.size());
    } else {
        heif_color_profile_nclx *n = heif_nclx_color_profile_alloc();
        n->color_primaries = heif_color_primaries_SMPTE_EG_432_1;
        n->transfer_characteristics = heif_transfer_characteristic_IEC_61966_2_1;
        n->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
        n->full_range_flag = 1;
        heif_image_set_nclx_color_profile(img, n);
        heif_nclx_color_profile_free(n);
    }
    heif_image_handle *handle = nullptr;
    if (heif_context_encode_image(ctx, img, enc, nullptr, &handle).code == heif_error_Ok) {
        if (thumb > 0) {
            heif_image_handle *th = nullptr;
            heif_context_encode_thumbnail(ctx, img, handle, enc, nullptr, thumb, &th);
            if (th)
                heif_image_handle_release(th);
        }
        heif_image_handle_release(handle);
        heif_writer writer;
        writer.writer_api_version = 1;
        writer.write = [](heif_context *, const void *data, size_t size, void *user) {
            auto *v = static_cast<std::vector<uint8_t> *>(user);
            v->insert(v->end(), (const uint8_t *) data, (const uint8_t *) data + size);
            return heif_error{heif_error_Ok, heif_suberror_Unspecified, ""};
        };
        heif_context_write(ctx, &writer, &out);
    }
    heif_image_release(img);
    heif_encoder_release(enc);
    heif_context_free(ctx);
    return out;
}
#endif

static void test_image_color(const FontLibrary &fonts) {
    (void) fonts;
    /* Display P3 from its xy numbers comes out as Apple's profile does. */
    std::vector<uint8_t> p3 = icc_from_primaries(kP3Red, kP3Green, kP3Blue, kD65,
                                                 Transfer::SRGB, "Display P3");
    CHECK(icc_is_rgb(p3), "the P3 profile is an RGB ICC profile");
    CHECK(icc_needs_conversion(p3), "P3 differs from sRGB on screen");
    const float sr[2] = {0.64f, 0.33f}, sg[2] = {0.30f, 0.60f}, sb[2] = {0.15f, 0.06f};
    CHECK(icc_from_primaries(sr, sg, sb, kD65, Transfer::SRGB, "sRGB").empty(),
          "sRGB numbers need no profile");

    /* lcms2 (transicc, relative colorimetric) gives 214.9 92.6 30.9. */
    uint8_t px[12] = {200, 100, 50, 128,  255, 0, 0, 255,  128, 128, 128, 255};
    icc_to_srgb(p3, px, 3);
    CHECK(near_px(px, 215, 93, 31, 1) && px[3] == 128, "P3 to sRGB (%d %d %d), alpha kept",
          px[0], px[1], px[2]);
    CHECK(near_px(px + 4, 255, 0, 0, 0), "P3 red clips to sRGB red");
    CHECK(near_px(px + 8, 128, 128, 128, 1), "gray stays gray");

    std::vector<uint8_t> cmyk = p3;
    std::memcpy(cmyk.data() + 16, "CMYK", 4);
    uint8_t same[4] = {200, 100, 50, 255};
    icc_to_srgb(cmyk, same, 1);
    CHECK(!icc_is_rgb(cmyk) && near_px(same, 200, 100, 50, 0), "a CMYK profile is left alone");
    uint8_t junk_px[4] = {200, 100, 50, 255};
    icc_to_srgb(std::vector<uint8_t>(40, 7), junk_px, 1);
    CHECK(near_px(junk_px, 200, 100, 50, 0), "a broken profile is left alone");

    std::string jpeg;
    std::vector<uint8_t> flat(16 * 8 * 4, 200);
    CHECK(encode_jpeg(flat.data(), 16, 8, 90, &jpeg) && jpeg.size() > 100 &&
          (uint8_t) jpeg[0] == 0xFF && (uint8_t) jpeg[1] == 0xD8, "encode_jpeg writes a JPEG");

#ifdef PAGEMADE_HAVE_HEIF
    CHECK(image_extension_supported(".HEIC") && image_extension_supported("avif"),
          "HEIC and AVIF are offered");
    std::vector<uint8_t> avif = make_avif(64, 32, 200, 100, 50, p3);
    if (avif.empty()) {
        std::printf("  heif: no AV1 encoder, skipping the HEIF file tests\n");
        return;
    }
    ImageMetadata meta;
    std::string err;
    CHECK(image_loader().probe(avif.data(), avif.size(), &meta, &err), "probe avif: %s", err.c_str());
    CHECK(meta.format == "avif" && meta.width_px == 64 && meta.height_px == 32 &&
          meta.model == ColorModel::RGB && meta.orientation == 1, "avif %s %dx%d",
          meta.format.c_str(), meta.width_px, meta.height_px);
    CHECK(meta.icc == p3, "the ICC profile is read from the file");
    DecodedImage full;
    CHECK(image_loader().decode(avif.data(), avif.size(), &full, &err), "decode avif: %s", err.c_str());
    CHECK(full.width == 64 && full.height == 32 && near_px(full.rgba.data(), 200, 100, 50, 2),
          "decode keeps the file's own P3 samples (%d %d %d)",
          full.rgba[0], full.rgba[1], full.rgba[2]);
    std::vector<uint8_t> shown;
    int dw = 0, dh = 0;
    CHECK(make_display(avif.data(), avif.size(), 0, &shown, &dw, &dh, &err) && dw == 64,
          "display view of avif");
    CHECK(near_px(shown.data(), 215, 93, 31, 2), "the screen gets sRGB (%d %d %d)",
          shown[0], shown[1], shown[2]);

    std::vector<uint8_t> nclx = make_avif(16, 16, 200, 100, 50, {});
    CHECK(image_loader().probe(nclx.data(), nclx.size(), &meta, &err), "probe nclx avif: %s", err.c_str());
    CHECK(icc_needs_conversion(meta.icc), "P3 nclx numbers become a profile");
    CHECK(make_display(nclx.data(), nclx.size(), 0, &shown, &dw, &dh, &err) &&
          near_px(shown.data(), 215, 93, 31, 2), "nclx P3 is shown as sRGB (%d %d %d)",
          shown[0], shown[1], shown[2]);

    /* libheif turns the picture upright (irot/imir) while decoding, so the
     * probed size must be the decoded one and EXIF orientation is unused.
     * (libheif 1.17's AV1 encoder doesn't write irot, so a rotated file
     * can't be made here.) */
    CHECK(image_loader().probe(avif.data(), avif.size(), &meta, &err) &&
          meta.width_px == full.width && meta.height_px == full.height && meta.orientation == 1,
          "probe size is the upright decoded size");

    /* Thumbnails come from the embedded preview when it is big enough. */
    std::vector<uint8_t> with_thumb = make_avif(256, 128, 200, 100, 50, p3, 64);
    CHECK(make_display(with_thumb.data(), with_thumb.size(), 48, &shown, &dw, &dh, &err) &&
          std::max(dw, dh) == 48 && near_px(shown.data(), 215, 93, 31, 3),
          "a small view from the preview (%dx%d)", dw, dh);
    DecodedImage view;
    CHECK(image_loader().decode_preview(with_thumb.data(), with_thumb.size(), 48, &view, &err) &&
          view.width == 64 && view.height == 32, "the 64 px preview is decoded, not the photo (%dx%d)",
          view.width, view.height);
    CHECK(image_loader().decode_preview(with_thumb.data(), with_thumb.size(), 100, &view, &err) &&
          view.width == 256, "a bigger view decodes the photo (%d)", view.width);

    /* PDF: re-encoded as JPEG, tagged with the profile, written once. */
    ImageStore store;
    PageDoc doc;
    for (int i = 0; i < 2; ++i) {
        const uint32_t id = store.take_id();
        CHECK(store.add(id, avif, &meta, &err), "store avif: %s", err.c_str());
        ImageAsset asset;
        asset.id = id; asset.name = "photo.avif"; asset.meta = meta;
        doc.add_image(std::move(asset));
        Item it;
        it.xf = Transform::translate(80.f * i, 0); it.w = 64; it.h = 32;
        it.content = PlacedImage{id, 0, 0, 64, 32};
        doc.add_item(0, it);
    }
    const std::string pdf_path = "/tmp/pagemade-heif.pdf";
    std::vector<Composition> comps;
    CHECK(export_pdf(pdf_path, doc, comps, &store), "export avif");
    std::string plain = pdf_plain(read_bin(pdf_path));
    CHECK(plain.find("/DCTDecode") != std::string::npos, "HEIF goes into the PDF as JPEG");
    CHECK(plain.find("/ColorSpace [/ICCBased") != std::string::npos, "with its ICC profile");
    size_t profiles = 0;
    for (size_t at = plain.find("/N 3 /Alternate /DeviceRGB"); at != std::string::npos;
         at = plain.find("/N 3 /Alternate /DeviceRGB", at + 1))
        ++profiles;
    CHECK(profiles == 1, "one profile shared by both pictures (%zu)", profiles);
    CHECK(pdf_has_number(plain, "/Width ", 64), "full-resolution width");
#endif
}

static void test_groups(const FontLibrary &fonts) {
    PageDoc doc = new_publication(PageSetup(), 2);
    auto box = [&](size_t page, float x) {
        Item it;
        it.w = 20; it.h = 20;
        it.xf = Transform::translate(x, 0);
        it.content = Shape();
        return doc.add_item(page, it);
    };
    const ItemId a = box(0, 0), b = box(0, 30), c = box(0, 60), d = box(0, 90), other = box(1, 0);
    CHECK(doc.group_members(a) == std::vector<ItemId>{a}, "an ungrouped item is its own group");
    CHECK(doc.group_items({a}) == 0, "one item can't be grouped");
    CHECK(doc.group_items({a, other}) == 0, "items on two pages can't be grouped");

    const uint32_t g = doc.group_items({a, c});
    CHECK(g != 0, "two items group");
    CHECK(doc.group_members(c) == (std::vector<ItemId>{a, c}), "members in stacking order");
    /* a and c are pulled together where c (the topmost) was: b, a, c, d */
    std::vector<ItemId> order;
    for (const Item &it : doc.pages[0].items)
        order.push_back(it.id);
    CHECK(order == (std::vector<ItemId>{b, a, c, d}), "the group is adjacent in the stack");

    const uint32_t g2 = doc.group_items({c, d});
    CHECK(g2 != 0 && g2 != g && doc.group_members(a).size() == 3,
          "grouping a member with another item takes in its whole group");

    const std::string path = "/tmp/pagemade-groups.pagemade";
    std::string err;
    CHECK(save_document(doc, fonts, path, SaveOptions{false}, &err), "save groups: %s", err.c_str());
    OpenResult back = open_document(path, fonts);
    CHECK(back.ok && back.doc.group_members(d).size() == 3, "groups survive saving");
    if (back.ok) {
        const uint32_t g3 = back.doc.group_items({b, other});
        CHECK(g3 == 0, "still one page per group after opening");
        Item *ib = back.doc.find_item(b);
        Item *ia = back.doc.find_item(a);
        CHECK(ia && ib && back.doc.next_id > ia->group, "new ids come after saved groups");
    }

    doc.ungroup_items({a});
    CHECK(doc.group_members(c).size() == 1 && doc.find_item(d)->group == 0,
          "ungroup takes the whole group apart");

    doc.group_items({a, b});
    doc.remove_item(a);
    doc.tidy_groups();
    CHECK(doc.find_item(b)->group == 0, "a group left with one item ends");
}

static void test_page_markers(const FontLibrary &fonts) {
    PageDoc doc = new_publication(PageSetup(), 3);
    const std::string marked = std::string("Page ") + "\xEE\x80\x80" + " of " + "\xEE\x80\x81";
    auto story_on = [&](size_t page, const std::string &text) {
        Paragraph p;
        p.runs.push_back({CharStyle(), text});
        Story s;
        s.paragraphs.push_back(p);
        const StoryId sid = doc.add_story(s);
        doc.add_text_frame(page, sid, 300, 100, Transform::translate(72, 72));
        return sid;
    };
    /* Ids, not pointers: adding a story can move the others */
    const StoryId with_id = story_on(1, marked);
    const StoryId plain_id = story_on(1, "Page 2 of 3");
    const StoryEntry *with = doc.find_story(with_id);
    const StoryEntry *plain = doc.find_story(plain_id);
    Composition a = compose(with->story, doc.thread_frames(*with), fonts);
    Composition b = compose(plain->story, doc.thread_frames(*plain), fonts);
    auto gids = [](const Composition &c) {
        std::vector<uint32_t> out;
        for (const ComposedLine &l : c.lines)
            for (const GlyphRun &r : l.runs)
                for (const PlacedGlyph &g : r.glyphs)
                    out.push_back(g.gid);
        return out;
    };
    CHECK(!a.lines.empty() && gids(a) == gids(b), "markers on page 2 of 3 read \"Page 2 of 3\"");
    CHECK(!a.lines.empty() && a.lines[0].byte_end == marked.size(),
          "the line still covers the markers' bytes");
    /* Both digits of a marker sit on its one character */
    uint32_t marker_at = 5, glyphs_on_marker = 0;
    for (const GlyphRun &r : a.lines[0].runs)
        for (const PlacedGlyph &g : r.glyphs)
            glyphs_on_marker += g.cluster == marker_at;
    CHECK(glyphs_on_marker == 1, "page 2 is one glyph on the marker (%u)", glyphs_on_marker);

    Composition none = compose(with->story, {Frame{0, 0, 300, 100}}, fonts);
    Composition hashes = compose(plain->story, {Frame{0, 0, 300, 100}}, fonts);
    Paragraph hp;
    hp.runs.push_back({CharStyle(), "Page # of #"});
    Story hs;
    hs.paragraphs.push_back(hp);
    CHECK(gids(none) == gids(compose(hs, {Frame{0, 0, 300, 100}}, fonts)),
          "without a page the markers show #");
    (void) hashes;

    const std::string pdf_path = "/tmp/pagemade-markers.pdf";
    std::vector<Composition> comps;
    for (const StoryEntry &se : doc.stories)
        comps.push_back(compose(se.story, doc.thread_frames(se), fonts));
    CHECK(export_pdf(pdf_path, doc, comps, nullptr), "export markers");
    if (FILE *pipe = popen(("pdftotext -f 2 -l 2 " + pdf_path + " - 2>/dev/null").c_str(), "r")) {
        std::string out;
        char buf[256];
        while (std::fgets(buf, sizeof buf, pipe))
            out += buf;
        if (pclose(pipe) == 0)
            CHECK(out.find("Page 2 of 3") != std::string::npos &&
                  out.find("\xEE\x80") == std::string::npos,
                  "the PDF copies as digits: %s", out.c_str());
        else
            std::printf("  markers: pdftotext not available, skipped\n");
    }
}

static void test_layers(const FontLibrary &fonts) {
    PageDoc doc = new_publication(PageSetup(), 1);
    CHECK(doc.layers.size() == 1 && doc.active_layer == doc.layers[0].id, "one Default layer");
    const LayerId base = doc.layers[0].id;
    auto box = [&](float x) {
        Item it;
        it.w = 10; it.h = 10;
        it.xf = Transform::translate(x, 0);
        it.content = Shape();
        return doc.add_item(0, it);
    };
    auto order = [&] {
        std::vector<ItemId> o;
        for (const Item &it : doc.pages[0].items)
            o.push_back(it.id);
        return o;
    };
    const ItemId a = box(0);
    const LayerId top = doc.add_layer("Top");
    CHECK(top && doc.active_layer == top && doc.layers.back().id == top, "a new layer goes above, active");
    const ItemId b = box(20);
    doc.active_layer = base;
    const ItemId c = box(40);                // added last, on the lower layer
    CHECK(order() == (std::vector<ItemId>{a, c, b}), "a lower layer's item stays below");
    doc.bring_to_front(a);
    CHECK(order() == (std::vector<ItemId>{c, a, b}), "Bring to Front stays within the layer");
    doc.move_layer(top, -1);
    CHECK(order() == (std::vector<ItemId>{b, c, a}), "moving a layer down moves its items");
    doc.move_layer(top, 1);

    for (int i = 0; i < 10; ++i)
        doc.add_layer("More");
    CHECK(doc.layers.size() == kMaxLayers, "at most 8 layers (%zu)", doc.layers.size());

    doc.find_layer(top)->visible = false;
    DrawList list = build_page(doc, 0, {});
    CHECK(list.size() == 2 && !doc.item_editable(*doc.find_item(b)), "a hidden layer isn't drawn");
    doc.find_layer(top)->visible = true;
    doc.find_layer(base)->locked = true;
    CHECK(!doc.item_editable(*doc.find_item(a)) && doc.item_visible(*doc.find_item(a)),
          "a locked layer is drawn but not editable");
    doc.find_layer(base)->locked = false;

    doc.set_items_layer({c}, top);
    CHECK(doc.find_item(c)->layer == top && order().back() == c, "moved items go on top of the layer");

    const std::string path = "/tmp/pagemade-layers.pagemade";
    doc.find_layer(top)->locked = true;
    doc.active_layer = top;
    std::string err;
    CHECK(save_document(doc, fonts, path, SaveOptions{false}, &err), "save layers: %s", err.c_str());
    OpenResult back = open_document(path, fonts);
    CHECK(back.ok && back.doc.layers.size() == doc.layers.size() && back.doc.active_layer == top &&
          back.doc.find_layer(top)->locked && back.doc.find_item(c)->layer == top,
          "layers survive saving");

    /* The extra layers went in above the active Default, so Top is last */
    const LayerId below = doc.layers[doc.layer_index(top) - 1].id;
    CHECK(doc.remove_layer(top), "a layer can be deleted");
    CHECK(doc.find_item(b)->layer == below && doc.find_item(c)->layer == below &&
          order() == (std::vector<ItemId>{a, b, c}), "its items go on top of the layer below");

    doc.find_layer(base)->locked = false;
    const LayerId other = doc.layers.back().id;
    doc.set_items_layer({b}, other);
    doc.group_items({a, b});
    CHECK(doc.find_item(a)->layer == doc.find_item(b)->layer, "a group is on one layer");
}

static std::string run_cmd(const std::string &cmd, bool *ok) {
    std::string out;
    *ok = false;
    if (FILE *pipe = popen((cmd + " 2>/dev/null").c_str(), "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof buf, pipe))
            out += buf;
        *ok = pclose(pipe) == 0;
    }
    return out;
}

static void test_document_setup(const FontLibrary &fonts) {
    (void) fonts;
    PageSetup letter;                       // 612 x 792, 54 margins, 2 columns, 18 gutter
    PageDoc doc = new_publication(letter, 1);
    /* A block filling column 2, a picture in column 1, a box on the right margin */
    const float col = (612.f - 108.f - 18.f) / 2.f;      // 243
    Paragraph para;
    para.runs.push_back({CharStyle(), "Column two"});
    Story st;
    st.paragraphs.push_back(para);
    const StoryId sid = doc.add_story(st);
    const ItemId text = doc.add_text_frame(0, sid, col, 300, Transform::translate(54 + col + 18, 100));
    Item pic;
    pic.w = col; pic.h = col * 0.5f;
    pic.xf = Transform::translate(54, 100);
    pic.content = PlacedImage{1, 0, 0, col, col * 0.5f};
    const ItemId picture = doc.add_item(0, pic);
    Item box;
    box.w = 20; box.h = 20;
    box.xf = Transform::translate(612 - 54 - 20, 700);
    box.content = Shape();
    const ItemId edge = doc.add_item(0, box);
    doc.guides.push_back({true, 54 + col});              // column 1's right edge

    PageDoc adjusted = doc;
    PageSetup a4 = letter;
    a4.width = 595.28f; a4.height = 841.89f;
    a4.margin_inside = a4.margin_outside = 36.f;
    a4.gutter = 12.f;
    change_setup(adjusted, a4, SetupAdjust::Adjust);
    const float ncol = (595.28f - 72.f - 12.f) / 2.f;
    const Item *t = adjusted.find_item(text);
    CHECK(t && std::fabs(t->xf.e - (36 + ncol + 12)) < 0.05f && std::fabs(t->w - ncol) < 0.05f,
          "Adjust: a block filling column 2 fills the new column 2 (x %.2f w %.2f)",
          t ? t->xf.e : 0.f, t ? t->w : 0.f);
    /* Between the top and bottom margins, positions stretch with the live area */
    const float stretch = (841.89f - 108.f) / (792.f - 108.f);
    CHECK(t && std::fabs(t->h - 300.f * stretch) < 0.05f && std::fabs(t->xf.f - (54 + 46 * stretch)) < 0.05f,
          "Adjust: its height stretches with the live area (%.2f)", t ? t->h : 0.f);
    const Item *p = adjusted.find_item(picture);
    CHECK(p && std::fabs(p->w - ncol) < 0.05f && std::fabs(p->h / p->w - 0.5f) < 0.001f &&
          std::fabs(p->image()->w - p->w) < 0.05f,
          "Adjust: a picture fills its column and keeps its proportions");
    const Item *e = adjusted.find_item(edge);
    CHECK(e && std::fabs(e->xf.e + e->w - (595.28f - 36.f)) < 0.05f,
          "Adjust: a box on the right margin stays on it (%.2f)", e ? e->xf.e + e->w : 0.f);
    CHECK(std::fabs(adjusted.guides[0].pos - (36 + ncol)) < 0.05f, "Adjust: a ruler guide follows");

    PageDoc scaled = doc;
    PageSetup half = letter;
    half.width = 306; half.height = 396;
    change_setup(scaled, half, SetupAdjust::Scale);
    CHECK(std::fabs(scaled.stories.back().story.paragraphs[0].runs[0].style.size - 6.f) < 0.01f,
          "Scale: type scales with the page");
    const Item *ts = scaled.find_item(text);
    CHECK(ts && std::fabs(ts->w - col * 0.5f) < 0.05f && std::fabs(ts->xf.e - (54 + col + 18) * 0.5f) < 0.05f,
          "Scale: blocks scale and move with it");

    PageDoc left = doc;
    change_setup(left, a4, SetupAdjust::Leave);
    CHECK(left.find_item(text)->xf.e == doc.find_item(text)->xf.e && left.setup.width == a4.width,
          "Leave: only the setup changes");
}

static void test_pdf_settings(const FontLibrary &fonts) {
    PageDoc doc = new_publication(PageSetup(), 3);
    for (size_t i = 0; i < 3; ++i) {
        Paragraph para;
        para.runs.push_back({CharStyle(), "Page " + std::to_string(i + 1)});
        Story st;
        st.paragraphs.push_back(para);
        const StoryId sid = doc.add_story(st);
        doc.add_text_frame(i, sid, 200, 50, Transform::translate(72, 72));
    }
    /* A 600 x 300 picture placed one inch wide: 600 ppi */
    ImageStore store;
    std::vector<uint8_t> big = make_png(600, 300, nullptr, 0, 0);
    ImageMetadata meta;
    std::string err;
    const uint32_t id = store.take_id();
    CHECK(store.add(id, big, &meta, &err), "store png: %s", err.c_str());
    ImageAsset asset;
    asset.id = id; asset.name = "big.png"; asset.meta = meta;
    doc.add_image(asset);
    Item pic;
    pic.w = 72; pic.h = 36;
    pic.xf = Transform::translate(72, 200);
    pic.content = PlacedImage{id, 0, 0, 72, 36};
    doc.add_item(0, pic);
    doc.setup.bleed = 9.f;
    auto comps_of = [&] {
        std::vector<Composition> comps;
        for (const StoryEntry &se : doc.stories)
            comps.push_back(compose(se.story, doc.thread_frames(se), fonts));
        return comps;
    };
    const std::vector<Composition> comps = comps_of();
    const std::string path = "/tmp/pagemade-settings.pdf";

    PdfSettings o;
    o.all_pages = false;
    o.first_page = 2;
    o.last_page = 3;
    o.bleed = false;
    CHECK(export_pdf(path, doc, comps, &store, &o), "export a range");
    std::string plain = pdf_plain(read_bin(path));
    size_t count = 0;
    for (size_t at = plain.find("/Type /Page "); at != std::string::npos; at = plain.find("/Type /Page ", at + 1))
        ++count;
    CHECK(count == 2, "pages 2-3 are two pages (%zu)", count);

    o = PdfSettings();
    o.downsample_above = 450.f;
    o.downsample_to = 150.f;
    o.crop_marks = true;
    CHECK(export_pdf(path, doc, comps, &store, &o), "export downsampled, with bleed and marks");
    plain = pdf_plain(read_bin(path));
    CHECK(pdf_has_number(plain, "/Width ", 150) && !pdf_has_number(plain, "/Width ", 600),
          "a 600 ppi picture is resampled to 150 ppi (150 px)");
    /* The sheet: bleed 9, then 3 + 15 + 3 for the marks */
    CHECK(plain.find("/TrimBox [30 30 642 822]") != std::string::npos,
          "bleed 9 and marks: the trim sits 30 pt in");
    CHECK(plain.find("/BleedBox [21 21 651 831]") != std::string::npos &&
          plain.find("/MediaBox [0 0 672 852]") != std::string::npos, "the bleed and media boxes");

    o = PdfSettings();
    o.pdfx4 = true;
    o.title = "Holiday book";
    o.author = "R\xC3\xA9my";
    CHECK(export_pdf(path, doc, comps, &store, &o), "export PDF/X-4");
    const std::string raw = read_bin(path);
    plain = pdf_plain(raw);
    CHECK(raw.rfind("%PDF-1.6", 0) == 0, "PDF/X-4 is PDF 1.6");
    CHECK(plain.find("/S /GTS_PDFX") != std::string::npos &&
          plain.find("/OutputIntents") != std::string::npos, "with an output intent");
    CHECK(plain.find("<pdfxid:GTS_PDFXVersion>PDF/X-4</pdfxid:GTS_PDFXVersion>") != std::string::npos &&
          plain.find("/Trapped /False") != std::string::npos, "identified as PDF/X-4");
    CHECK(plain.find("/CS0 cs") != std::string::npos && plain.find(" rg\n") == std::string::npos,
          "every color is tagged");
    CHECK(plain.find("/Interpolate") == std::string::npos, "no image interpolation in PDF/X");
    CHECK(plain.find("/Title (Holiday book)") != std::string::npos &&
          plain.find("/Author <FEFF") != std::string::npos, "title, and a non-ASCII author");
    bool ok = false;
    const std::string info = run_cmd("pdfinfo " + path, &ok);
    if (ok)
        CHECK(info.find("Pages:           3") != std::string::npos &&
              info.find("Holiday book") != std::string::npos, "poppler reads it: %s", info.c_str());

    std::vector<std::string> warn = pdf_preflight(doc, comps, &store, PdfSettings());
    bool odd = false, low = false;
    for (const std::string &w : warn) {
        odd = odd || w.find("even page count") != std::string::npos;
        low = low || w.find("ppi") != std::string::npos;
    }
    CHECK(odd && !low, "preflight: three pages is odd; 600 ppi is fine");
    Item *p = &doc.pages[0].items.back();
    p->w = 2000; p->content = PlacedImage{id, 0, 0, 2000, 1000};
    warn = pdf_preflight(doc, comps_of(), &store, PdfSettings());
    low = false;
    for (const std::string &w : warn)
        low = low || w.find("big.png is 22 ppi") != std::string::npos;
    CHECK(low, "preflight: a picture blown up to 22 ppi is reported");
    const std::vector<PdfOutputProfile> profiles = pdf_output_profiles();
    CHECK(!profiles.empty() && profiles[0].path.empty(), "sRGB is always an output intent");
    std::printf("  pdf: %zu output intent profiles\n", profiles.size());
}

static void test_strict_loose(const FontLibrary &fonts) {
    /* Justified lines at a width where word spaces run out of stretch on
     * some lines and letter spacing takes up the rest */
    Paragraph p;
    p.style.align = Align::Justify;
    p.style.hyphenate = false;
    p.runs.push_back({CharStyle(), "Justified type uses spacing attributes. Word spaces may "
                                    "shrink or stretch; when that isn't enough, letter spacing "
                                    "takes up the slack. Narrow measures show it best of all."});
    Story st;
    st.paragraphs.push_back(p);
    bool word_only = false, any_strict = false;
    for (float w = 90.f; w <= 200.f; w += 5.f) {
        Composition c = compose(st, {Frame{0, 0, w, 2000}}, fonts);
        for (const ComposedLine &l : c.lines) {
            CHECK(!l.loose || l.word_loose, "loose implies word-loose (w %.0f)", w);
            CHECK(!l.tight || l.word_tight, "tight implies word-tight (w %.0f)", w);
            word_only = word_only || (l.word_loose && !l.loose);
            any_strict = any_strict || l.word_loose;
        }
    }
    CHECK(any_strict && word_only,
          "some lines pass the word-space maximum while letter spacing absorbs the rest");
    /* Widening the word-space maximum leaves fewer strict-loose lines */
    st.paragraphs[0].style.word_max = 400.f;
    size_t wide = 0, narrow = 0;
    Composition a = compose(st, {Frame{0, 0, 120, 2000}}, fonts);
    for (const ComposedLine &l : a.lines) wide += l.word_loose;
    st.paragraphs[0].style.word_max = 150.f;
    Composition b = compose(st, {Frame{0, 0, 120, 2000}}, fonts);
    for (const ComposedLine &l : b.lines) narrow += l.word_loose;
    CHECK(wide < narrow, "a larger word maximum flags fewer lines (%zu < %zu)", wide, narrow);
}

static void test_contents_tabs(const FontLibrary &fonts) {
    /* The right edge of the last visible glyph on the first line */
    auto right_end = [&](const Story &st, float w, size_t *leaders = nullptr) {
        Composition c = compose(st, {Frame{0, 0, w, 500}}, fonts);
        float end = 0.f;
        size_t inserted = 0;
        if (!c.lines.empty())
            for (const GlyphRun &r : c.lines[0].runs)
                for (const PlacedGlyph &g : r.glyphs) {
                    if (g.flags & PlacedGlyph::Invisible)
                        continue;
                    end = std::max(end, g.x + g.adv);
                    inserted += (g.flags & PlacedGlyph::Inserted) != 0;
                }
        if (leaders)
            *leaders = inserted;
        return end;
    };
    Paragraph p;
    p.style.tabs = {{0, TabAlign::Right, ".", true}};
    p.style.right_indent = 6;
    p.runs.push_back({CharStyle(), "Threaded blocks\t12"});
    Story st;
    st.paragraphs.push_back(p);
    size_t dots_narrow = 0, dots_wide = 0;
    const float at200 = right_end(st, 200, &dots_narrow), at320 = right_end(st, 320, &dots_wide);
    CHECK(std::fabs(at200 - 194.f) < 0.6f && std::fabs(at320 - 314.f) < 0.6f,
          "a right-anchored stop puts the number at the right indent (%.1f, %.1f)", at200, at320);
    CHECK(dots_narrow > 3 && dots_wide > dots_narrow, "the leader fills the gap (%zu, %zu dots)",
          dots_narrow, dots_wide);

    /* Shift+Tab's right-indent tab: no stops needed, the right stop's leader */
    Paragraph q;
    q.runs.push_back({CharStyle(), "Rotated blocks\xEE\x80\x82" "7"});
    Story qs;
    qs.paragraphs.push_back(q);
    size_t none = 0;
    const float rit = right_end(qs, 250, &none);
    CHECK(std::fabs(rit - 250.f) < 0.6f && none == 0,
          "a right-indent tab jumps to the right indent, no leader without a stop (%.1f)", rit);
    qs.paragraphs[0].style.tabs = {{100, TabAlign::Right, ".", false}};
    size_t dots = 0;
    right_end(qs, 250, &dots);
    CHECK(dots > 3, "with a right stop it takes that stop's leader (%zu)", dots);

    /* Saved and read back */
    PageDoc doc = new_publication(PageSetup(), 1);
    const StyleDef *contents = doc.find_style("Contents");
    CHECK(contents && contents->para.tabs.size() == 1 && contents->para.tabs[0].from_right,
          "new publications have a Contents style with a right-side stop");
    const StoryId sid = doc.add_story(st);
    doc.add_text_frame(0, sid, 200, 100, Transform::translate(72, 72));
    std::string err;
    const std::string path = "/tmp/pagemade-tabs.pagemade";
    CHECK(save_document(doc, fonts, path, SaveOptions{false}, &err), "save tabs: %s", err.c_str());
    OpenResult back = open_document(path, fonts);
    CHECK(back.ok && back.doc.stories.back().story.paragraphs[0].style.tabs[0].from_right,
          "from_right survives saving");
}

static void test_images(const FontLibrary &fonts) {
    CHECK(std::fabs(image_print_points(300, 300) - 72.f) < 0.01f, "300 px at 300 ppi is 72 pt");
    CHECK(std::fabs(image_print_points(100, 0) - 100.f) < 0.01f, "unspecified resolution is 72 ppi");

    uint8_t px[2 * 2 * 3] = {
        255, 0, 0,  0, 255, 0,
        0, 0, 255,  255, 255, 0
    };
    const uint32_t ppm300 = 11811;       // 11811 * 0.0254 ≈ 299.999
    std::vector<uint8_t> png = make_png(2, 2, px, ppm300, ppm300);
    ImageMetadata meta;
    std::string err;
    CHECK(image_loader().probe(png.data(), png.size(), &meta, &err), "probe png: %s", err.c_str());
    CHECK(meta.format == "png" && meta.width_px == 2 && meta.height_px == 2, "png size");
    CHECK(meta.model == ColorModel::RGB && meta.channels == 3 && !meta.alpha, "png is RGB");
    CHECK(std::fabs(meta.ppi_x - 300.0) < 0.1 && std::fabs(meta.ppi_y - 300.0) < 0.1,
          "png ppi %.3f", meta.ppi_x);
    DecodedImage decoded;
    CHECK(image_loader().decode(png.data(), png.size(), &decoded, &err), "decode png: %s", err.c_str());
    CHECK(decoded.width == 2 && decoded.rgba.size() == 16 && decoded.rgba[0] == 255 &&
          decoded.rgba[2] == 0 && decoded.rgba[8] == 0 && decoded.rgba[10] == 255,
          "png pixels come back upright");

    std::vector<uint8_t> jpeg = make_jpeg_header(20, 8, 300);
    CHECK(image_loader().probe(jpeg.data(), jpeg.size(), &meta, &err), "probe jpeg: %s", err.c_str());
    CHECK(meta.format == "jpeg" && meta.orientation == 6, "jpeg orientation 6 (%d)", meta.orientation);
    CHECK(meta.stored_width_px == 20 && meta.stored_height_px == 8, "jpeg stored size");
    CHECK(meta.width_px == 8 && meta.height_px == 20, "orientation 6 swaps the size");
    CHECK(std::fabs(meta.ppi_x - 300.0) < 0.1, "jfif density %.3f", meta.ppi_x);

    {
        ImageLoader loader;
        register_builtin_decoders(loader);
        loader.add(std::make_unique<TiffStub>());
        const uint8_t tiff[] = {'I', 'I', '*', 0, 1, 2, 3, 4};
        const uint8_t nope[] = {'n', 'o', 'p', 'e'};
        ImageMetadata m;
        CHECK(loader.probe(png.data(), png.size(), &m, &err) && m.format == "png",
              "png still matches the png decoder");
        CHECK(loader.probe(tiff, sizeof tiff, &m, &err) && m.format == "tiff",
              "tiff magic matches the registered decoder");
        CHECK(!loader.probe(nope, sizeof nope, &m, &err), "an unknown file is declined");
        CHECK(!loader.decode(tiff, sizeof tiff, &decoded, &err), "the tiff stub does not decode");
    }

    {
        std::vector<uint8_t> wide = make_png(32, 8, nullptr, 0, 0);
        ImageStore store;
        const uint32_t id = store.take_id();
        const std::vector<uint8_t> source = wide;
        CHECK(store.add(id, std::move(wide), &meta, &err), "store add: %s", err.c_str());
        const uint8_t *rgba = nullptr;
        int w = 0, h = 0;
        CHECK(store.display(id, 8, &rgba, &w, &h), "display proxy");
        CHECK(std::max(w, h) <= 8 && std::max(w, h) <= 32 && w > 0 && h > 0,
              "proxy long edge %d is within the request and the source", std::max(w, h));
        CHECK(source_is(store.find(id), source), "display leaves the source bytes");
        ImageStore::Bytes copied = store.source_of(id);
        CHECK(copied && *copied == source, "source_of copies the bytes for another thread");
        const int kept_w = w, kept_h = h;
        CHECK(store.display(id, 4, &rgba, &w, &h) && w == kept_w && h == kept_h,
              "a smaller request keeps the proxy already built");
    }

    {
        PlacedImage im;
        im.x = 0; im.y = 0; im.w = 100; im.h = 40;
        PlacedImage scaled = scale_placement(im, 50, 20, 100, 40);
        CHECK(near(scaled.x, 0) && near(scaled.y, 0) && near(scaled.w, 200) && near(scaled.h, 80),
              "scale keeps the crop's fraction of the picture");
        PlacedImage cropped = crop_placement(im, 20, 5);
        CHECK(near(cropped.x, -20) && near(cropped.y, -5) && near(cropped.w, 100) && near(cropped.h, 40),
              "crop leaves the picture where it was");
    }

    {
        PageDoc doc;
        Item it;
        it.w = 40; it.h = 20;
        it.xf = Transform::translate(3, 4);
        PlacedImage im;
        im.asset = 7; im.x = -5; im.y = 1; im.w = 80; im.h = 40;
        it.content = im;
        doc.add_item(0, std::move(it));
        DrawList list = build_page(doc, 0, {});
        const DrawImage *d = list.size() == 1 ? std::get_if<DrawImage>(&list[0]) : nullptr;
        CHECK(d && d->asset == 7 && near(d->x, -5) && near(d->y, 1) && near(d->w, 80) &&
              near(d->h, 40) && near(d->clip_w, 40) && near(d->clip_h, 20) &&
              near(d->xf.e, 3) && near(d->xf.f, 4), "build_page emits the placement");
    }

    {
        ImageStore store;
        PageDoc doc;
        const uint32_t id = store.take_id();
        const std::vector<uint8_t> kept = make_png(2, 2, px, 0, 0);
        CHECK(store.add(id, kept, &meta, &err), "asset bytes");
        ImageAsset asset;
        asset.id = id;
        asset.name = "two.png";
        doc.add_image(asset);
        Item a, b;
        a.w = a.h = b.w = b.h = 10;
        a.content = PlacedImage{id, 0, 0, 10, 10};
        b.content = PlacedImage{id, 0, 0, 10, 10};
        ItemId ia = doc.add_item(0, a), ib = doc.add_item(0, b);
        doc.remove_item(ia);
        CHECK(doc.find_image(id), "one placement keeps the asset");
        doc.remove_item(ib);
        CHECK(doc.find_image(id), "an unused picture stays in the publication");
        CHECK(doc.remove_image(id) && !doc.find_image(id),
              "remove_image takes an unused picture out");
        ImageAsset spare;
        spare.name = "spare.png";
        uint32_t spare_id = doc.add_image(spare);
        ImageAsset used;
        used.name = "used.png";
        uint32_t used_id = doc.add_image(used);
        Item kept_item;
        kept_item.w = kept_item.h = 10;
        kept_item.content = PlacedImage{used_id, 0, 0, 10, 10};
        doc.add_item(0, kept_item);
        CHECK(doc.remove_unused_images() == 1 && !doc.find_image(spare_id) &&
              doc.find_image(used_id), "remove unused leaves a placed picture");
        CHECK(source_is(store.find(id), kept),
              "the bytes stay in the store for undo");
        const uint32_t next = store.take_id();
        CHECK(next == id + 1, "ids keep moving forward (%u)", next);
    }

    const std::string linked_png = "/tmp/pagemade-pic.png";
    const std::string emb_path = "/tmp/pagemade-img-emb.pagemade";
    const std::string link_path = "/tmp/pagemade-img-link.pagemade";
    {
        FILE *f = std::fopen(linked_png.c_str(), "wb");
        CHECK(f != nullptr, "write the linked png");
        if (f) {
            std::fwrite(png.data(), 1, png.size(), f);
            std::fclose(f);
        }
        ImageStore store;
        PageDoc doc;
        const uint32_t id = store.take_id();
        CHECK(store.load_file(id, linked_png, &meta, &err), "load linked png: %s", err.c_str());
        ImageAsset asset;
        asset.id = id;
        asset.name = "pic.png";
        asset.source = file_uri(linked_png);
        asset.meta = store.find(id)->meta;
        asset.source_size = png.size();
        asset.hash = image_bytes_hash(png.data(), png.size());
        struct stat st;
        if (stat(linked_png.c_str(), &st) == 0)
            asset.source_mtime = (int64_t) st.st_mtime;
        CHECK(asset_status(asset, &store) == AssetStatus::Ok, "a fresh import is ok");
        ImageAsset drifted = asset;
        drifted.hash = 1;
        CHECK(asset_status(drifted, &store) == AssetStatus::Modified,
              "a different hash of the loaded bytes is modified");
        ImageAsset unstamped = asset;
        unstamped.source_size = 0;
        unstamped.source_mtime = 0;
        unstamped.hash = 0;
        CHECK(asset_status(unstamped, &store) == AssetStatus::Ok,
              "a missing stamp is not itself a change");
        ImageAsset ghost = asset;
        ghost.source = file_uri("/tmp/pagemade-no-such-picture.png");
        CHECK(asset_status(ghost, nullptr) == AssetStatus::Missing, "a missing file is missing");
        /* The overload a background thread uses: loaded bytes passed in */
        ImageStore::Bytes held_png = std::make_shared<const std::vector<uint8_t>>(png);
        CHECK(asset_status(asset, held_png) == AssetStatus::Ok &&
              asset_status(drifted, held_png) == AssetStatus::Modified,
              "status from bytes handed to another thread");

        ImageImport probed = probe_image_file(linked_png);
        CHECK(probed.ok && probed.bytes.empty() && probed.stamp.size == png.size() &&
              probed.meta.width_px == asset.meta.width_px &&
              probed.meta.height_px == asset.meta.height_px,
              "probe_image_file reads the header, keeps no bytes");
        CHECK(!probe_image_file("/tmp/pagemade-no-such-picture.png").ok,
              "probing a missing file fails");

        /* A link loads on demand and unloads without dropping every texture */
        ImageStore lazy;
        lazy.add_link(7, linked_png, asset.meta);
        CHECK(!lazy.is_loaded(7) && lazy.path_of(7) == linked_png, "a link starts unloaded");
        std::string load_err;
        CHECK(lazy.load(7, &load_err) && lazy.is_loaded(7), "load reads a link");
        const uint64_t gen = lazy.generation();
        CHECK(lazy.unload(7) && !lazy.is_loaded(7) && lazy.path_of(7) == linked_png,
              "unload drops the bytes and keeps the path");
        CHECK(lazy.generation() == gen, "unload leaves the generation alone");
        CHECK(lazy.unloadable_bytes() == 0, "nothing left to unload");
        doc.add_image(asset);
        Item it;
        it.w = 30; it.h = 16;
        it.xf = Transform::translate(12, 18);
        it.content = PlacedImage{id, -4, 2, 40, 20};
        doc.add_item(0, it);
        CHECK(save_document(doc, fonts, emb_path, SaveOptions{true}, &err, &store),
              "embed save: %s", err.c_str());
        OpenResult emb = open_document(emb_path, fonts);
        CHECK(emb.ok, "open embedded: %s", emb.error.c_str());
        if (emb.ok) {
            check_doc(doc, emb.doc);
            const ImageStore::Entry *e = emb.images.find(id);
            CHECK(source_is(e, png), "embedded bytes match the file that was placed");
            CHECK(emb.doc.find_image(id) && emb.doc.find_image(id)->meta.format == "png" &&
                  std::fabs(emb.doc.find_image(id)->meta.ppi_x - 300) < 0.1,
                  "opened metadata comes from the file");
        }
        const std::string json = document_json(doc, fonts, SaveOptions{false}, &store);
        CHECK(json.find("\"uri\": \"file:///") != std::string::npos, "a linked picture names its file");
        CHECK(save_document(doc, fonts, link_path, SaveOptions{false}, &err, &store),
              "link save: %s", err.c_str());
        OpenResult link = open_document(link_path, fonts);
        CHECK(link.ok, "open linked: %s", link.error.c_str());
        if (link.ok) {
            const ImageStore::Entry *e = link.images.find(id);
            CHECK(source_is(e, png), "opening a link reads the file again");
        }
        std::remove(linked_png.c_str());
        OpenResult gone = open_document(link_path, fonts);
        CHECK(gone.ok, "a missing linked file still opens");
        bool warned = false;
        for (const std::string &w : gone.warnings)
            warned |= w.find("couldn't read the picture") != std::string::npos;
        CHECK(warned, "a missing linked file is reported");
        CHECK(gone.images.find(id) == nullptr, "no bytes when the file is gone");
    }

    const std::string pdf_path = "/tmp/pagemade-img.pdf";
    {
        std::vector<uint8_t> photo = make_jpeg_rgb(8, 4, 200, 10, 10);
        ImageStore store;
        PageDoc doc;
        const uint32_t id = store.take_id();
        CHECK(store.add(id, photo, &meta, &err), "real jpeg: %s", err.c_str());
        CHECK(meta.stored_width_px == 8 && meta.stored_height_px == 4, "stb jpeg size");
        ImageAsset asset;
        asset.id = id; asset.name = "photo.jpg"; asset.meta = meta;
        doc.add_image(std::move(asset));
        Item it;
        it.w = 72; it.h = 36;
        it.content = PlacedImage{id, 0, 0, 72, 36};
        doc.add_item(0, it);
        std::vector<Composition> comps;
        CHECK(export_pdf(pdf_path, doc, comps, &store), "export a jpeg");
        std::string plain = pdf_plain(read_bin(pdf_path));
        CHECK(plain.find("/Subtype /Image") != std::string::npos, "an image XObject");
        CHECK(plain.find("/DCTDecode") != std::string::npos, "the JPEG stream is passed through");
        CHECK(pdf_has_number(plain, "/Width ", 8), "width is the source's 8 pixels");
        CHECK(plain.find("re W n") != std::string::npos, "the frame clips with W n");
        CHECK(plain.find("72 0 0 -36 0 36 cm") != std::string::npos,
              "an upright JPEG uses the orientation-1 matrix");

        doc.pages[0].hidden = true;
        CHECK(export_pdf(pdf_path, doc, comps, &store), "export with the picture page hidden");
        plain = pdf_plain(read_bin(pdf_path));
        CHECK(plain.find("/DCTDecode") == std::string::npos, "a hidden page is left out of the PDF");

        std::vector<uint8_t> orient = make_jpeg_header(20, 8, 72);
        const uint32_t id6 = store.take_id();
        CHECK(store.add(id6, orient, &meta, &err) && meta.orientation == 6, "orient-6 jpeg");
        PageDoc turned;
        ImageAsset a6;
        a6.id = id6; a6.name = "turn.jpg"; a6.meta = meta;
        turned.add_image(std::move(a6));
        Item frame;
        frame.w = 20; frame.h = 10;
        frame.content = PlacedImage{id6, 0, 0, 20, 10};
        turned.add_item(0, frame);
        CHECK(export_pdf(pdf_path, turned, comps, &store), "export an oriented jpeg");
        plain = pdf_plain(read_bin(pdf_path));
        CHECK(pdf_has_number(plain, "/Width ", 20), "oriented JPEG keeps the stored width");
        CHECK(!pdf_has_number(plain, "/Width ", 8), "the oriented height is not the XObject width");
        CHECK(plain.find("0 10 20 0 0 0 cm") != std::string::npos, "orientation 6 matrix");

        std::vector<uint8_t> big = make_png(32, 16, nullptr, 0, 0);
        ImageStore pngs;
        const uint32_t pid = pngs.take_id();
        CHECK(pngs.add(pid, big, &meta, &err), "png for pdf");
        PageDoc sheet;
        ImageAsset pa;
        pa.id = pid; pa.name = "sheet.png"; pa.meta = meta;
        sheet.add_image(std::move(pa));
        Item box;
        box.w = 32; box.h = 16;
        box.content = PlacedImage{pid, 0, 0, 32, 16};
        sheet.add_item(0, box);
        CHECK(export_pdf(pdf_path, sheet, comps, &pngs), "export a png");
        plain = pdf_plain(read_bin(pdf_path));
        CHECK(plain.find("/Subtype /Image") != std::string::npos &&
              plain.find("/FlateDecode") != std::string::npos, "PNG pixels are Flate");
        CHECK(pdf_has_number(plain, "/Width ", 32), "PNG width is the source pixel count");
        CHECK(plain.find("/DCTDecode") == std::string::npos, "a PNG is not passed through as JPEG");

        PageDoc bare;
        Item missing;
        missing.w = 10; missing.h = 10;
        missing.content = PlacedImage{99, 0, 0, 10, 10};
        bare.add_item(0, missing);
        CHECK(export_pdf(pdf_path, bare, comps, &pngs), "a missing picture still exports");
        plain = pdf_plain(read_bin(pdf_path));
        CHECK(plain.find("0.9 0.9 0.9 rg") != std::string::npos, "a missing picture is a stand-in");
    }

    {
        const int W = 16, H = 8;
        std::vector<uint8_t> src((size_t) W * H * 4, 0);
        auto paint = [&](int x, int y, uint8_t r) {
            src[((size_t) y * W + x) * 4] = r;
        };
        paint(0, 0, 10);
        paint(W - 1, 0, 20);
        paint(0, H - 1, 30);
        paint(W - 1, H - 1, 40);
        for (int orient = 1; orient <= 8; ++orient) {
            int w = W, h = H;
            unsigned char *turned = nvg__exifApply(src.data(), &w, &h, 4, orient);
            const uint8_t *pix = turned ? turned : src.data();
            const int dw = turned ? w : W, dh = turned ? h : H;
            float m[6];
            image_pdf_matrix(orient, 0, 0, (float) dw, (float) dh, m);
            const int corners[4][3] = {{0, 0, 10}, {W - 1, 0, 20}, {0, H - 1, 30}, {W - 1, H - 1, 40}};
            for (const auto &c : corners) {
                int px = -1, py = -1;
                for (int y = 0; y < dh && px < 0; ++y)
                    for (int x = 0; x < dw; ++x)
                        if (pix[((size_t) y * dw + x) * 4] == c[2]) { px = x; py = y; break; }
                const float u = ((float) c[0] + 0.5f) / W;
                const float v = 1.f - ((float) c[1] + 0.5f) / H;
                const float x = m[0] * u + m[2] * v + m[4];
                const float y = m[1] * u + m[3] * v + m[5];
                const bool same = px >= 0 && (px >= dw / 2) == (x >= dw * 0.5f) &&
                                  (py >= dh / 2) == (y >= dh * 0.5f);
                CHECK(same, "orient %d corner %d pixel (%d,%d) matrix (%.2f,%.2f)",
                      orient, c[2], px, py, x, y);
            }
            std::free(turned);
        }
        float m[6];
        image_pdf_matrix(1, 10, 20, 30, 40, m);
        CHECK(near(m[0], 30) && near(m[1], 0) && near(m[2], 0) && near(m[3], -40) &&
              near(m[4], 10) && near(m[5], 60), "orientation 1 is {w 0 0 -h x y+h}");
    }

    std::remove(emb_path.c_str());
    std::remove(link_path.c_str());
    std::remove(pdf_path.c_str());
    std::remove(linked_png.c_str());
}

/* Picture opacity and borders, and the compound line styles */
static void test_picture_style(const FontLibrary &fonts) {
    /* Bands fill the weight from the outside in; thick-thin is thick outside */
    float span = 0;
    for (const StrokeBand &b : stroke_bands(LineStyle::Double, 4.f))
        span += b.width;
    const std::vector<StrokeBand> dbl = stroke_bands(LineStyle::Double, 4.f);
    CHECK(dbl.size() == 2 && near(dbl[0].offset, -dbl[1].offset) && near(dbl[0].width, 1.2f) &&
          near(dbl[0].offset + dbl[0].width / 2, 2.f), "double: two bands to the edges");
    CHECK(span < 4.f, "a double line leaves a gap (%g)", span);
    const std::vector<StrokeBand> tt = stroke_bands(LineStyle::ThickThin, 4.f);
    CHECK(tt.size() == 2 && tt[0].offset > tt[1].offset && tt[0].width > tt[1].width,
          "thick-thin: the thick band is outside");
    const std::vector<StrokeBand> ttt = stroke_bands(LineStyle::ThinThickThin, 6.f);
    CHECK(ttt.size() == 3 && near(ttt[1].offset, 0.f) && ttt[1].width > ttt[0].width,
          "thin-thick-thin: the thick band is centered");
    CHECK(stroke_bands(LineStyle::Dashed, 4.f).empty() &&
          dash_pattern(LineStyle::Double, 4.f).empty(), "compound and dashed styles don't mix");

    /* Offsetting moves a loop outward whichever way it winds */
    for (bool reversed : {false, true}) {
        Polyline sq;
        sq.closed = true;
        sq.pts = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        if (reversed)
            std::reverse(sq.pts.begin(), sq.pts.end());
        Bounds b;
        for (const Polyline &o : offset_polylines({sq}, 1.f))
            for (const Point &p : o.pts)
                b.add(p);
        CHECK(near(b.x0, -1.f) && near(b.y0, -1.f) && near(b.x1, 11.f) && near(b.y1, 11.f),
              "offset square (%s winding) %g %g %g %g", reversed ? "reversed" : "first",
              b.x0, b.y0, b.x1, b.y1);
    }
    Polyline line;
    line.pts = {{0, 0}, {10, 0}};
    const std::vector<Polyline> left = offset_polylines({line}, 2.f);
    CHECK(left.size() == 1 && near(left[0].pts[0].y, -2.f) && near(left[0].pts[1].y, -2.f),
          "an open line moves left of travel");

    /* Drawing: the picture at its opacity, then its border's bands */
    PageDoc doc;
    Item pic;
    pic.w = 100;
    pic.h = 50;
    PlacedImage im{7, 0, 0, 100, 50};
    im.opacity = 40.f;
    im.stroke = {{kBlack, 100.f}, 4.f, LineStyle::ThickThin};
    pic.content = im;
    const ItemId pid = doc.add_item(0, pic);
    Item box;
    box.w = box.h = 40;
    box.xf = Transform::translate(200, 0);
    Shape sh;
    sh.stroke = {{kBlack, 100.f}, 3.f, LineStyle::Double};
    box.content = sh;
    doc.add_item(0, box);
    const DrawList list = build_page(doc, 0, {});
    CHECK(list.size() == 5, "picture, two border bands, two shape bands (%zu)", list.size());
    if (list.size() == 5) {
        const DrawImage *di = std::get_if<DrawImage>(&list[0]);
        CHECK(di && near(di->opacity, 0.4f), "the picture is drawn at 40%%");
        const DrawStroke *outer = std::get_if<DrawStroke>(&list[1]);
        const DrawStroke *inner = std::get_if<DrawStroke>(&list[2]);
        CHECK(outer && inner && near(outer->width, 2.2f) && near(inner->width, 0.8f) &&
              outer->dash.empty(), "the border is thick, then thin");
        if (outer) {
            const Bounds ob = path_bounds(outer->path);
            /* 4 pt thick-thin: the 2.2 pt band's center is 0.9 pt out, so
             * its outer edge is the stroke's, 2 pt outside the frame */
            CHECK(near(ob.x0, -0.9f) && near(ob.x1, 100.9f),
                  "the thick band sits outside the frame edge (%g %g)", ob.x0, ob.x1);
        }
        CHECK(std::get_if<DrawStroke>(&list[3]) && std::get_if<DrawStroke>(&list[4]),
              "a double-lined shape is two strokes");
    }

    /* The file keeps opacity, borders and the new styles */
    const std::string path = "/tmp/pagemade-picture-style.pagemade";
    std::string error;
    CHECK(save_document(doc, fonts, path, {}, &error), "save styled pictures: %s", error.c_str());
    OpenResult back = open_document(path, fonts);
    CHECK(back.ok, "open styled pictures: %s", back.error.c_str());
    if (back.ok) {
        const Item *p = back.doc.find_item(pid);
        CHECK(p && p->image() && near(p->image()->opacity, 40.f) &&
              p->image()->stroke.style == LineStyle::ThickThin &&
              near(p->image()->stroke.weight, 4.f), "picture opacity and border survive");
        bool dbl_kept = false;
        for (const Item &it : back.doc.pages[0].items)
            if (it.shape())
                dbl_kept |= it.shape()->stroke.style == LineStyle::Double;
        CHECK(dbl_kept, "a double line survives");
    }
    std::remove(path.c_str());

    /* PDF: the picture's alpha is an ExtGState set before it is painted */
    ImageMetadata meta;
    std::string err;
    std::vector<uint8_t> photo = make_jpeg_rgb(8, 4, 200, 10, 10);
    ImageStore store;
    PageDoc pd;
    const uint32_t id = store.take_id();
    CHECK(store.add(id, photo, &meta, &err), "jpeg for opacity: %s", err.c_str());
    ImageAsset asset;
    asset.id = id;
    asset.name = "photo.jpg";
    asset.meta = meta;
    pd.add_image(std::move(asset));
    Item it;
    it.w = 72;
    it.h = 36;
    PlacedImage half{id, 0, 0, 72, 36};
    half.opacity = 50.f;
    half.stroke = {{kBlack, 100.f}, 2.f, LineStyle::Double};
    it.content = half;
    pd.add_item(0, it);
    const std::string pdf_path = "/tmp/pagemade-picture-style.pdf";
    std::vector<Composition> comps;
    CHECK(export_pdf(pdf_path, pd, comps, &store), "export a translucent picture");
    const std::string plain = pdf_plain(read_bin(pdf_path));
    const size_t gs = plain.find("/A500 gs");
    const size_t paint = plain.find("/Im" + std::to_string(id) + " Do");
    CHECK(gs != std::string::npos && paint != std::string::npos && gs < paint,
          "the picture is painted at 50%% alpha");
    CHECK(plain.find("/ca 0.5") != std::string::npos, "with a 0.5 ExtGState");
    size_t strokes = 0;
    for (size_t at = plain.find("\nS\n"); at != std::string::npos; at = plain.find("\nS\n", at + 1))
        ++strokes;
    CHECK(strokes == 2, "the double border is two stroked paths (%zu)", strokes);
    std::remove(pdf_path.c_str());
}

static std::vector<const PlacedGlyph *> visible_glyphs(const Composition &c) {
    std::vector<const PlacedGlyph *> g;
    for (const ComposedLine &l : c.lines)
        for (const GlyphRun &r : l.runs)
            for (const PlacedGlyph &p : r.glyphs)
                if (!(p.flags & PlacedGlyph::Invisible))
                    g.push_back(&p);
    return g;
}

static void test_caps(const FontLibrary &fonts) {
    CharStyle cs;
    cs.size = 36;
    ParaStyle ps;
    std::vector<Frame> wide = {{0, 0, 2000, 200}};
    Composition plain = compose(one_para("Ab", cs, ps), wide, fonts);
    cs.caps = Caps::All;
    Composition all = compose(one_para("Ab", cs, ps), wide, fonts);
    CharStyle up = cs;
    up.caps = Caps::Normal;
    Composition upper = compose(one_para("AB", up, ps), wide, fonts);
    const auto gp = visible_glyphs(plain);
    const auto ga = visible_glyphs(all);
    const auto gu = visible_glyphs(upper);
    CHECK(gp.size() >= 2 && ga.size() >= 2 && gu.size() >= 2,
          "caps glyphs %zu %zu %zu", gp.size(), ga.size(), gu.size());
    if (gp.size() >= 2 && ga.size() >= 2 && gu.size() >= 2)
        CHECK(ga[1]->gid == gu[1]->gid && ga[1]->gid != gp[1]->gid,
              "all caps shapes the capital (%u vs plain %u vs AB %u)",
              ga[1]->gid, gp[1]->gid, gu[1]->gid);

    cs.caps = Caps::Small;
    Composition small = compose(one_para("a", cs, ps), wide, fonts);
    Composition low = compose(one_para("a", up, ps), wide, fonts);
    Composition cap = compose(one_para("A", up, ps), wide, fonts);
    const auto gs = visible_glyphs(small);
    const auto gl = visible_glyphs(low);
    const auto gc = visible_glyphs(cap);
    CHECK(!gs.empty() && !gl.empty() && !gc.empty(), "small caps glyphs");
    if (!gs.empty() && !gl.empty() && !gc.empty()) {
        CHECK(gs[0]->gid != gl[0]->gid, "small caps is not the lowercase glyph");
        CHECK(gs[0]->adv < gc[0]->adv - 0.2f,
              "small caps is narrower than the capital (%.2f vs %.2f)",
              gs[0]->adv, gc[0]->adv);
    }

    cs.caps = Caps::Normal;
    cs.underline = true;
    cs.strike = true;
    Composition ruled = compose(one_para("Hi", cs, ps), wide, fonts);
    bool flags = false;
    for (const ComposedLine &l : ruled.lines)
        for (const GlyphRun &r : l.runs)
            flags = flags || (r.underline && r.strike && !r.glyphs.empty());
    CHECK(flags, "underline and strikethrough land on the glyph run");
}

static void test_type_file(const FontLibrary &fonts) {
    PageDoc doc = sample_document();
    CHECK(!doc.stories.empty() && doc.stories[0].story.paragraphs.size() >= 2 &&
          !doc.stories[0].story.paragraphs[0].runs.empty() &&
          !doc.stories[0].story.paragraphs[1].runs.empty(),
          "sample headline has two runs");
    if (doc.stories.empty() || doc.stories[0].story.paragraphs.size() < 2)
        return;
    Run &light = doc.stories[0].story.paragraphs[0].runs[0];
    light.style.face = "Light";
    light.style.bold = false;
    light.style.italic = false;
    light.style.caps = Caps::Small;
    light.style.underline = true;
    light.style.strike = true;
    Run &classic = doc.stories[0].story.paragraphs[1].runs[0];
    classic.style.face = FontLibrary::style_name(classic.style.bold, classic.style.italic);

    const std::string json = document_json(doc, fonts, SaveOptions{});
    CHECK(json.find("\"face\": \"Light\"") != std::string::npos, "face is written");
    CHECK(json.find("\"caps\": \"small\"") != std::string::npos, "small caps is written");
    CHECK(json.find("\"underline\": true") != std::string::npos, "underline is written");
    CHECK(json.find("\"strike\": true") != std::string::npos, "strikethrough is written");
    size_t faces = 0;
    for (size_t at = 0; (at = json.find("\"face\"", at)) != std::string::npos; at += 6)
        ++faces;
    CHECK(faces == 1, "a face equal to Regular/Bold/Italic is omitted (%zu)", faces);

    const std::string path = "/tmp/pagemade-type-specs.pagemade";
    std::string error;
    CHECK(save_document(doc, fonts, path, SaveOptions{}, &error),
          "save type specs (%s)", error.c_str());
    OpenResult opened = open_document(path, fonts);
    CHECK(opened.ok, "open type specs (%s)", opened.error.c_str());
    bool found = false, classic_clear = false;
    if (opened.ok && !opened.doc.stories.empty() &&
        opened.doc.stories[0].story.paragraphs.size() >= 2 &&
        !opened.doc.stories[0].story.paragraphs[0].runs.empty() &&
        !opened.doc.stories[0].story.paragraphs[1].runs.empty()) {
        const CharStyle &a = opened.doc.stories[0].story.paragraphs[0].runs[0].style;
        found = a.face == "Light" && a.caps == Caps::Small && a.underline && a.strike && !a.bold;
        const CharStyle &b = opened.doc.stories[0].story.paragraphs[1].runs[0].style;
        classic_clear = b.face.empty() && b.italic;
    }
    CHECK(found, "opened face, caps, underline and strike");
    CHECK(classic_clear, "opened classic face stays empty");
    std::remove(path.c_str());
}

static std::string join_rows(const std::vector<FontMenuRow> &rows) {
    std::string text;
    for (const FontMenuRow &row : rows) {
        if (!text.empty())
            text += " | ";
        if (row.heading)
            text += "[";
        text += row.text;
        if (row.starred)
            text += "*";
        if (row.heading)
            text += "]";
    }
    return text;
}

static void test_font_menu() {
    const std::vector<std::string> installed = {
        "Serif", "Sans", "Display", "Liberation Sans", "Noto Sans Arabic",
        "Noto Sans Display", "Noto Sans", "DejaVu Sans"};
    const FontMenuList defaults = font_menu_defaults(installed);
    const std::vector<std::string> english = {
        "Serif", "Sans", "Display", "Liberation Sans", "DejaVu Sans", "Noto Sans"};
    std::string got_defaults;
    for (const std::string &name : defaults.favorites) {
        if (!got_defaults.empty())
            got_defaults += ", ";
        got_defaults += name;
    }
    CHECK(defaults.favorites == english, "english defaults %s", got_defaults.c_str());
    CHECK(defaults.recent_limit == 8, "recent limit starts at 8");

    FontMenuModel model;
    model.set_list(defaults);
    model.list();
    FontMenuList listed = defaults;
    listed.recents = {"DejaVu Sans"};
    model.set_list(listed);
    const std::vector<FontMenuRow> short_list =
        model.rows(installed, {"Noto Sans Arabic"}, "Noto Sans Arabic", "");
    CHECK(join_rows(short_list) ==
              "[In this publication] | Noto Sans Arabic | [Recent] | DejaVu Sans* | "
              "[Favorites] | Serif* | Sans* | Display* | Liberation Sans* | Noto Sans*",
          "short list %s", join_rows(short_list).c_str());

    const std::vector<FontMenuRow> found = model.rows(installed, {"Noto Sans Arabic"}, "", "noto");
    CHECK(join_rows(found) ==
              "[Search: noto] | Noto Sans Arabic | Noto Sans Display | Noto Sans*",
          "search %s", join_rows(found).c_str());
    bool section = false;
    for (const FontMenuRow &row : found)
        section |= row.text == "Recent" || row.text == "Favorites" || row.text == "In this publication";
    CHECK(!section, "a search has no section headings");

    const std::vector<FontMenuRow> none = model.rows(installed, {}, "", "zzz");
    CHECK(none.size() == 2 && none[0].heading && none[0].text == "Search: zzz" &&
              none[1].heading && none[1].text == "No matching fonts",
          "no matches %s", join_rows(none).c_str());

    FontMenuModel odd;
    FontMenuList odd_list;
    odd_list.favorites = {"Serif"};
    odd.set_list(odd_list);
    const std::vector<FontMenuRow> current_only =
        odd.rows({"Serif", "Weird Face"}, {}, "Weird Face", "");
    CHECK(join_rows(current_only) == "Weird Face | [Favorites] | Serif*",
          "current family leads %s", join_rows(current_only).c_str());
    FontMenuList missing = odd_list;
    missing.favorites = {"Missing", "Serif"};
    missing.recents = {"Missing"};
    odd.set_list(missing);
    const std::vector<FontMenuRow> dropped = odd.rows({"Serif"}, {}, "", "");
    CHECK(join_rows(dropped) == "[Favorites] | Serif*",
          "uninstalled names stay out of the menu %s", join_rows(dropped).c_str());

    FontMenuList capped;
    capped.recent_limit = 2;
    model.set_list(capped);
    model.note_use("A");
    model.note_use("B");
    model.note_use("C");
    CHECK(model.list().recents.size() == 2 && model.list().recents[0] == "C" &&
              model.list().recents[1] == "B",
          "recents trim");
    model.note_use("A");
    CHECK(model.list().recents.size() == 2 && model.list().recents[0] == "A" &&
              model.list().recents[1] == "C",
          "a repeated recent moves to the front");
    model.toggle_star("Sans");
    model.toggle_star("Serif");
    model.toggle_star("Sans");
    CHECK(model.list().favorites.size() == 1 && model.list().favorites[0] == "Serif",
          "unstarring removes and starring appends");
    FontMenuList limited;
    limited.recent_limit = 0;
    model.set_list(limited);
    model.note_use("A");
    CHECK(model.list().recents.empty(), "a zero recent limit keeps nothing");

    PageDoc doc;
    StoryEntry first;
    Paragraph paragraph;
    Run run;
    run.style.family = "Noto Sans Arabic";
    run.text = "a";
    paragraph.runs.push_back(run);
    run.style.family = "Serif";
    paragraph.runs.push_back(run);
    run.style.family.clear();
    paragraph.runs.push_back(run);
    run.style.family = "Noto Sans Arabic";
    paragraph.runs.push_back(run);
    first.story.paragraphs.push_back(paragraph);
    doc.stories.push_back(first);
    StoryEntry second;
    Paragraph later;
    run.style.family = "Display";
    run.text = "b";
    later.runs.push_back(run);
    second.story.paragraphs.push_back(later);
    doc.stories.push_back(second);
    const std::vector<std::string> used = document_font_families(doc);
    CHECK(used.size() == 3 && used[0] == "Noto Sans Arabic" && used[1] == "Serif" &&
              used[2] == "Display",
          "document families follow the stories");

    std::string query = "No\xC3\xA9";
    CHECK(font_menu_query_edit(query, 0) && query == "No", "backspace drops one codepoint");
    CHECK(font_menu_query_edit(query, ' ') && query == "No ", "a space joins the query");
    CHECK(font_menu_query_edit(query, 0x1B) && query.empty(), "escape clears the query");
    CHECK(!font_menu_query_edit(query, 0x1B), "escape on an empty query closes the menu");

    const std::string dir = "/tmp/pagemade-font-menu-test";
    nanogui::fs::remove_all(dir);
    nanogui::fs::create_directories(dir);
    const std::string path = dir + "/fonts.conf";
    FontMenuModel fresh;
    CHECK(!fresh.load(path, installed), "a missing config is not a saved list");
    CHECK(!nanogui::fs::exists(path), "load does not create the file");
    font_menu_load_or_seed(fresh, path, installed);
    CHECK(nanogui::fs::exists(path), "the first run writes the english list");
    FontMenuModel seeded;
    CHECK(seeded.load(path, installed), "the seeded file loads");
    CHECK(seeded.list().favorites == english, "seeded favorites match the english list");

    {
        std::ofstream out(path, std::ios::trunc);
        out << "# comment\n\n";
        out << "recent_limit 2\n";
        out << "favorite Serif\n";
        out << "favorite Serif\n";
        out << "favorite Noto Sans Arabic\n";
        out << "recent DejaVu Sans\n";
        out << "recent Liberation Sans\n";
        out << "recent DejaVu Sans\n";
        out << "unknown junk\n";
        out << "favorite\n";
        out << "favorite  Liberation Sans\n";
    }
    FontMenuModel round;
    CHECK(round.load(path, {"Serif"}), "a hand-written config loads");
    CHECK(round.list().recent_limit == 2, "recent limit is stored");
    CHECK(round.list().favorites.size() == 3 && round.list().favorites[0] == "Serif" &&
              round.list().favorites[1] == "Noto Sans Arabic" &&
              round.list().favorites[2] == "Liberation Sans",
          "favorites keep spaces, the first duplicate, and fonts that are not installed");
    CHECK(round.list().recents.size() == 2 && round.list().recents[0] == "DejaVu Sans" &&
              round.list().recents[1] == "Liberation Sans",
          "recents stay newest-first and drop duplicates");
    {
        std::ofstream out(path, std::ios::trunc);
        out << "# none\n";
    }
    FontMenuModel empty;
    CHECK(empty.load(path, installed) && empty.list().favorites.empty(),
          "an empty favorites file is a choice, not a missing file");
    font_menu_load_or_seed(empty, path, installed);
    CHECK(empty.list().favorites.empty(), "seeding does not refill a file the user emptied");
    nanogui::fs::remove_all(dir);

    const std::string config = font_menu_config_path();
    CHECK(!config.empty() && (config.find("pagemade/fonts.conf") != std::string::npos ||
                              config == "pagemade-fonts.conf"),
          "config path %s", config.c_str());
}

static bool near_cmyk(const Cmyk &c, float cyan, float mag, float yel, float blk) {
    return std::fabs(c.c - cyan) < 0.5f && std::fabs(c.m - mag) < 0.5f &&
           std::fabs(c.y - yel) < 0.5f && std::fabs(c.k - blk) < 0.5f;
}

static void test_swatch_library() {
    /* CMYK is the truth; RGB follows naively. */
    CHECK(near_color(cmyk_to_rgb({100, 0, 0, 0}), 0, 1, 1, 1), "cyan");
    CHECK(near_color(cmyk_to_rgb({0, 0, 0, 100}), 0, 0, 0, 1), "black is all key");
    CHECK(near_color(cmyk_to_rgb({0, 0, 0, 0}), 1, 1, 1, 1), "paper is white");
    CHECK(near_cmyk(rgb_to_cmyk({1, 0, 0, 1}), 0, 100, 100, 0), "red inverts");
    CHECK(near_cmyk(rgb_to_cmyk({0, 0, 0, 1}), 0, 0, 0, 100), "black is key only");
    CHECK(near_color(cmyk_to_rgb(rgb_to_cmyk({0.8f, 0.6f, 0.2f, 1})), 0.8f, 0.6f, 0.2f, 1),
          "screen colors round-trip (cmyk with key does not: one rgb, many cmyks)");
    CHECK(cmyk_text({100, 0, 0, 0}) == "C=100 M=0 Y=0 K=0", "the menu tag");

    /* The defaults carry both definitions now. */
    PageDoc doc;
    const Swatch *red = nullptr;
    for (const Swatch &s : doc.swatches)
        if (s.name == "Red")
            red = &s;
    CHECK(red && near_cmyk(red->cmyk, 0, 100, 100, 0), "Red knows its CMYK");
    const Swatch *reg = doc.find_swatch(kRegistration);
    CHECK(reg && near_cmyk(reg->cmyk, 100, 100, 100, 100), "registration is all plates");
    const SwatchId ochre = doc.add_swatch("Ochre", {0.8f, 0.6f, 0.2f, 1});
    CHECK(near_cmyk(doc.find_swatch(ochre)->cmyk, 0, 25, 75, 20), "added colors derive CMYK");

    /* The file round-trips, seeding, and the editing rules. */
    const std::string dir = "/tmp/pagemade-swatch-test";
    nanogui::fs::remove_all(dir);
    nanogui::fs::create_directories(dir);
    const std::string path = dir + "/colors.conf";
    SwatchLibrary fresh;
    CHECK(!swatch_library_load(fresh, path), "a missing config is not a palette");
    swatch_library_load_or_seed(fresh, path);
    CHECK(nanogui::fs::exists(path) && !fresh.custom.empty(), "the first run seeds a starter set");
    SwatchLibrary seeded;
    CHECK(swatch_library_load(seeded, path), "the seeded file loads");
    CHECK(seeded.custom.size() == fresh.custom.size(), "the seed round-trips");
    CHECK(swatch_library_find(seeded, "Cyan") != nullptr, "Cyan is in the starter set");

    {
        std::ofstream out(path, std::ios::trunc);
        out << "# comment\n\n";
        out << "color \"Deep Teal\" 100 20 40 10\n";
        out << "color Deep Teal 0 0 0 0\n";
        out << "color \"Bad\" 100 0\n";
        out << "color \"Out\" 200 -10 0 0\n";
        out << "unknown junk\n";
    }
    SwatchLibrary round;
    CHECK(swatch_library_load(round, path), "a hand-written config loads");
    CHECK(round.custom.size() == 2, "duplicates and short lines drop out (%zu)",
          round.custom.size());
    CHECK(round.custom[0].name == "Deep Teal" &&
              near_cmyk(round.custom[0].cmyk, 100, 20, 40, 10),
          "quoted names keep spaces");
    CHECK(near_cmyk(round.custom[1].cmyk, 100, 0, 0, 0), "values clamp to 0..100");

    SwatchId teal = 0;
    CHECK(swatch_library_add(round, path, doc.swatches, "Teal", {100, 0, 40, 0}, &teal) &&
              teal > kRegistration,
          "added colors get fresh ids (%u)", teal);
    CHECK(!swatch_library_add(round, path, doc.swatches, "Teal", {0, 0, 0, 0}, nullptr),
          "duplicate names are refused");
    CHECK(!swatch_library_add(round, path, doc.swatches, "", {0, 0, 0, 0}, nullptr),
          "empty names are refused");
    CHECK(swatch_library_set(round, path, teal, "Teal Dark", {100, 0, 40, 20}),
          "rename and recolor");
    CHECK(!swatch_library_set(round, path, kBlack, "Black", {0, 0, 0, 100}),
          "built-ins stay locked");
    CHECK(!swatch_library_set(round, path, teal, "Deep Teal", {0, 0, 0, 0}),
          "renames cannot collide");
    CHECK(swatch_library_remove(round, path, teal), "custom colors delete");
    CHECK(!swatch_library_remove(round, path, kPaper), "built-ins do not delete");
    CHECK(!swatch_library_remove(round, path, 999999), "missing ids do nothing");
    SwatchLibrary reloaded;
    CHECK(swatch_library_load(reloaded, path) &&
              swatch_library_find(reloaded, "Teal Dark") == nullptr &&
              swatch_library_find(reloaded, "Deep Teal") != nullptr,
          "edits persist");
    CHECK(swatch_library_upsert(reloaded, path, doc.swatches, "Deep Teal", {50, 50, 50, 50}) &&
              near_cmyk(swatch_library_find(reloaded, "Deep Teal")->cmyk, 50, 50, 50, 50),
          "the globe recolors by name");
    CHECK(swatch_library_upsert(reloaded, path, doc.swatches, "Fresh", {1, 2, 3, 4}) &&
              swatch_library_find(reloaded, "Fresh") != nullptr,
          "the globe adds by name");

    /* Seeding never moves a document's own colors. */
    PageDoc blank;
    swatch_library_seed(blank, reloaded);
    const Swatch *accent_red = nullptr;
    for (const Swatch &s : blank.swatches)
        if (s.id == 8)
            accent_red = &s;
    CHECK(accent_red && accent_red->name == "Red" && near_cmyk(accent_red->cmyk, 0, 100, 100, 0),
          "id 8 stays Red for the Accent style");
    std::set<SwatchId> ids;
    bool unique = true;
    for (const Swatch &s : blank.swatches)
        unique &= ids.insert(s.id).second;
    CHECK(unique, "seeded tables keep unique ids");
    bool seeded_teal = false;
    for (const Swatch &s : blank.swatches)
        seeded_teal |= s.name == "Deep Teal";
    CHECK(seeded_teal, "library colors join new documents");
    nanogui::fs::remove_all(dir);

    const std::string config_path = swatch_library_config_path();
    CHECK(!config_path.empty() &&
              (config_path.find("pagemade/colors.conf") != std::string::npos ||
               config_path == "pagemade-colors.conf"),
          "config path %s", config_path.c_str());
}

/* Export without loading every picture first, flattening, and stopping. */
static void test_pdf_export_jobs() {
    std::string err;
    /* A linked JPEG the store hasn't loaded (it was off screen) */
    const std::string jpg = "/tmp/pagemade-linked-export.jpg";
    {
        const std::vector<uint8_t> bytes = make_jpeg_rgb(16, 8, 20, 120, 220);
        FILE *f = std::fopen(jpg.c_str(), "wb");
        CHECK(f != nullptr, "write the linked jpeg");
        if (f) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
    const ImageImport probed = probe_image_file(jpg);
    CHECK(probed.ok, "probe the linked jpeg");
    ImageStore store;
    PageDoc doc;
    const uint32_t id = store.take_id();
    store.add_link(id, jpg, probed.meta);
    ImageAsset asset;
    asset.id = id;
    asset.name = "linked.jpg";
    asset.meta = probed.meta;
    asset.source = file_uri(jpg);
    doc.add_image(asset);
    Item pic;
    pic.w = 160;
    pic.h = 80;
    pic.content = PlacedImage{id, 0, 0, 160, 80};
    doc.add_item(0, pic);

    PdfSettings o;
    auto unreadable = [&](const std::vector<std::string> &warnings) {
        for (const std::string &w : warnings)
            if (w.find("can't be read") != std::string::npos)
                return true;
        return false;
    };
    CHECK(!store.is_loaded(id) && !unreadable(pdf_preflight(doc, {}, &store, o)),
          "an unloaded link whose file is there is not reported missing");
    const std::string pdf = "/tmp/pagemade-export-jobs.pdf";
    CHECK(export_pdf(pdf, doc, {}, &store, &o), "export with the link unloaded");
    std::string plain = pdf_plain(read_bin(pdf));
    CHECK(plain.find("/Im" + std::to_string(id) + " Do") != std::string::npos &&
              plain.find("/DCTDecode") != std::string::npos,
          "the export reads the linked file itself");
    CHECK(!store.is_loaded(id), "and leaves the store as it was");
    const std::string moved = jpg + ".moved";
    std::rename(jpg.c_str(), moved.c_str());
    CHECK(unreadable(pdf_preflight(doc, {}, &store, o)), "a link whose file is gone is reported");
    std::rename(moved.c_str(), jpg.c_str());

    /* Flatten: the translucent page is rendered by the hook, the opaque
     * one stays vector */
    PageDoc fd;
    fd.insert_page(1);
    const SwatchId glass = fd.add_swatch("Glass", {0.2f, 0.4f, 0.8f, 0.5f});
    const SwatchId solid = fd.add_swatch("Solid", {0.8f, 0.2f, 0.2f, 1.f});
    for (int pg = 0; pg < 2; ++pg) {
        Item box;
        box.w = 100;
        box.h = 50;
        Shape sh;
        sh.fill = Paint{pg == 0 ? glass : solid, 100.f};
        sh.stroke.weight = 0;
        box.content = sh;
        fd.add_item((size_t) pg, box);
    }
    CHECK(draws_transparency(build_page(fd, 0, {}), fd) && !draws_transparency(build_page(fd, 1, {}), fd),
          "draws_transparency finds the translucent fill only");
    PdfSettings flat;
    flat.flatten = true;
    flat.flatten_ppi = 144.f;
    flat.bleed = false;
    int calls = 0, got_w = 0, got_h = 0;
    PdfExportHooks hooks;
    hooks.rasterize = [&](const DrawList &list, const std::map<uint32_t, PdfRasterPicture> &,
                          float, float, float, float, int width, int height,
                          std::vector<uint8_t> &rgba) {
        ++calls;
        got_w = width;
        got_h = height;
        rgba.assign((size_t) width * height * 4, list.empty() ? 0 : 200);
        return true;
    };
    CHECK(export_pdf(pdf, fd, {}, nullptr, &flat, nullptr, &hooks), "export flattened");
    plain = pdf_plain(read_bin(pdf));
    CHECK(calls == 1 && got_w == (int) std::lround(fd.setup.width * 2) &&
              got_h == (int) std::lround(fd.setup.height * 2),
          "one page rendered, at 144 ppi (%d calls, %d x %d)", calls, got_w, got_h);
    CHECK(plain.find("/Fl0 Do") != std::string::npos && plain.find("/Fl1") == std::string::npos &&
              plain.find("/ExtGState") == std::string::npos,
          "the translucent page is a picture; no transparency is left");
    CHECK(export_pdf(pdf, fd, {}, nullptr, &flat), "without a rasterizer");
    plain = pdf_plain(read_bin(pdf));
    CHECK(plain.find("/Fl0") == std::string::npos && plain.find("/ExtGState") != std::string::npos,
          "the page is written as it is");

    /* Progress, and a stop that leaves no file */
    std::remove(pdf.c_str());
    float last = -1.f;
    bool rising = true;
    PdfExportHooks watch;
    watch.progress = [&](float f, const std::string &) {
        rising = rising && f >= last;
        last = f;
        return true;
    };
    CHECK(export_pdf(pdf, doc, {}, &store, &o, nullptr, &watch) && rising && last > 0.999f,
          "progress rises to 1 (%g)", last);
    std::remove(pdf.c_str());
    PdfExportHooks stop;
    stop.progress = [](float, const std::string &) { return false; };
    CHECK(!export_pdf(pdf, doc, {}, &store, &o, nullptr, &stop) && !nanogui::fs::exists(pdf),
          "a stop writes no file");
    std::remove(jpg.c_str());
}

/* fill_windings: holes stay holes, the outline is kept counter-clockwise
 * (NanoVG's sign) whichever way it was drawn, and a mirror changes nothing. */
static void test_fill_windings() {
    auto square = [](Path &p, float x, float y, float w, bool cw) {
        p.push_back({PathCmd::Move, 0, 0, 0, 0, x, y});
        if (cw) {
            p.push_back({PathCmd::Line, 0, 0, 0, 0, x + w, y});
            p.push_back({PathCmd::Line, 0, 0, 0, 0, x + w, y + w});
            p.push_back({PathCmd::Line, 0, 0, 0, 0, x, y + w});
        } else {
            p.push_back({PathCmd::Line, 0, 0, 0, 0, x, y + w});
            p.push_back({PathCmd::Line, 0, 0, 0, 0, x + w, y + w});
            p.push_back({PathCmd::Line, 0, 0, 0, 0, x + w, y});
        }
        p.push_back({PathCmd::Close, 0, 0, 0, 0, 0, 0});
    };
    for (int outer_cw = 0; outer_cw < 2; ++outer_cw) {
        Path p;
        square(p, 0, 0, 10, outer_cw);
        square(p, 2, 2, 2, !outer_cw);     // a hole
        square(p, 6, 6, 2, outer_cw);      // a second solid
        const std::vector<uint8_t> w = fill_windings(p);
        CHECK(w.size() == 3 && w[0] == 1 && w[1] == 0 && w[2] == 1,
              "outline ccw, hole cw, solid ccw (outer drawn %s)", outer_cw ? "cw" : "ccw");
        Path mirrored = p;
        for (PathCmd &c : mirrored)
            c.x = -c.x;
        CHECK(fill_windings(mirrored) == w, "a mirror doesn't change the windings");
    }
}

/* The Image Assets library's folders and files survive a restart. */
static void test_asset_library_config() {
    const std::string dir = "/tmp/pagemade-assets-test";
    nanogui::fs::remove_all(dir);
    const std::string path = dir + "/sub/assets.conf";
    std::vector<std::string> got{"stale"};
    CHECK(!asset_library_load(got, path) && got.empty(), "a missing config is no library");
    const std::vector<std::string> roots{"/mnt/share/Photo Library", "/home/me/pic one.jpg",
                                         "//server/art"};
    CHECK(asset_library_save(roots, path), "save makes the directory and writes");
    CHECK(asset_library_load(got, path) && got == roots, "paths with spaces round-trip in order");
    {
        std::ofstream out(path, std::ios::app);
        out << "\n# a note\n/home/me/pic one.jpg\r\n/extra\n";
    }
    CHECK(asset_library_load(got, path) && got.size() == 4 && got.back() == "/extra",
          "comments, blank lines, CRLF and repeats are skipped (%zu)", got.size());
    CHECK(asset_library_save({}, path) && asset_library_load(got, path) && got.empty(),
          "an emptied library saves empty");
    nanogui::fs::remove_all(dir);
    const std::string config = asset_library_config_path();
    CHECK(config.find("pagemade/assets.conf") != std::string::npos ||
              config == "pagemade-assets.conf",
          "config path %s", config.c_str());
}

static std::vector<uint8_t> bytes_of(const std::string &s) {
    return {s.begin(), s.end()};
}

static bool sniffs_svg(const std::string &xml) {
    return vector_loader().match(reinterpret_cast<const uint8_t *>(xml.data()), xml.size()) != nullptr;
}

static bool decode_svg(const std::string &xml, VectorScene &scene, std::string &err) {
    const std::vector<uint8_t> b = bytes_of(xml);
    return vector_loader().decode(b.data(), b.size(), &scene, &err);
}

static bool path_at(const Path &p, PathCmd::Kind kind, float x, float y) {
    for (const PathCmd &c : p)
        if (c.kind == kind && near(c.x, x) && near(c.y, y))
            return true;
    return false;
}

static int path_kind(const Path &p, PathCmd::Kind kind) {
    int n = 0;
    for (const PathCmd &c : p)
        n += c.kind == kind;
    return n;
}

static std::string join_skipped(const std::vector<std::string> &skipped) {
    std::string s;
    for (const std::string &t : skipped) {
        if (!s.empty())
            s += ",";
        s += t;
    }
    return s;
}

/* SVG place: one graphic, solids and strokes, the frame at the margin.
 * Gradients paint their middle stop and text is named and skipped. A
 * <clipPath> becomes a clip on the shapes; one that can't be read is named. */
static void test_svg(const FontLibrary &fonts) {
    const std::string declared =
        "<?xml version=\"1.0\"?>\n"
        "<!-- saved as icon.txt -->\n"
        "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1 1\"/>";
    CHECK(sniffs_svg(declared), "an svg root sniffs; the .txt name is not consulted");
    CHECK(sniffs_svg("\xEF\xBB\xBF<svg xmlns=\"http://www.w3.org/2000/svg\"/>"), "a BOM still sniffs");
    CHECK(sniffs_svg("<svg:svg xmlns:svg=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 4 4\"/>"),
          "a prefixed svg root sniffs");
    CHECK(!sniffs_svg("<html><body><svg viewBox=\"0 0 1 1\"></svg></body></html>"),
          "an svg later in html is not an svg file");
    CHECK(!sniffs_svg("<SVG viewBox=\"0 0 1 1\"/>"), "the root name is case-sensitive");

    std::string err;
    VectorScene scene;
    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"24\" height=\"24\">"
              "<rect width=\"24\" height=\"24\"/></svg>",
              scene, err),
          "24px icon: %s", err.c_str());
    CHECK(near(scene.width, 18.f) && near(scene.height, 18.f), "24px is 18pt (%g x %g)",
          scene.width, scene.height);
    CHECK(scene.shapes.size() == 1 && path_at(scene.shapes[0].path, PathCmd::Move, 0, 0) &&
              path_at(scene.shapes[0].path, PathCmd::Line, 18, 0) &&
              path_at(scene.shapes[0].path, PathCmd::Line, 18, 18),
          "the view box scales the rect into the 18pt box");

    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\" width=\"10\" height=\"10\">"
              "<g transform=\"translate(2,3)\">"
              "<rect width=\"1\" height=\"1\" stroke=\"black\" stroke-width=\"2\"/>"
              "</g></svg>",
              scene, err),
          "translated rect: %s", err.c_str());
    CHECK(near(scene.width, 7.5f) && scene.shapes.size() == 1, "10px box is 7.5pt");
    if (!scene.shapes.empty()) {
        const VectorShape &sh = scene.shapes[0];
        CHECK(path_at(sh.path, PathCmd::Move, 1.5f, 2.25f) &&
                  path_at(sh.path, PathCmd::Line, 2.25f, 2.25f) &&
                  path_at(sh.path, PathCmd::Line, 2.25f, 3.f) &&
                  path_at(sh.path, PathCmd::Line, 1.5f, 3.f),
              "translate then the 0.75 scale bakes the unit rect");
        CHECK(near(sh.stroke.width, 1.5f), "stroke width scales with the view box (%g)", sh.stroke.width);
    }

    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 30 30\" width=\"30\">"
              "<path d=\"M10 20 A10 10 0 0 1 20 20\" fill=\"none\" stroke=\"black\"/></svg>",
              scene, err),
          "arc: %s", err.c_str());
    CHECK(!scene.shapes.empty() && path_kind(scene.shapes[0].path, PathCmd::Cubic) >= 1 &&
              path_at(scene.shapes[0].path, PathCmd::Cubic, 15.f, 15.f),
          "an arc becomes cubics in the point box");

    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\" width=\"10\">"
              "<style>rect { fill:#0000ff } .cls { fill:#ff0000 }</style>"
              "<rect class=\"cls\" fill=\"#ffffff\" width=\"2\" height=\"2\"/>"
              "<rect style=\"fill:#00ff00\" x=\"3\" width=\"2\" height=\"2\"/>"
              "</svg>",
              scene, err),
          "styles: %s", err.c_str());
    CHECK(scene.shapes.size() == 2, "two styled rects (%zu)", scene.shapes.size());
    if (scene.shapes.size() == 2) {
        check_color(scene.shapes[0].fill.color, Color{1, 0, 0, 1}, "class over presentation and element");
        check_color(scene.shapes[1].fill.color, Color{0, 1, 0, 1}, "style attribute");
    }

    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\" width=\"10\">"
              "<defs><linearGradient id=\"g\">"
              "<stop offset=\"0\" stop-color=\"#000000\"/>"
              "<stop offset=\"0.5\" stop-color=\"#123456\"/>"
              "<stop offset=\"1\" stop-color=\"#ffffff\"/>"
              "</linearGradient>"
              "<clipPath id=\"c\"><rect width=\"2\" height=\"2\"/></clipPath></defs>"
              "<rect width=\"10\" height=\"10\" fill=\"url(#g)\" clip-path=\"url(#c)\"/>"
              "<text x=\"1\" y=\"2\">Hi</text></svg>",
              scene, err),
          "skipped features: %s", err.c_str());
    CHECK(scene.shapes.size() == 1, "the clipped rect is still drawn (%zu)", scene.shapes.size());
    if (!scene.shapes.empty()) {
        const Color mid{18.f / 255.f, 52.f / 255.f, 86.f / 255.f, 1.f};
        check_color(scene.shapes[0].fill.color, mid, "middle gradient stop");
        CHECK(path_at(scene.shapes[0].path, PathCmd::Line, 7.5f, 0),
              "the clip does not trim the rect");
        const int ci = scene.shapes[0].clip;
        CHECK(ci == 0 && scene.clips.size() == 1 && scene.clips[0].size() == 1 &&
                  scene.clips[0][0].size() == 1 &&
                  path_at(scene.clips[0][0][0].path, PathCmd::Line, 1.5f, 0),
              "the rect carries the 2px clip in points (clip %d)", ci);
    }
    if (!scene.shapes.empty()) {
        const VectorPaint &g = scene.shapes[0].fill;
        CHECK(g.kind == PaintKind::Linear && g.stops.size() == 3 && near(g.xf.a, 7.5f) &&
                  near(g.xf.d, 7.5f) && near(g.xf.b, 0) && near(g.xf.e, 0),
              "a bounding-box gradient spans the rect (%g %g %g %g)", g.xf.a, g.xf.b, g.xf.d, g.xf.e);
    }
    CHECK(join_skipped(scene.skipped) == "text", "skipped %s", join_skipped(scene.skipped).c_str());
    CHECK(vector_place_note("mark.svg", {"gradients", "clips", "text"}) ==
              "placed mark.svg (gradients, clips, text skipped)",
          "the status names what was skipped");

    /* A clip is in the referencing element's user space (after its own
     * transform), a bounding-box clip scales to the element, nested clips
     * intersect, an empty clip removes the element and a basic-shape clip
     * is named and left out. */
    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\" width=\"100\">"
              "<defs><clipPath id=\"a\"><rect x=\"10\" width=\"10\" height=\"10\"/></clipPath>"
              "<clipPath id=\"b\" clipPathUnits=\"objectBoundingBox\">"
              "<rect width=\"0.5\" height=\"1\" clip-rule=\"evenodd\"/></clipPath>"
              "<clipPath id=\"e\"/></defs>"
              "<g clip-path=\"url(#a)\" transform=\"translate(20 0)\">"
              "<rect width=\"40\" height=\"40\" clip-path=\"url(#b)\"/>"
              "<rect y=\"50\" width=\"10\" height=\"10\"/></g>"
              "<rect width=\"5\" height=\"5\" clip-path=\"url(#e)\"/>"
              "<rect width=\"5\" height=\"5\" clip-path=\"circle(2px)\"/></svg>",
              scene, err),
          "clips: %s", err.c_str());
    CHECK(scene.shapes.size() == 3, "the empty clip removes its rect (%zu)", scene.shapes.size());
    if (scene.shapes.size() == 3) {
        const VectorShape &both = scene.shapes[0], &outer = scene.shapes[1];
        CHECK(both.clip >= 0 && outer.clip >= 0 && both.clip != outer.clip &&
                  scene.shapes[2].clip == -1,
              "each clipped rect has its own clip; the basic shape has none");
        if (both.clip >= 0 && outer.clip >= 0) {
            const ClipLayers &bl = scene.clips[(size_t) both.clip];
            const ClipLayers &ol = scene.clips[(size_t) outer.clip];
            CHECK(bl.size() == 2 && ol.size() == 1, "nested clips are two layers (%zu, %zu)",
                  bl.size(), ol.size());
            if (bl.size() == 2 && ol.size() == 1) {
                CHECK(path_at(bl[0][0].path, PathCmd::Move, 15.f, 0) &&
                          path_at(bl[0][0].path, PathCmd::Line, 30.f, 0) &&
                          bl[0][0].rule == FillRule::EvenOdd,
                      "the bounding-box clip covers the rect's left half, even-odd");
                CHECK(path_at(bl[1][0].path, PathCmd::Move, 22.5f, 0) &&
                          path_at(ol[0][0].path, PathCmd::Move, 22.5f, 0),
                      "the group's clip moves with the group's transform");
            }
        }
    }
    CHECK(join_skipped(scene.skipped) == "clips", "the basic shape is named (%s)",
          join_skipped(scene.skipped).c_str());

    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\""
              " viewBox=\"0 0 4 4\" width=\"4\"><defs>"
              "<linearGradient id=\"base\"><stop offset=\"0\" stop-color=\"#000\"/>"
              "<stop offset=\".3\" stop-color=\"#999\"/><stop offset=\"1\" stop-color=\"#fff\"/>"
              "</linearGradient>"
              "<linearGradient id=\"mid\" xlink:href=\"#base\"/>"
              "<radialGradient id=\"r\" xlink:href=\"#mid\" cx=\"2\" cy=\"2\" r=\"2\"/></defs>"
              "<rect width=\"1\" height=\"1\" fill=\"url(#r)\"/></svg>",
              scene, err) &&
              scene.shapes.size() == 1 && scene.shapes[0].fill.kind == PaintKind::Radial &&
              scene.shapes[0].fill.stops.size() == 3 && near(scene.shapes[0].fill.color.r, 0.6f),
          "a gradient takes its stops through a chain of xlink:href");

    /* Gradient geometry: user-space units with a transform and a focal
     * point, attributes lent through href, spread, opacity on the stops,
     * and the cases that paint solid or nothing. */
    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\""
              " viewBox=\"0 0 40 40\" width=\"40\"><defs>"
              "<radialGradient id=\"r\" gradientUnits=\"userSpaceOnUse\" cx=\"10\" cy=\"10\" r=\"5\""
              " fx=\"12.5\" spreadMethod=\"reflect\" gradientTransform=\"translate(10 0)\">"
              "<stop offset=\"0\" stop-color=\"#f00\" stop-opacity=\".5\"/>"
              "<stop offset=\"1\" stop-color=\"#00f\"/></radialGradient>"
              "<linearGradient id=\"down\" x2=\"0\" y2=\"1\"><stop offset=\"0\"/>"
              "<stop offset=\"1\" stop-color=\"#fff\"/></linearGradient>"
              "<linearGradient id=\"lent\" xlink:href=\"#down\"/>"
              "<linearGradient id=\"one\"><stop stop-color=\"#0f0\"/></linearGradient>"
              "<linearGradient id=\"empty\"/></defs>"
              "<rect width=\"40\" height=\"40\" fill=\"url(#r)\" fill-opacity=\".5\"/>"
              "<rect x=\"0\" y=\"20\" width=\"10\" height=\"20\" fill=\"url(#lent)\"/>"
              "<rect width=\"4\" height=\"4\" fill=\"url(#one)\"/>"
              "<rect width=\"4\" height=\"4\" fill=\"url(#empty)\"/>"
              "<line x2=\"10\" stroke=\"url(#down)\"/></svg>",
              scene, err),
          "gradients: %s", err.c_str());
    CHECK(scene.shapes.size() == 3, "no stops and a flat bounding box paint nothing (%zu)",
          scene.shapes.size());
    if (scene.shapes.size() == 3) {
        const VectorPaint &r = scene.shapes[0].fill;
        CHECK(r.kind == PaintKind::Radial && near(r.xf.a, 3.75f) && near(r.xf.e, 15.f) &&
                  near(r.xf.f, 7.5f) && near(r.focal.x, 0.5f) && near(r.focal.y, 0) &&
                  r.spread == Spread::Reflect,
              "user-space radial: center (%g,%g), radius %g, focal %g", r.xf.e, r.xf.f, r.xf.a,
              r.focal.x);
        CHECK(r.stops.size() == 2 && near(r.stops[0].color.a, 0.25f) && near(r.stops[1].color.a, 0.5f),
              "stop-opacity and fill-opacity scale the stops");
        const VectorPaint &v = scene.shapes[1].fill;
        CHECK(v.kind == PaintKind::Linear && near(v.xf.a, 0) && near(v.xf.b, 15.f) &&
                  near(v.xf.c, -7.5f) && near(v.xf.f, 15.f),
              "href lends x2/y2: top to bottom of the rect (%g %g %g %g)", v.xf.a, v.xf.b, v.xf.c,
              v.xf.f);
        CHECK(scene.shapes[2].fill.kind == PaintKind::Solid && near(scene.shapes[2].fill.color.g, 1.f),
              "one stop paints solid");
    }

    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 4 4\" width=\"4\">"
              "<rect width=\"1\" height=\"1\" fill=\"url(#missing)\"/></svg>",
              scene, err) &&
              !scene.shapes.empty() && scene.shapes[0].fill.color.r == 0 &&
              join_skipped(scene.skipped) == "gradients",
          "a missing gradient paints black and is noted");

    CHECK(decode_svg(
              "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 20 20\" width=\"20\">"
              "<g display=\"none\"><rect width=\"20\" height=\"20\"/></g>"
              "<path fill-rule=\"evenodd\" d=\"M0 0 H10 V10 H0 Z M2 2 H8 V8 H2 Z\"/>"
              "<path fill-rule=\"nonzero\" d=\"M0 12 H10 V20 H0 Z M2 14 H8 V18 H2 Z\"/>"
              "</svg>",
              scene, err),
          "fill rules: %s", err.c_str());
    CHECK(scene.shapes.size() == 2, "display:none draws nothing; two paths remain (%zu)",
          scene.shapes.size());
    if (scene.shapes.size() == 2) {
        CHECK(scene.shapes[0].rule == FillRule::EvenOdd && path_kind(scene.shapes[0].path, PathCmd::Move) == 2,
              "even-odd keeps the hole subpath");
        CHECK(scene.shapes[1].rule == FillRule::NonZero && path_kind(scene.shapes[1].path, PathCmd::Move) == 2,
              "nonzero keeps its subpaths too");
    }

    const std::string icon =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"24\" height=\"24\">"
        "<path fill-rule=\"evenodd\" d=\"M0 0 H24 V24 H0 Z M6 6 H18 V18 H6 Z\"/>"
        "</svg>";
    PageDoc doc;
    VectorStore store;
    VectorPlacement bad = place_svg_bytes(doc, store, bytes_of("<html></html>"), "no.svg", "", 0, nullptr);
    CHECK(!bad.ok && bad.asset == 0 && store.find(1) == nullptr, "a bad file takes no id");
    VectorPlacement placed = place_svg_bytes(doc, store, bytes_of(icon), "icon.svg", "", 0, nullptr);
    CHECK(placed.ok, "place: %s", placed.error.c_str());
    CHECK(placed.asset == 1 && placed.note == "placed icon.svg", "first graphic is asset 1");
    CHECK(doc.next_asset == 1, "graphic ids do not share the picture counter");
    const Item *it = doc.find_item(placed.item);
    CHECK(it && it->is_vector() && it->vector() && it->vector()->asset == placed.asset, "the item is a graphic");
    if (it) {
        CHECK(near(it->w, 18.f) && near(it->h, 18.f) && near(it->xf.e, 54.f) && near(it->xf.f, 54.f),
              "an 18pt graphic sits at the margin (%g,%g %g x %g)", it->xf.e, it->xf.f, it->w, it->h);
    }
    VectorPlacement again =
        place_svg_bytes(doc, store, bytes_of(icon), "icon.svg", "", 0, nullptr);
    CHECK(again.ok && again.asset == 2 && doc.vectors.size() == 2 && doc.pages[0].items.size() == 2,
          "a second place adds another graphic");

    const std::string wide =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 400 100\" width=\"400pt\" height=\"100pt\">"
        "<rect width=\"400\" height=\"100\"/></svg>";
    PageDoc fitted;
    VectorStore wide_store;
    VectorPlacement wide_place =
        place_svg_bytes(fitted, wide_store, bytes_of(wide), "wide.svg", "", 0, nullptr);
    CHECK(wide_place.ok, "wide place: %s", wide_place.error.c_str());
    if (const VectorScene *ws = wide_store.scene(wide_place.asset))
        CHECK(near(ws->width, 400.f) && near(ws->height, 100.f), "400pt by 100pt (%g x %g)",
              ws->width, ws->height);
    if (const Item *w = fitted.find_item(wide_place.item))
        CHECK(near(w->w, 243.f) && near(w->h, 60.75f) && near(w->xf.e, 54.f) && near(w->xf.f, 54.f),
              "wider than the column becomes 243 by 60.75 at the margin");
    Point drop{400.f, 120.f};
    VectorPlacement dropped =
        place_svg_bytes(fitted, wide_store, bytes_of(wide), "wide.svg", "", 0, &drop);
    if (const Item *w = fitted.find_item(dropped.item))
        CHECK(near(w->w, 243.f) && near(w->h, 60.75f) && near(w->xf.e, 315.f) && near(w->xf.f, 120.f),
              "a drop in the second column fits that column (%g,%g)", w->xf.e, w->xf.f);

    const std::string tall =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 2000\" width=\"100pt\" height=\"2000pt\">"
        "<rect width=\"100\" height=\"2000\"/></svg>";
    PageDoc tall_doc;
    VectorStore tall_store;
    VectorPlacement tall_place =
        place_svg_bytes(tall_doc, tall_store, bytes_of(tall), "tall.svg", "", 0, nullptr);
    CHECK(tall_place.ok, "tall place: %s", tall_place.error.c_str());
    if (const Item *t = tall_doc.find_item(tall_place.item))
        CHECK(near(t->w, 34.2f) && near(t->h, 684.f) && near(t->xf.e, 158.4f) && near(t->xf.f, 54.f),
              "taller than the live area shrinks and stays centered in the column (%g,%g %g x %g)",
              t->xf.e, t->xf.f, t->w, t->h);

    /* The draw list and the PDF see the scene. A null store draws nothing. */
    PageDoc one;
    VectorStore one_store;
    VectorPlacement one_place = place_svg_bytes(one, one_store, bytes_of(icon), "icon.svg", "", 0, nullptr);
    CHECK(build_page(one, 0, {}, nullptr, false, nullptr).empty(), "no store draws no graphic");
    DrawList drawn = build_page(one, 0, {}, nullptr, false, &one_store);
    const DrawFill *fill = nullptr;
    for (const DrawOp &op : drawn)
        if (const DrawFill *f = std::get_if<DrawFill>(&op))
            fill = f;
    CHECK(fill && fill->rule == FillRule::EvenOdd && near(fill->clip_w, 18.f) && near(fill->clip_h, 18.f),
          "the frame clips and the fill is even-odd");
    if (fill)
        CHECK(path_at(fill->path, PathCmd::Move, 4.5f, 4.5f), "the hole is in the frame's box");

    const std::string pdf_path = "/tmp/pagemade-svg.pdf";
    std::vector<Composition> comps;
    CHECK(export_pdf(pdf_path, one, comps, nullptr, nullptr, &one_store), "export the graphic");
    std::string plain = pdf_plain(read_bin(pdf_path));
    CHECK(plain.find("0 0 18 18 re W n") != std::string::npos, "the frame clips with W n");
    CHECK(plain.find("q 1 0 0 1 54 54 cm") != std::string::npos, "the frame is at the margin");
    CHECK(plain.find("f*\n") != std::string::npos, "even-odd fills with f*");

    const std::string solid =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"24\">"
        "<rect width=\"24\" height=\"24\"/></svg>";
    PageDoc nz;
    VectorStore nz_store;
    CHECK(place_svg_bytes(nz, nz_store, bytes_of(solid), "solid.svg", "", 0, nullptr).ok, "nonzero place");
    CHECK(export_pdf(pdf_path, nz, comps, nullptr, nullptr, &nz_store), "export nonzero");
    plain = pdf_plain(read_bin(pdf_path));
    CHECK(plain.find("f*\n") == std::string::npos && plain.find("f\n") != std::string::npos,
          "nonzero fills with f");
    CHECK(plain.find("0 0 18 18 re W n") != std::string::npos, "the nonzero frame clips too");

    /* A clip path reaches the draw list in the frame's box and the PDF as W n. */
    const std::string clipped =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"24\">"
        "<clipPath id=\"c\"><circle cx=\"12\" cy=\"12\" r=\"8\"/></clipPath>"
        "<rect width=\"24\" height=\"24\" clip-path=\"url(#c)\"/>"
        "<rect width=\"4\" height=\"4\" clip-path=\"url(#c)\" stroke=\"red\"/></svg>";
    PageDoc cd;
    VectorStore cd_store;
    CHECK(place_svg_bytes(cd, cd_store, bytes_of(clipped), "clipped.svg", "", 0, nullptr).ok,
          "clipped place");
    drawn = build_page(cd, 0, {}, nullptr, false, &cd_store);
    std::vector<DrawClip> seen;
    for (const DrawOp &op : drawn) {
        if (const DrawFill *f = std::get_if<DrawFill>(&op))
            seen.push_back(f->clip);
        else if (const DrawStroke *st = std::get_if<DrawStroke>(&op))
            seen.push_back(st->clip);
    }
    CHECK(seen.size() == 3 && seen[0] && seen[0] == seen[1] && seen[1] == seen[2],
          "the fills and the stroke share one clip (%zu ops)", seen.size());
    if (!seen.empty() && seen[0] && seen[0]->size() == 1 && (*seen[0])[0].size() == 1)
        CHECK(path_at((*seen[0])[0][0].path, PathCmd::Move, 9.f, 3.f),
              "the clip is in the frame's box");
    CHECK(export_pdf(pdf_path, cd, comps, nullptr, nullptr, &cd_store), "export clipped");
    plain = pdf_plain(read_bin(pdf_path));
    CHECK(plain.find("h\nW n\n") != std::string::npos, "the clip path closes and clips with W n");

    /* Gradients reach the draw list in the frame's box and the PDF as
     * shading patterns; varying stop alpha is a soft mask. */
    const std::string shaded =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"24\"><defs>"
        "<linearGradient id=\"l\"><stop offset=\"0\" stop-color=\"#f00\"/>"
        "<stop offset=\".5\" stop-color=\"#0f0\"/><stop offset=\"1\" stop-color=\"#00f\"/>"
        "</linearGradient><radialGradient id=\"r\"><stop offset=\"0\" stop-color=\"#fff\"/>"
        "<stop offset=\"1\" stop-color=\"#000\" stop-opacity=\"0\"/></radialGradient></defs>"
        "<rect width=\"24\" height=\"12\" fill=\"url(#l)\"/>"
        "<rect y=\"12\" width=\"24\" height=\"12\" fill=\"url(#r)\"/></svg>";
    PageDoc gd;
    VectorStore gd_store;
    CHECK(place_svg_bytes(gd, gd_store, bytes_of(shaded), "shaded.svg", "", 0, nullptr).ok,
          "gradient place");
    drawn = build_page(gd, 0, {}, nullptr, false, &gd_store);
    std::vector<const DrawFill *> gfills;
    for (const DrawOp &op : drawn)
        if (const DrawFill *f = std::get_if<DrawFill>(&op))
            gfills.push_back(f);
    CHECK(gfills.size() == 2 && gfills[0]->gradient && gfills[1]->gradient &&
              !gfills[0]->gradient->radial && gfills[1]->gradient->radial,
          "both fills carry their gradients");
    if (gfills.size() == 2 && gfills[0]->gradient)
        CHECK(near(gfills[0]->gradient->xf.a, 18.f) && near(gfills[0]->gradient->xf.e, 0),
              "the linear gradient spans the frame (%g)", gfills[0]->gradient->xf.a);
    CHECK(export_pdf(pdf_path, gd, comps, nullptr, nullptr, &gd_store), "export gradients");
    plain = pdf_plain(read_bin(pdf_path));
    CHECK(plain.find("/Pattern cs /P1 scn") != std::string::npos &&
              plain.find("/ShadingType 2") != std::string::npos &&
              plain.find("/ShadingType 3") != std::string::npos &&
              plain.find("/FunctionType 3") != std::string::npos,
          "shading patterns, a three-stop function");
    CHECK(plain.find("/SMask") != std::string::npos && plain.find("/M2 gs") != std::string::npos &&
              plain.find("/Sh0 sh") != std::string::npos,
          "the fading radial gradient is soft-masked");

    /* A paste has no source file, so the bytes are stored even when the
     * publication otherwise links its assets. */
    const std::string pkg = "/tmp/pagemade-svg.pagemade";
    CHECK(save_document(one, fonts, pkg, SaveOptions{false}, &err, nullptr, &one_store),
          "save: %s", err.c_str());
    OpenResult opened = open_document(pkg, fonts);
    CHECK(opened.ok, "open: %s", opened.error.c_str());
    if (opened.ok) {
        check_doc(one, opened.doc);
        CHECK(opened.doc.vectors.size() == 1 && opened.doc.vectors[0].id == one_place.asset &&
                  opened.doc.vectors[0].name == "icon.svg" && near(opened.doc.vectors[0].width, 18.f),
              "the graphic record round-trips");
        CHECK(opened.doc.pages[0].items.size() == 1 && opened.doc.pages[0].items[0].is_vector(),
              "the opened item is still a graphic");
        VectorStore::Bytes back = opened.vectors.source_of(one_place.asset);
        CHECK(back && *back == bytes_of(icon), "the package holds the svg bytes");
        const VectorScene *again_scene = opened.vectors.scene(one_place.asset);
        CHECK(again_scene && again_scene->shapes.size() == 1 &&
                  again_scene->shapes[0].rule == FillRule::EvenOdd,
              "opening decodes the scene again");
        const std::string json = document_json(one, fonts, SaveOptions{false}, nullptr, &one_store);
        CHECK(json.find("\"kind\": \"vector\"") != std::string::npos &&
                  json.find("assets/vectors/") != std::string::npos,
              "document.json names the graphic");
    }

    const std::string link_file = "/tmp/pagemade-link.svg";
    {
        FILE *f = std::fopen(link_file.c_str(), "wb");
        CHECK(f != nullptr, "write the linked svg");
        if (f) {
            std::fwrite(icon.data(), 1, icon.size(), f);
            std::fclose(f);
        }
    }
    PageDoc linked;
    VectorStore link_store;
    VectorPlacement link_place =
        place_svg_bytes(linked, link_store, bytes_of(icon), "icon.svg", file_uri(link_file), 0, nullptr);
    const std::string link_pkg = "/tmp/pagemade-svg-link.pagemade";
    CHECK(link_place.ok &&
              save_document(linked, fonts, link_pkg, SaveOptions{false}, &err, nullptr, &link_store),
          "save the link: %s", err.c_str());
    OpenResult from_link = open_document(link_pkg, fonts, false);
    CHECK(from_link.ok, "open a linked graphic: %s", from_link.error.c_str());
    if (from_link.ok) {
        VectorStore::Bytes back = from_link.vectors.source_of(link_place.asset);
        CHECK(back && *back == bytes_of(icon), "a linked svg is read even when pictures would wait");
    }
    std::remove(link_file.c_str());
    OpenResult missing = open_document(link_pkg, fonts, false);
    CHECK(missing.ok, "a missing linked graphic still opens");
    bool warned = false;
    for (const std::string &w : missing.warnings)
        warned |= w.find("couldn't read the graphic") != std::string::npos;
    CHECK(warned, "a missing linked graphic is reported");

    std::remove(pdf_path.c_str());
    std::remove(pkg.c_str());
    std::remove(link_pkg.c_str());
    std::remove(link_file.c_str());
}

static void test_system_fonts() {
    FontLibrary lib;
    register_default_fonts(lib);
    register_system_fonts(lib);
    const std::vector<std::string> fams = lib.families();
    CHECK(fams.size() >= 3, "at least the three built-in families (%zu)", fams.size());
    if (fams.size() >= 3)
        CHECK(fams[0] == "Serif" && fams[1] == "Sans" && fams[2] == "Display",
              "built-ins stay first (%s, %s, %s)",
              fams[0].c_str(), fams[1].c_str(), fams[2].c_str());
    if (fams.size() > 3) {
        const Font *loaded = nullptr;
        std::string which;
        const size_t stop = std::min(fams.size(), (size_t) 11);
        for (size_t i = 3; i < stop && !loaded; ++i) {
            if (const Font *f = lib.face_for(fams[i], false, false)) {
                loaded = f;
                which = fams[i];
            }
        }
        CHECK(loaded && loaded->units_per_em() > 0, "an installed family loads");
        if (loaded)
            std::printf("  system fonts: %zu families, loaded %s\n", fams.size(), which.c_str());
    } else {
        std::printf("  system fonts: fc-list added none\n");
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
    test_outline_cache(fonts);
    test_line_spacing(fonts);
    test_paragraph_attributes(fonts);
    test_drop_caps(fonts);
    test_keeps(fonts);
    test_special_chars(fonts);
    test_lists(fonts);
    test_styles();
    test_edit(fonts);
    test_align_ranges(fonts);
    test_pdf(fonts);
    test_tounicode(fonts);
    test_pdf_alpha(fonts);
    test_deterministic(fonts);
    test_model(fonts);
    test_tabs(fonts);
    test_hyphenation(fonts);
    test_swatches();
    test_geometry();
    test_arrange();
    test_drawlist(fonts);
    test_docfile(fonts);
    test_images(fonts);
    test_svg(fonts);
    test_asset_library_config();
    test_fill_windings();
    test_pdf_export_jobs();
    test_picture_style(fonts);
    test_groups(fonts);
    test_page_markers(fonts);
    test_layers(fonts);
    test_document_setup(fonts);
    test_pdf_settings(fonts);
    test_strict_loose(fonts);
    test_contents_tabs(fonts);
    test_image_color(fonts);
    test_background_worker();
    test_caps(fonts);
    test_type_file(fonts);
    test_font_menu();
    test_swatch_library();
    test_system_fonts();
    if (verbose)
        dump_sample(fonts);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
