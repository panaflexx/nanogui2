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
#include "drawlist.h"
#include "geometry.h"
#include "page.h"
#include "pdf.h"
#include "docfile.h"
#include "uri.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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
    CHECK(a.family == b.family && a.bold == b.bold && a.italic == b.italic, "char style face");
    CHECK(near(a.size, b.size) && near(a.leading, b.leading) && near(a.tracking, b.tracking) &&
          near(a.hscale, b.hscale) && near(a.baseline_shift, b.baseline_shift),
          "char style metrics");
    CHECK(a.kerning == b.kerning && a.ligatures == b.ligatures, "char style switches");
    check_color(a.color, b.color, "type");
}

static void check_para(const ParaStyle &a, const ParaStyle &b) {
    CHECK(a.name == b.name && a.align == b.align, "paragraph name/align");
    CHECK(near(a.left_indent, b.left_indent) && near(a.right_indent, b.right_indent) &&
          near(a.first_indent, b.first_indent) && near(a.space_before, b.space_before) &&
          near(a.space_after, b.space_after) && near(a.autoleading, b.autoleading),
          "paragraph spacing");
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
          a.setup.columns == b.setup.columns && near(a.setup.gutter, b.setup.gutter),
          "page setup");
    CHECK(a.swatches.size() == b.swatches.size(), "swatches %zu vs %zu",
          a.swatches.size(), b.swatches.size());
    for (size_t i = 0; i < std::min(a.swatches.size(), b.swatches.size()); ++i) {
        CHECK(a.swatches[i].id == b.swatches[i].id && a.swatches[i].name == b.swatches[i].name,
              "swatch %zu", i);
        check_color(a.swatches[i].rgb, b.swatches[i].rgb, "swatch");
    }
    CHECK(a.pages.size() == b.pages.size(), "pages %zu vs %zu", a.pages.size(), b.pages.size());
    for (size_t p = 0; p < std::min(a.pages.size(), b.pages.size()); ++p) {
        CHECK(a.pages[p].hidden == b.pages[p].hidden, "page %zu hidden", p);
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

static void test_docfile(const FontLibrary &fonts) {
    test_uri();

    PageDoc doc = sample_document();
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
    ParaStyle note_ps;
    note_ps.name = "Note";
    note_ps.align = Align::Right;
    note_ps.hyphenate = false;
    note_ps.hyphen_limit = 2;
    note_ps.hyphen_zone = 28;
    note_ps.default_tab = 48;
    note_ps.word_min = 80;
    note_ps.tabs.push_back({120, TabAlign::Decimal, "."});
    Story extra = one_para("He said \"hello\"\nsecond\tline\\end", note, note_ps);
    doc.add_text_frame(1, doc.add_story(std::move(extra)), 200, 80, Transform::translate(40, 200));

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
        "\"format\":\"pagemade\",\"version\":2,"
        "\"pages\":[{\"hidden\":true,\"items\":["
        "{\"id\":5,\"type\":\"text\",\"w\":100,\"h\":40,\"xf\":[1,0,0,1,10,20]},"
        "{\"id\":5,\"type\":\"shape\",\"shape\":\"rectangle\",\"w\":10,\"h\":10,\"xf\":[1,0,0,1,0,0]},"
        "{\"id\":0,\"type\":\"text\",\"w\":1,\"h\":1,\"xf\":[1,0,0,1,0,0]},"
        "{\"id\":7,\"type\":\"text\",\"w\":30,\"h\":20,\"xf\":[1,0,0,1,4,6]},"
        "{\"id\":8,\"type\":\"image\",\"w\":1,\"h\":1,\"xf\":[1,0,0,1,0,0]}"
        "]}],"
        "\"stories\":["
        "{\"id\":9,\"thread\":[5,99],\"paragraphs\":[{\"runs\":[{\"text\":\"Kept\"}]}]},"
        "{\"id\":9,\"thread\":[],\"paragraphs\":[{\"runs\":[{\"text\":\"Gone\"}]}]}"
        "]}";
    const std::string repair_path = "/tmp/pagemade-doc-repair.pagemade";
    CHECK(write_zip(repair_path, kPublicationMimeType, repair_json), "write a damaged package");
    OpenResult repaired = open_document(repair_path, fonts);
    CHECK(repaired.ok, "a newer, damaged file still opens: %s", repaired.error.c_str());
    CHECK(!repaired.warnings.empty(), "damage and a newer version are reported");
    if (repaired.ok) {
        CHECK(repaired.doc.pages.size() == 1 && repaired.doc.pages[0].hidden, "the page survived");
        CHECK(repaired.doc.pages[0].items.size() == 2, "duplicate, zero and unknown items drop");
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

    std::remove(linked.c_str());
    std::remove(embedded.c_str());
    std::remove(keep.c_str());
    std::remove(repair_path.c_str());
    std::remove(bad.c_str());
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
    test_pdf(fonts);
    test_deterministic(fonts);
    test_model(fonts);
    test_tabs(fonts);
    test_hyphenation(fonts);
    test_swatches();
    test_geometry();
    test_drawlist(fonts);
    test_docfile(fonts);
    if (verbose)
        dump_sample(fonts);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
