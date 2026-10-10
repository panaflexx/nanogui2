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
  right, first-line and last-line indents, and space before and after. It
  works on whole paragraphs. A last line too long for its indent breaks
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

1. **SVG import**: nanosvg makes a group of path items in the shape format. This
   step brings even-odd fill and per-subpath winding to nanovgd. The fill rule
   reaches the backend through the `renderFill` hook, which the Metal backend
   (`ext/nanovg_metal`) implements too, so Mac builds need the matching change.
   Multi-stop gradients come with it. Rectangular picture crops are already
   clipped; this step is where a clip becomes an arbitrary path.
2. **Text wrap**: lines ask which horizontal spans are free at their height,
   from the wrap outlines of items in front of the text.
3. **Print dialog**: choose the printer, copies and page range. Linux first (the
   CUPS API with our own dialog, or GTK's), then Mac (NSPrintOperation on the
   PDF), then Windows. Today printing exports a PDF and hands it to `lp`.

## Later and known gaps

- Small text on screen: a nanovgd entry point that draws cached glyph bitmaps by
  glyph id at exact composed positions; cache drawn pages as display lists.
- Paragraph-at-a-time (Knuth-Plass) composer; right-to-left text.
- Text colors as swatches (character styles still carry RGB).
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
- Text formatting, roughly in order of payoff:
  - The rest of ParaStyle in the panel (or a Specs dialog off it):
    autoleading percent, hyphenation, the spacing attributes, tab stops.
  - Styles that carry character color, and a Styles palette for editing a
    style without a paragraph to make it from (and "based on" chains).
  - Text color from swatches (with tint), replacing CharStyle's RGB.
  - Keep lines together, keep with next, and widow/orphan control. The
    composer breaks frames line by line today, so this needs lookahead at
    frame ends.
  - Superscript/subscript (PageMaker's Position: size and offset percents)
    and real small caps or old-style figures from OpenType features (smcp,
    onum, lnum, tnum) when the face has them.
  - Paragraph rules (lines above/below) and drop caps.
  - Align to grid: snap baselines to a leading grid across columns.
  - Whether a block selected with the pointer should restyle its whole story
    (PageMaker does) rather than only the text in that block (pagemade does).


# Code review: pagemade text engine (composer / edit / font / hyphenator / pdf)

P1 items 1–3 are fixed and covered by `pagemade_compose_test`. Items 4–16 are still open.

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

**4. `pdf.cpp:69-78, 98-101` — Flate failure silently yields a corrupt PDF.**
If `mz_compress2` fails, `flate()` returns `""` and `add_stream` writes a 0-length `/FlateDecode` stream with no error; `export_pdf` still returns true.
*Fix:* have `flate`/`add_stream` propagate failure (e.g. `std::optional` or a `Writer::ok` flag checked in `export_pdf`).

**5. `composer.cpp:228` — Run index truncated to 16 bits.**
`g.run = (uint16_t) ri`; a paragraph with >65535 runs (possible via many alternating-style edits — `normalize` only merges same-style neighbors) wraps and assigns glyphs the wrong style/font. `PGlyph` has padding, so widening is free.
*Fix:* make `PGlyph::run` `uint32_t`.

**6. `pdf.cpp:326-328, 339` — Alpha is silently dropped.**
`emit_color` writes only `rg/RG`; semi-transparent text/fills that the screen renders translucent come out fully opaque in PDF, with no note in the header comment ("Colors are device RGB for now" doesn't mention alpha).
*Fix:* emit a `/ca` ExtGState for `a < 1`, or document the limitation explicitly.

**7. `pdf.cpp:225-229` — `static int tag_seq` is shared mutable state.**
The subset-tag counter persists across exports and is a data race if export ever runs off the UI thread; two documents exported in one process also share the sequence (harmless but needless).
*Fix:* make the counter a local in `export_pdf` threaded into `embed_font`.

**8. `edit.cpp:544-545` vs `composer.cpp:108-111` — `line_end` doesn't recognize all break chars the composer does.**
`line_end` trims ` `, 0xA0, `\n`, `\r`, 0x2028, 0x2029 but not 0x0B, 0x0C, 0x85, which `is_break()` treats as breaks; a line ending in VT/FF/NEL leaves the End-key caret *after* the invisible break char.
*Fix:* share one `is_break` helper between the two files.

## P3 — Duplicated logic

**9. UTF-8 decoding is implemented five times with subtly different invalid-input behavior.**
`composer.cpp` (`decode_utf8`, `utf8_len_at`, `char_before`, and the lead-byte walker in hyphenation points), `edit.cpp` (`decode`, `char_len`, and the continuation walk in `line_end`), `hyphenator.cpp` (`utf8_to_u32`), and `pdf.cpp` (`utf16_hex`). They diverge on malformed input (e.g. a continuation byte as a lead: `edit.cpp` treats it as a 2-byte lead, `hyphenator.cpp` maps it to `c & 0x7F`, `composer.cpp` to `c & 0x3F`), so caret math and shaping can disagree on corrupt text. `pdf.cpp`'s separate `char_len` was removed with item 2.
*Fix:* one shared `utf8.h` with `decode`/`length`/`prev` used everywhere.

**10. Underline/strike span computation is duplicated almost verbatim.**
`pdf.cpp:352-374` (`emit_glyphs`) and `render_nvg.cpp:72-96` (`draw_glyph_run`) both compute the visible-glyph x-span and emit the two rules from `underline_position/thickness` and `strike_position/thickness`.
*Fix:* extract a helper that returns the rule rects `(x0, x1, y, thickness)` for a run, used by both backends.

## P4 — Performance (compose runs per keystroke)

**11. `composer.cpp:463-471, 519-530` — `fits()` re-flattens and re-measures the whole accumulated line for every piece tried.**
Each `fits` call copies all accepted glyphs (`flatten`) and re-runs `measure`→`layout` (including tab resolution), making line-building O(L²) in glyphs per line; the overflow fallback then multiplies this by the box length, doing a full `slice` copy + `fits` per cluster boundary — O(box²) copies for a long unbreakable segment (a 5 000-char URL pasted into a narrow frame = ~25 M glyph copies on one keystroke).
*Fix:* keep a running `Measure` for the accepted pieces (incremental per piece, reset only when a piece is hyphenated/reshaped), and scan cluster boundaries with a running width instead of re-slicing and re-measuring each candidate.

**12. `hyphenator.cpp:113-120` — One heap allocation per pattern probe, per word, per compose.**
`m_patterns.find(w.substr(i, len))` allocates a fresh `std::u32string` for every (position, length) pair — O(n·max_len) allocations per word — and `Breaker::m_points` is rebuilt from scratch on every keystroke because `Breaker` is reconstructed in `compose()`.
*Fix:* key the map so lookup needs no allocation (e.g. a trie over `char32_t`, or `std::map<std::u32string,…,std::less<>>` with `u32string_view` transparent lookup).

## P5 — Dead code / error-handling gaps

**13. `font.cpp:54-63` (`Font::load_memory`) is unused** — no callers anywhere in the tree (only the declaration at `font.h:51`). *Fix:* remove it or use it for the built-in fonts.

**14. `pdf.cpp:509, 573` — `pages_kids_placeholder` is dead**: declared, `(void)`-cast, never used. *Fix:* delete it.

**15. `hyphenator.cpp:82-90` — An explicitly passed exceptions file that fails to open is silently ignored** (`std::ifstream ex` failure is unchecked), so a misspelled path degrades hyphenation with no signal. *Fix:* check `ex.is_open()` and report/return false.

**16. `font.cpp:79` — No guard against `upem == 0`.** A malformed font with glyph count > 0 but upem 0 flows into `cs.size / units_per_em()` (`composer.cpp:215`), `em()` (`pdf.cpp:200`), and `render_nvg.cpp:16`, producing infinities. *Fix:* clamp `m_upem` to ≥ 1 in `from_blob`.

---

**Verified non-issues** (checked and deliberately not reported): the odd-looking mask `c & (0x3F >> n)` / `c & (0x7F >> n)` in the UTF-8 decoders is equivalent to the canonical mask because the branching already constrains the lead-byte ranges; `%04X` gid formatting is safe because OpenType `numGlyphs` is uint16-bounded so gids never exceed 0xFFFF; `erase()`'s `first` reference stays valid across `s.paragraphs.erase()` because the erased range starts after it; `next_line` always makes progress (every path consumes the carry or advances `next_seg`), so no infinite loop on zero/negative measure.
