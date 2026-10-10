# pagemade plan

A modern reincarnation of Aldus PageMaker (the Mac version) on the nanogui2 fork
and its nanovgd renderer. The composer lays out in points and every backend
(screen, PDF, SVG) draws the same positioned glyphs and paths.

## Done

- **Composer** (`src/composer/`): HarfBuzz shaping in font units (GPOS kerning,
  ligatures, tracking, horizontal scale), greedy line breaking, justification
  with PageMaker's spacing attributes, proportional leading, threading through
  frames, overset.
- **Line breaking and hyphenation**: UAX #14 via vendored libunibreak, Liang
  pattern hyphenation (TeX hyph-utf8 en-US) plus discretionary hyphens, hyphen
  zone and consecutive-hyphen limit, reshaping wherever HarfBuzz flags a break
  unsafe.
- **Tab stops**: left, center, right and decimal, with leaders; default stops.
- **Text editing**: text tool with caret, selection, word/paragraph clicks,
  clipboard, bold/italic, grapheme and word movement; threading gestures (pick
  up the overset arrow, merge back).
- **Undo/redo**: whole-document snapshots, 100 deep, one step per action.
- **PDF export and printing** (`src/pdf.cpp`): the draw list's second
  backend. Every page at trim size, paths and strokes (native PDF dashes),
  text as real text — each used face subset with hb-subset (glyph ids
  retained, so character codes are glyph ids under Identity-H), embedded as
  CIDFontType0 (raw CFF program) or CIDFontType2, with a ToUnicode map for
  search and copy. Toolbar buttons and `--export-pdf`; printing exports and
  hands the PDF to CUPS `lp`. Colors are device RGB still; CMYK/spot and
  trim/bleed/marks remain open.
- **Pages**: insert before or after the page on screen, drag the page icons
  (or Layout > Move Page) to reorder, hide a page so it stays editable but
  is left out of PDF and print, remove a page and the items on it. The last
  page cannot be removed. Layout menu, and PageMaker's page icons along the
  bottom of the window.
