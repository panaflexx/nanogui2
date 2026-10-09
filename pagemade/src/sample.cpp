/*
 * pagemade/sample.cpp — the demo newsletter page (see page.h).
 */
#include "page.h"

namespace pagemade {

namespace {

Paragraph para(const ParaStyle &ps, std::initializer_list<Run> runs) {
    Paragraph p;
    p.style = ps;
    p.runs = runs;
    return p;
}

} // namespace

PageDoc sample_document() {
    PageDoc doc;
    const PageSetup &s = doc.setup;
    const float col_w = (s.width - s.margin_inside - s.margin_outside -
                         s.gutter * (s.columns - 1)) / s.columns;

    /* ---- Headline flow ---- */
    CharStyle head;
    head.family = "Display"; head.bold = true; head.size = 34; head.leading = 36;
    CharStyle deck;
    deck.family = "Serif"; deck.italic = true; deck.size = 15; deck.leading = 19;
    deck.color = {0.25f, 0.25f, 0.25f, 1};
    CharStyle byline;
    byline.family = "Sans"; byline.bold = true; byline.size = 8.5f; byline.tracking = 120;

    ParaStyle head_ps;   head_ps.name = "Headline"; head_ps.align = Align::Center;
    ParaStyle deck_ps;   deck_ps.name = "Deck";     deck_ps.align = Align::Center;
    deck_ps.space_before = 8;
    deck_ps.left_indent = deck_ps.right_indent = 36;
    ParaStyle byline_ps; byline_ps.name = "Byline"; byline_ps.align = Align::Center;
    byline_ps.space_before = 12;

    TextFlow headline;
    headline.story.paragraphs = {
        para(head_ps,   {{head, "WAVE AFTER WAVE OF TYPE"}}),
        para(deck_ps,   {{deck, "A first look at the pagemade composer: every glyph shaped "
                                "by HarfBuzz, every line broken in points, and every page "
                                "drawn the same way at any zoom."}}),
        para(byline_ps, {{byline, "THE PASTEBOARD · VOLUME 1, NUMBER 1 · OCTOBER 2026"}}),
    };
    headline.frames = {{s.margin_inside, s.margin_top, s.width - s.margin_inside - s.margin_outside, 150}};

    /* ---- Body flow ---- */
    CharStyle body;
    body.family = "Serif"; body.size = 10.5f; body.leading = 13;
    CharStyle body_bold = body;
    body_bold.bold = true;
    CharStyle sub;
    sub.family = "Sans"; sub.bold = true; sub.size = 11; sub.leading = 13;

    ParaStyle first_ps; first_ps.name = "Body text"; first_ps.align = Align::Justify;
    ParaStyle body_ps = first_ps;
    body_ps.first_indent = 12;
    ParaStyle sub_ps;   sub_ps.name = "Subhead 1"; sub_ps.space_before = 9; sub_ps.space_after = 2;

    TextFlow article;
    article.story.paragraphs = {
        para(first_ps, {{body_bold, "Desktop publishing began"},
                        {body, " with a simple promise: what you see on the screen is what "
                               "comes out of the printer. Aldus PageMaker kept that promise by "
                               "treating the page as a pasteboard, where text and pictures "
                               "could be picked up, moved and set down again, while the type "
                               "itself was measured with the same font metrics the LaserWriter "
                               "would use."}}),
        para(body_ps, {{body, "pagemade follows the same rule. Every glyph is shaped by HarfBuzz "
                              "from the font’s own design units, so pair kerning, ligatures "
                              "such as fi and fl, and tracking are settled before a single pixel "
                              "is drawn. Lines are broken in points rather than pixels, which "
                              "means a column breaks the same way at 25 percent, at 800 percent, "
                              "and in the PDF that will eventually go to press."}}),
        para(sub_ps, {{sub, "Spacing attributes"}}),
        para(first_ps, {{body, "Justified type uses PageMaker’s spacing attributes. Word "
                               "spaces may shrink to 75 percent of the font’s space band or "
                               "stretch to 150 percent; when that isn’t enough, letter "
                               "spacing takes up the slack, from −5 to +25 percent. Lines "
                               "that still come out too loose or too tight can be highlighted, "
                               "just as the old Preferences dialog offered."}}),
        para(body_ps, {{body, "Each line’s slug is as tall as its largest leading, with "
                              "the baseline two thirds of the way down. That is PageMaker’s "
                              "proportional leading, and it is why a bit of oversized type "
                              "raises its whole line instead of colliding with the one above."}}),
        para(sub_ps, {{sub, "Threaded text blocks"}}),
        para(first_ps, {{body, "Text flows from one column to the next through threaded text "
                               "blocks. When a story runs out of room, the bottom windowshade "
                               "handle shows a red arrow, just as it did in 1985. Drag the "
                               "handle down, and the story reflows to fill the space."}}),
        para(body_ps, {{body, "Awkward pairs are a quick test of the kerning tables: AVATAR, "
                              "WAVE, Toyota, Yokohama, LT, “quoted” words and "
                              "1,234.56 figures. Turn kerning off in the toolbar and watch the "
                              "gaps open up; turn ligatures off and the fi in “efficient” "
                              "and the fl in “flow” come apart again."}}),
        para(sub_ps, {{sub, "What comes next"}}),
        para(first_ps, {{body, "Hyphenation, tab stops, a paragraph composer that weighs every "
                               "break at once, text wrap around graphics, and PDF output with "
                               "embedded fonts. The composer already hands every backend the "
                               "same positioned glyphs, so each of those can be checked against "
                               "the screen point for point."}}),
    };
    const float top = s.margin_top + 162;
    const float bottom = s.height - s.margin_bottom;
    article.frames = {
        {s.margin_inside, top, col_w, bottom - top},
        {s.margin_inside + col_w + s.gutter, top, col_w, 120},
    };

    doc.flows = {headline, article};
    return doc;
}

} // namespace pagemade