- **Type**: the control palette's type view (the square / A button, or it
  switches itself when the text tool is chosen). Font (Serif, Sans and
  Display, then the installed families), size, style (that family's faces),
  leading (Auto, or a point size) and baseline shift. Element > Type Specs…
  (Ctrl/Cmd+T), and Specs… on the palette, set weight, roman/italic/oblique,
  caps, set width, tracking, underline and strikethrough, with a preview of
  the face. A text selection is restyled; a caret changes what is typed
  next; text blocks selected with the pointer restyle the type in those
  blocks; with nothing selected, the fields are the defaults for a new
  text block. OK in Type Specs changes only the fields that were edited.
  Line spacing (0.1–3, a paragraph attribute) multiplies each line's slug,
  auto or fixed leading alike.
- **Paragraph panel**: the ¶ button on the toolbar (or Element > Paragraph…,
  Ctrl/Cmd+M) pops open a panel laid out after VectorStyler's. It holds the
  style menu, seven alignments (justified with the last line left, centered
  or right, and force justify), leading as a percent, extra spacing, left,
  right, first-line and last-line indents, space before and after, and drop
  caps. It works on whole paragraphs. A drop cap enlarges the first
  characters so their cap height runs from the first line's cap height to
  line N's baseline, sized from there by a percent; the lines it reaches
  move over by its width, and a short paragraph pushes the next one below
  it. The cap stays text: the caret, selection and PDF treat it as such. A last line too long for its indent breaks
  again. Named styles live in the publication (Normal, PageMaker's
  predefined set, Pull quote, Byline, plus the sample's own). Applying one
  keeps italic or bold emphasis, underline and color; the menu shows "Name+"
  for overrides; Add makes or redefines a style from the paragraph (its
  paragraphs without overrides follow); Remove deletes one.
- **Spinners**: number fields in the palettes and Type Specs step with their
  arrows, the scroll wheel or a right-drag (`spinbox.h`). A run of steps is
  one undo step, and it keeps changing the text it started on even as that
  text reflows out of (or into) the selected block.
- **Arrange**: Element menu, and the keys. Bring to Front (Ctrl+F), Bring
  Forward (Ctrl+]), Send Backward (Ctrl+[), Send to Back (Ctrl+B). Shift
  with the brackets jumps to the front or the back. A group moves together
  and keeps its order.
- **Page items foundation**: pages holding items in stacking order; stable ids;
  per-item size and transform; stories with threads of frame ids; minimal shapes;
  rotate tool; Delete; Bring to Front / Send to Back.
- **Drawing pipeline and shapes**: `drawlist` turns a page into fills, strokes
  and glyph runs in points (the NanoVG renderer consumes it); `geometry` holds
  shape outlines, flattening and dashing for every backend. Line, rectangle
  (rounded corners), ellipse and polygon/star tools; swatches with tints; stroke
  weights and line styles (solid, dashed, dotted, dash-dot). Multiple selection
  (Shift+click, marquee), moving and rotating the selection together, arrow-key
  nudges, snapping to page edges, margins and columns, new text blocks drawn
  with the text tool, and a control palette (X/Y/W/H, angle, fill, stroke,
  polygon sides and star inset).
- **Publications**: File > New, Open, Save and Save As. A `.pagemade` file is a
  zip package (`mimetype`, `document.json`, and `assets/fonts/…` when assets
  are embedded). Linked fonts are `file:` URIs; an embedded copy keeps its
  source URI so it can be relinked. Save writes beside the target and renames
  over it. New, Open and Quit ask before discarding changes. The window title
  shows a star until the publication is saved again, including after undo.
- **Images, with cropping**: a picture is a publication asset. The page item
  stores the asset id and a crop rectangle; the bytes live in an ImageStore
  beside the document, so undo stays small. File > Place (Ctrl/Cmd+D) puts a
  PNG or JPEG at the margin at print size. The composer draws a screen-sized
  display view; PDF export embeds the source (a JPEG's own DCT stream, anything
  else the full-resolution pixels), so a RIP samples the file and not the
  proxy. The crop tool trims the frame or pans the picture inside it (Shift
  locks one axis); the pointer scales the picture with the frame. The frame is
  a rectangle: the screen intersects the scissor, and PDF clips with `W n`.
  Clipping to an arbitrary path stays with SVG import. Another format registers
  an ImageDecoder (TIFF and HEIC are not built in). The asset table is what a
  later library will move between documents and pages; that window is not built.

## Next, in order

1. **Keep options and breaks**: keep lines together, keep with next, widow and
   orphan control (minimum lines at the top and bottom of a text block), start
   in the next text block, and a frame break character (Ctrl/Cmd+Return).
   The composer gains lookahead at frame ends: a paragraph that breaks a rule
   is composed again from a checkpoint.
2. **Character styles and swatch colors**: text color is a swatch with a tint,
   as shapes are; named character styles set only the attributes they define
   (an Emphasis style is just italic), with a menu that shows overrides.
3. **Special characters and smart quotes**: curly quotes as you type (a
   preference); em and en dashes; em, en, thin and hair spaces that keep their
   width when justified; non-breaking space and hyphen; a No break attribute
   that keeps a word or phrase on one line. Insert them from a menu.
4. **Bullets and numbering**: a list attribute on the paragraph (bullet or
   number format, start value), the marker set in the hanging indent, numbers
   counted through consecutive list paragraphs.
5. **SVG import**: nanosvg makes a group of path items in the shape format. This
   step brings even-odd fill and per-subpath winding to nanovgd. The fill rule
   reaches the backend through the `renderFill` hook, which the Metal backend
   (`ext/nanovg_metal`) implements too, so Mac builds need the matching change.
   Multi-stop gradients come with it. Rectangular picture crops are already
   clipped; this step is where a clip becomes an arbitrary path.
6. **Text wrap**: lines ask which horizontal spans are free at their height,
   from the wrap outlines of items in front of the text.
7. **Print dialog**: choose the printer, copies and page range. Linux first (the
   CUPS API with our own dialog, or GTK's), then Mac (NSPrintOperation on the
   PDF), then Windows. Today printing exports a PDF and hands it to `lp`.

## Later and known gaps

- Small text on screen: a nanovgd entry point that draws cached glyph bitmaps by
  glyph id at exact composed positions; cache drawn pages as display lists.
- Paragraph-at-a-time (Knuth-Plass) composer; right-to-left text.
- Groups; master pages; spreads or a continuous page view (the view shows one
  page at a time). An asset library that moves pictures between documents and
  pages; the publication's asset table is the record it will move.
- Import: Markdown/HTML (gumbo), RTF, DOCX; old PageMaker files via libpagemaker.
- Story editor; Styles and Colors palettes; the rest of the Control palette
  (its object and type views are done).
- Print production: CMYK and spot colors, bleed, crop and registration marks,
  PDF/X.
- Fonts on macOS and Windows (only Linux paths, then Roboto from `resources/`).
- Status bar doesn't refresh after a pinch zoom.
- Resizing a multiple selection as one group (each item resizes on its own today).
- Ruler guides (dragged from rulers) to snap to; rulers themselves.
- Adding and editing swatches (the Colors palette); the palette's swatch menus
  are built when a document is loaded.
- Fills and strokes for text blocks (PageMaker 7 frames).
- Text and type, the gaps from professional layout software (outlines and
  tables are left out for now), roughly in order of payoff:
  - Styles: "based on" chains and "next style" for paragraph styles, and a
    Styles palette for editing a style without a paragraph to make it from.
  - The rest of ParaStyle in the panel (or a Specs dialog off it):
    autoleading percent, hyphenation settings, the spacing attributes, tab
    stops.
  - OpenType features: old-style, lining, tabular and proportional figures,
    fractions, ordinals, discretionary ligatures, stylistic sets, swashes.
    Superscript and subscript from sups/subs, synthesized (PageMaker's size
    and position percents) when the face has neither.
  - Manual kerning at the caret with keyboard nudges; tracking shortcuts.
  - Optical margin alignment (hanging punctuation), and glyph scaling
    (about ±3%) as a third justification tool after word and letter spacing.
  - Paragraph-at-a-time (Knuth-Plass) composer (see above).
  - Underline and strikethrough options: weight, offset, color, dashes.
  - Text frame options: inset, vertical alignment (top, center, bottom,
    justified), first-baseline offset, columns inside one frame, paragraphs
    that span columns; a baseline grid with align to grid.
  - Inline graphics anchored in the text flow (they share plumbing with
    text wrap).
  - Find and Change, including formatting; spell check (Hunspell) with a
    language per paragraph or run, which hyphenation then follows (English
    only today); the Story Editor; show invisibles.
  - Fonts: a missing-font alert on open (the list is already collected),
    Find Font, per-glyph fallback to another font (emoji, CJK), variable
    font axes.
  - Automatic page number markers (with master pages).
  - Paragraph rules (lines above/below). Drop cap options: a raised cap
    (baseline on line 1), a font or color of its own, and text that follows
    the cap's outline instead of its box.
  - Nested numbering levels and list continuation across interruptions.
  - Whether a block selected with the pointer should restyle its whole story
    (PageMaker does) rather than only the text in that block (pagemade does).


# Code review: pagemade text engine (composer / edit / font / hyphenator / pdf)

Items 1–16 are fixed. `pagemade_compose_test` covers the behavior changes: trailing breaks, ToUnicode, the outline cache, End on VT/FF/NEL, a missing exceptions file, and ExtGState alpha.

## P1 — Correctness bugs

**1. Fixed. A forced break as the paragraph's final character was dropped.**
`Breaker::segment()` skipped the break class of the last byte (`end >= t.size()`), and the trailing segment was always `forced=false`, so `m_need_empty` never ran. A paragraph ending in `\n` or U+2028 (Shift+Return) produced no empty line, and the caret stayed on the text.
The last segment is now forced when `brks[t.size()-1] == LINEBREAK_MUSTBREAK`. A trailing break opens an empty line and the caret sits on it. Two trailing breaks open two lines. A break in the middle of the paragraph is unchanged (`test_breaks`).

**2. Fixed. A ligature at the end of a GlyphRun got a truncated ToUnicode string.**
`collect_unicodes` ended a glyph at the next higher cluster in the same run, otherwise at one character (`char_len`). A final "fi", or one ending a run, copied as "f".
The end is now the next glyph on the line, including a later run, or the line's `byte_end` when the ligature ends the line. The one-character fallback in `pdf.cpp` is gone. "fi" maps to both characters and does not swallow the following run (`test_tounicode`).

**3. Fixed. `Font::outline()` returned a reference into a mutating `std::unordered_map`.**
Inserting a gid could rehash and invalidate an outline held across another `outline()` call (`render_nvg.cpp` binds the reference while drawing).
Outlines live in a `std::deque`; the map stores pointers. A returned outline stays at that address for the life of the Font (`test_outline_cache`).

## P2 — Robustness / spec issues

**4. Fixed. A failed Flate compression wrote a 0-length stream and still returned true.**
`flate` returns `std::optional`. `Writer::ok` is cleared when `mz_compress2` fails, and `write` returns false without creating the file. An empty input still compresses to a zlib header, so that is not treated as failure.

**5. Fixed. The run index was truncated to 16 bits.**
`PGlyph::run` is `uint32_t`. A paragraph with many alternating-style runs no longer wraps the index and picks up the wrong face.

**6. Fixed. PDF export dropped alpha.**
A color with alpha below 1 still writes device RGB, then selects an ExtGState named `/A` plus the alpha in thousandths, with both `/ca` and `/CA`. Opaque colors emit the same operators as before, and the sample document's PDF stays the same size. `pdf.h` describes the ExtGState. (`test_pdf_alpha`)

**7. Fixed. The subset-tag counter was a function-local static.**
`tag_seq` is a local in `export_pdf` and is passed into `embed_font`, so exports do not share a counter.

**8. Fixed. End did not treat VT, form feed, or NEL as hanging breaks.**
`is_break` lives in `composer/utf8.h`. `line_end` uses it, so the caret stops before U+000B, U+000C, and U+0085 the same way it stops before a newline. (`test_breaks`)

## P3 — Duplicated logic

**9. Fixed. UTF-8 decoding was copied in the composer, the editor, the hyphenator, and the PDF writer.**
`composer/utf8.h` provides `utf8_decode`, `utf8_len`, and `utf8_prev`. Shaping, caret math, hyphenation points, `utf8_to_u32`, and the ToUnicode encoder all call them. A continuation byte is one byte. Valid text, including ToUnicode for "fi", is unchanged.

**10. Fixed. Underline and strike geometry was duplicated in the PDF and NanoVG backends.**
`glyph_run_rules` in `composer.h` returns the spans `(x0, y, x1, thickness)`, underline then strike. Both backends stroke those spans.

## P4 — Performance (compose runs per keystroke)

**11. Fixed. `fits` copied and remeasured the whole line for every piece, and the emergency split copied a prefix per cluster.**
Accepted pieces stay in one buffer. A try appends, measures, and drops the extra; the hyphen zone uses that cached measure. A tab-free overflow is one forward pass at desired spacing. A tabbed prefix is measured in place, with no slice copy per cluster. Line breaks, justification, tabs, and hyphenation are unchanged.

**12. Fixed. Each pattern probe allocated a `u32string`.**
Patterns are a `std::map` with a transparent comparator. `points` looks up a `u32string_view` into `.word.`. Hyphenation points for "hyphenation" and the "ta-ble" exception are unchanged. (`test_hyphenation`)

## P5 — Dead code / error-handling gaps

**13. Fixed. `Font::load_memory` had no callers.**
The declaration and the definition are gone. Built-in faces still load from files.

**14. Fixed. `pages_kids_placeholder` was unused.**
The variable and its `(void)` cast are gone. `/Parent` is still patched after the Pages object exists.

**15. Fixed. A missing exceptions file was ignored.**
`Hyphenator::load` returns false when a non-empty exceptions path does not open. An empty path is still optional. (`test_hyphenation`)

**16. Fixed. A font with units-per-em of 0 could divide by zero.**
`from_blob` clamps `m_upem` to at least 1 before advances, PDF widths, or screen drawing divide by it.

---

**Verified non-issues** (checked and deliberately not reported): the odd-looking mask `c & (0x3F >> n)` / `c & (0x7F >> n)` in the UTF-8 decoders is equivalent to the canonical mask because the branching already constrains the lead-byte ranges; `%04X` gid formatting is safe because OpenType `numGlyphs` is uint16-bounded so gids never exceed 0xFFFF; `erase()`'s `first` reference stays valid across `s.paragraphs.erase()` because the erased range starts after it; `next_line` always makes progress (every path consumes the carry or advances `next_seg`), so no infinite loop on zero/negative measure.
