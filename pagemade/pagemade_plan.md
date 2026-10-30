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
- **Keep options and breaks**: keep lines together, keep with next, widow and
  orphan control (the fewest lines at the top and bottom of a text block),
  start in the next text block (Paragraph panel), and a frame break
  character, U+000C (Ctrl/Cmd+Return, or Type > Frame Break). The composer
  saves a checkpoint before each paragraph; one that breaks a rule gets a
  decision (start in the next block, or end a block early so a widow has
  company) and is composed again from there. Decisions only accumulate, so
  composition settles. Headings in the default styles keep with next; body
  text keeps two lines at a block break.
- **Character styles and swatch colors**: text color is a swatch with a tint,
  resolved by the draw list for the screen and PDF (text from before
  swatches keeps its RGB). Character styles set only the fields they define
  (Emphasis is just italic; Accent is bold red); "Add" makes one from what
  the selection changes from its paragraph's type. The Character panel (the
  A button, Element > Character…, Ctrl/Cmd+Shift+T) has the style menu
  ("Name+" for overrides), color, tint, Remove and Add. Paragraph styles
  keep a run's character style.
- **Special characters and smart quotes**: the Type menu turns typographer's
  quotes on and off and inserts dashes, fixed spaces (em, en, thin, hair:
  they keep their width when justified, and a face without them gets the
  standard width), non-breaking space and hyphen, discretionary hyphen,
  common symbols, and line and frame breaks, with InDesign's shortcuts where
  it has them. No break (Type Specs) keeps a phrase on one line.
- **Bullets and numbering**: a list on the paragraph (three bullets; 1, a, A,
  i, I numbers with a suffix; a start value), set from the Paragraph panel.
  The marker hangs at the first-line indent and the text sits at the left
  indent; numbers count through consecutive numbered paragraphs. The marker
  is inserted text: carets and copied text pass over it. Bulleted list and
  Numbered list styles.
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
  Clipping a picture to an arbitrary path is still later. Another format registers
  an ImageDecoder (TIFF and HEIC are not built in). The asset table is what a
  later library will move between documents and pages; that window is not built.
- **SVG import and paste**: File > Place and a paste of SVG add one placed
  graphic on the active layer, as one undo step. The decoder is linked in from
  `pagemade/src/plugin/` (a small XML reader and the SVG reader); nothing is
  loaded from a directory at run time. The page item names the asset. The file
  bytes and the parsed scene live in a VectorStore beside the document, and
  both screen and PDF draw that scene. The scene is already in the graphic's
  box, in points: viewBox, transforms, presentation attributes, and one
  `<style>` block of element and class selectors. CSS px at 96dpi is 0.75pt.
  Wider than the column under the drop shrinks to that column and keeps its
  aspect; taller than the live area shrinks the same way. Solids and strokes
  draw, arcs become cubics, and even-odd fill reaches nanovgd and the Metal
  backend. A gradient paints the stop nearest its middle. A clip or some text
  is named in the status line; the shape is drawn in full, and text draws
  nothing. The frame clips like a picture. Copying one placed SVG also puts
  its source on the system clipboard. Point editing of an imported path is not
  in this slice. A `.pagemade` file stores the bytes under `assets/vectors/`.
- **Layout templates** (`layout_templates.h`, the Layouts tab): page designs of
  empty picture frames, formatted text, shapes and inline SVG, in their own box
  with margins. Dragged onto a page (or double-clicked), the content area is
  fitted into the live area, scaled the same both ways and centered, with type
  scaled along; the items are one group, one undo step. A spread template on a
  facing spread splits at the spine (one group per page). The Rotate menu turns
  the layout 90/180/270° while frame contents stay upright. The templates live
  in `~/.config/pagemade/layouts.json`, which is seeded with 32 built-ins;
  Save Page adds the selection (or page) as a layout, Remove and Reload edit
  the file.
- **Tool windows as tabs** (`tool_tabs.h`): a `ToolTabsWindow` (dockable) holds
  `ToolPanel`s as tabs; only the panel in front refreshes, the others catch up
  in `shown()`. Image Assets is Images + Layouts. Layers and Type Specs are
  the same kind of panel, and a tab can be dragged onto another tool window
  or off into its own. Swatch dialogs stay separate. The last section is the
  detail.
- **Print dialog** (`print_dialog.h`, File > Print and Ctrl+P): printer, copies,
  and all, current, or a page range. The dialog does not spool. It returns a
  `PrintChoice`; the app writes that range to a PDF (trim, no bleed or marks,
  hidden pages left out) and `print_pdf` (`print.h`) hands the file to the
  spooler. Linux and macOS use `lp` and `lpstat`. Windows says printing is not
  available yet.
- **Copy to other programs**: Copy with the pointer offers the items as SVG
  (`image/svg+xml`, `svg_export.h`) at their size in points. Shapes, rules,
  borders, placed graphics and pictures come from the draw list (pictures as
  embedded sRGB JPEG or PNG, about 300 ppi as placed, clipped to the frame);
  text is editable `<text>` per composed line, named by the font file's own
  family, with tracking, caps, bullets, leaders and break hyphens. Groups are
  `<g>`. The clipboard holds several formats at once through
  `glfwSetClipboardFormats`, added to the bundled GLFW (X11 with INCR for
  large data, and Wayland; macOS and Windows offer the text format only so
  far). Next: RTF and HTML for text.

## Next, in order

1. **Text wrap**: lines ask which horizontal spans are free at their height,
   from the wrap outlines of items in front of the text.

## Later and known gaps

- Small text on screen: a nanovgd entry point that draws cached glyph bitmaps by
  glyph id at exact composed positions; cache drawn pages as display lists.
- Paragraph-at-a-time (Knuth-Plass) composer; right-to-left text.
- Groups; master pages; spreads or a continuous page view (the view shows one
  page at a time). An asset library that moves pictures between documents and
  pages; the publication's asset table is the record it will move.
- SVG still to come: a gradient drawn as its real stops, and clipping a shape
  to a path. The frame stays a rectangle until then.
- Import: Markdown/HTML (gumbo), RTF, DOCX; old PageMaker files via libpagemaker.
- Story editor; Styles and Colors palettes; the rest of the Control palette
  (its object and type views are done).
- Tool-window tabs, tear-off, the green drop, and a one-level vertical split
  are in the code (the last section). A hand check of that section is still
  open. Swatch dialogs are not part of it.
- Windows printing. The print dialog is the same one; `print_none.cpp` is the stub that says it is not available yet.
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
  - Nested numbering levels and list continuation across interruptions;
    a custom bullet character and number suffix in the panel (the model and
    file have them).
  - Smart quotes: a leading apostrophe ('90s) types as an opening quote;
    "--" to an em dash as you type.
  - Keep rules work between text blocks; columns inside one block (a
    later frame option) will need them too.
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

---

# Code review: application, threading, UI, PDF, images

Checked against the current sources (October 2026). Items 1–16 above still hold. This pass is the rest of the app: `PagemadeApp`, `BackgroundWorker`, the image store and asset window, `ComposerView` drawing and loading, and `export_pdf`.

What is already in good shape, and should stay:

- Picture and SVG bytes live in `ImageStore` / `VectorStore`, beside the document, so an undo snapshot copies the layout and the stories, not the pixels.
- Display decodes run on `BackgroundWorker`. Results come back through `nanogui::async`, and `ComposerView`'s load gate drops them after the view, or its document, is gone. The short-circuit is `!gate->open` before any use of the view pointer.
- A JPEG that is embedded at full size keeps its DCT stream. PDF page drawing that needs GL is posted back to the UI thread and waited on with a future, so the worker never touches the GL context.
- The asset-library directory scan is a joinable `std::thread` with an atomic stop flag. `stop_scan` joins it. That is the lifetime model the other workers should follow.
- Flate failure sets `Writer::ok` and `write` returns false without creating the file.

## Bugs

**17. Fixed. The export dialog was not modal for the mouse, and the worker used live stores.**
Clicks and the wheel were already swallowed while a modal window is up. Motion behind it, and a file drop outside it, now are too (`Screen`, `front_modal`). An open menu popup still receives its own clicks.

`run_pdf_export` copies the document and the compositions, then `ImageStore::snapshot` and `VectorStore::snapshot`, and `FontLibrary::retain` for every face those compositions use. The thread calls `export_pdf` on the copies. `clear_document_fonts`, `m_images.clear`, and `m_vectors.clear` no longer race the worker. `picture_source` still copies a `shared_ptr` to the file bytes; the worker does not call `find` or `cached_display`. (`test_export_snapshots`)

**18. Fixed. The PDF thread had no exception barrier, and the app never joined it on the way out.**
The job in `run_pdf_export` catches exceptions, so `std::bad_alloc` from the giant PDF string closes the dialog instead of killing the process. `~PagemadeApp` sets cancel and pumps `process_async` until `m_pdf_thread` is joined, before the stores die. A plain `join` would deadlock: the worker can be waiting on a rasterize posted back to this thread. Shutdown does not run the export's `done` callback.

**19. Fixed. A failed or cancelled export truncated an existing PDF.**
`Writer::write` writes `path.saving` and renames over the target, the same way `save_document` does. A short write removes the temporary file and leaves the old PDF. Cancel still returns before `write`. (`test_pdf_export_jobs`)

**20. Fixed. Downsampling averaged straight RGBA.**
`downsample_rgba` premultiplies, averages, then unpremultiplies. `make_display` and `resample_rgba` / `embed_picture` both use it, so screen and print stay matched. An opaque red pixel beside a clear pixel stays red, at the averaged alpha. (`test_image_color`)

**21. Fixed. Replacing a keyed job skipped the thread-spawn retry.**
`BackgroundWorker::post` falls through to the spawn check after a keyed replace. If the first `std::thread` constructor threw, a later post of the same key starts the worker instead of leaving `running` false.

**Progress. Fixed. Export and print progress did not repaint.**
The status line and the bar sit under a cached dialog draw list. `Label::set_caption` and `ProgressBar::set_value` now dirty that list, the two widgets are live so a parent bake cannot freeze them, and the progress callback calls `redraw`. With `mainloop(-1)` an empty event alone does not draw a frame.

## Performance

**22. Fixed. Screen proxies decoded the full picture, then converted color, then shrank.**
With libjpeg (libjpeg-turbo; optional in CMake, `PAGEMADE_HAVE_LIBJPEG`), `JpegDecoder::decode_preview` decodes at the largest 1/2, 1/4 or 1/8 scale that still covers the long edge asked for, and `make_display` box-filters the rest. A JPEG whose EXIF thumbnail covers the request, at the photo's proportions (a letterboxed one is refused), is decoded from the thumbnail. CMYK and YCCK JPEGs, and anything libjpeg rejects, keep the stb path. `make_display` now shrinks first and converts the ICC profile on the pixels it keeps, for every format. Flattened PDF pages get the same scaled decodes.

Measured with `pagemade_render_bench --decode` on 12 MP phone JPEGs: a 256 px view 29–38 ms (was 111–120), 512 px 42–49 ms (104–122), 1024 px 78–88 ms (134–152); 2048 px is unchanged (no scale applies). The floor is libjpeg's entropy decode of the whole file. Mean difference from the old path 0.5–2.5 levels, single pixels up to about 90 at hard edges (the DCT scale filters differently). (`test_jpeg_previews`)

**23. Fixed. One loader thread, failed pictures never retried, proxies never shrank.**
`BackgroundWorker` takes a thread count; the display loader runs two, so a slow HEIC no longer holds up the JPEG beside it. Other workers stay at one. A failed picture is drawn as missing and read again in the background after 15 s, the wait doubling to five minutes (`LoadFailure`), so a remounted share or a fixed file comes back; placing, replacing or reloading it clears the failure at once. The retry runs on the loader, so a dead share never blocks the UI. Once the zoom has been still for a second (a redraw is booked for then), a proxy more than one size step above the request is decoded again smaller from the loaded bytes (`ImageStore::shrink_proxy`, `request_display(..., shrink)`), which frees the large texture and RGBA buffer. (`test_background_pool`)

**24. Fixed. Every keystroke recomposed every story, twice when replacing a selection.**
`after_edit` recomposes only the story being edited (`recompose_story`): a story's layout depends only on its own frames, and its page markers on which pages those are, which typing doesn't change. `delete_selection(false)` erases without composing when an insert, a tab, Return or a frame break follows, so each keystroke composes once. Undo, styles, page insert, reorder and setup stay on the full path.

**25. Fixed. The PDF was assembled twice in memory, and the dialog painted after the copies.**
`Writer` opens `path.saving` and writes each object as it is made; `finish` adds the xref and renames over the target, and a Writer that doesn't finish (failure, cancel) removes its file. The page tree's number is reserved before the pages, so `/Parent` is written once instead of patched. Each page's content string is freed once written, and a picture's samples go when `embed_picture` returns, so peak memory is about one picture plus the page content strings, not the whole file twice. `run_pdf_export` opens the dialog and returns to the loop; the document copy, snapshots and thread start (`start_pdf_job`) run two async turns later, after the dialog has painted. A Cancel before then just closes it, and the destructor pumps until an unstarted job is gone. Cancel still waits for the step in hand (one picture's encode, one face's subset).

**26. Half done. Idle redraw walks every visible page's geometry; one picture's new bytes dropped every texture.**
Textures: `ImageStore::Entry::version` bumps when that picture's bytes are replaced (`add` or `install` over loaded bytes), and `texture_for` remakes only a texture whose version differs. `generation()` now moves only when the whole store is replaced (new or opened publication). The asset window's thumbnail key includes the version.

Draw-list cache: not done, on purpose. Measured, `build_page` for a two-page spread is under 0.1 ms and for all 100 photobook pages about 1.4 ms, and the culling added for slow scrolling (pages outside the window aren't built) already removes most of that. The document is edited in place from many paths and there is no single change counter to key a cache on (`m_version` moves with undo steps, not every edit), so a missed invalidation would show a stale page. Revisit if a profile shows `build_page`; the safe shape is a generation bumped in one choke point that every edit already passes through.

## Abstractions

These are contracts the code already almost has. Writing them down, and closing the holes above, is the cleanup. Splitting `PagemadeApp` into more files is not. Menus, palettes, and document commands belong in the app; the two seams below are the ones that cross threads.

**ImageStore is the session's bytes. The UI thread is the only mutator. Other threads copy `shared_ptr`s under the mutex and do not keep `Entry *`.**
`find` and `cached_display` document this and then drop the lock before returning the pointer. `texture_for` uses them on the UI thread and uploads immediately, which is safe. `ImageStore::display` decodes on the caller and is only used from tests; the app path is `request_display` → `make_display` → `install` / `set_proxy`. Either delete the sync `display` from the app API or mark it test-only, so a later call from a worker does not mutate `proxy` while `texture_for` holds the pointer.

`VectorStore` is still a plain map. `scene()` and `find()` return pointers into it and stay on the thread that mutates it. The PDF job does not: it takes `snapshot()` on the UI thread (item 17). A mutex, matching `ImageStore`, is the leftover if some other worker starts calling `scene()`.

**BackgroundWorker is one ordered queue with coalescing, for jobs that post their own result to the UI thread.**
Keep that. The asset window's scan thread shows the other rule: a thread the owner waits for is joinable. `BackgroundWorker` detaches so a destructor never blocks on a network read. The matching cost is process teardown. A job that calls `nanogui::async` after the async mutex is destroyed is a race, and `stop()` never sets `running` back to false because `stopped` is sticky. Join in `stop()` for widget teardown (the in-flight job finishes; the queue is already dropped). A read that must not hang quit needs a timeout inside the read, which a detached thread does not provide.

The PDF job is the third shape: a single cancellable task with progress, a GL callback on the UI thread, and a join when it ends. It stays a private struct in `PagemadeApp`. It owns the snapshots from item 17 and the try/catch from item 18, and it does not borrow `m_images` or `m_vectors` for the run.

**Draw list in, backends out.**
`build_page` is the right cut for screen and PDF. The screen should consume a cached list (item 26). PDF consumes the list plus the snapshotted picture bytes and the retained font `shared_ptr`s. `picture_source` still copies bytes under the image lock, on the snapshot the job owns.

## Leave as they are

- Whole-document undo, 100 deep, with pixels outside the snapshot. A book-length story duplicates its text in the undo stack. That is the ceiling to revisit when stories get large, not a bug now.
- `read_file` uses `ftell`, which is 64-bit on this Linux. A Windows port needs an off_t-sized size before it opens multi-gigabyte files. Fonts on Windows are already listed under later work.
- Serial composition of one story is in decent shape after items 11 and 12. Item 24 is about which stories run, not about `fits` copying the line again.
- CMYK, spot colors, and real gradient stops are product gaps already listed under Later and known gaps, not defects in the RGB path. PDF/X-4 still writes tagged RGB when CMYK is requested, and preflight says so.

---

# Tool frames: tabs, tear-off, green drop highlight

**In the code. Not hand-checked in a window.** Tab groups are not written into the `.pagemade` file. Swatch dialogs are not part of this.

`ToolTabsWindow` (`pagemade/src/tool_tabs.h`) was already the dockable frame, and Image Assets was already the Images and Layouts tabs. This cut makes those tabs draggable and puts Layers and Type in the same kind of window. There is no separate `ToolFrame`. Modal dialogs (save, new, the export and print progress) stay modal. The paragraph editor stays the menu popup it is (`m_para_panel`). Image Assets keeps its own grid/details `Split`; that divider is not the frame split.

## NanoGUI

`TabWidgetBase::mouse_drag_event` reorders while the pointer stays on the strip. The old midpoint test (`p.x() - m_pos.y()`) is unchanged. Once the pointer has left the strip by 12px, the drag stays torn even if it comes back. The callback gets the tab id, the screen position (`p + parent()->absolute_position()`), and a `TabDragPhase`: `Move`, `Release`, or `Cancel`. `mouse_motion_event` only keeps the close-button hover. The in-strip ghost still draws inside the strip scissor, and only while the drag has not left. Off the strip, `Screen::draw_tab_drag_ghost` paints a green caption pill under the cursor after `nvgResetScissor`. The drag handler returns true, so `mainloop(-1)` repaints.

A left release is delivered back to the tab widget, including a release outside it. `Widget::consumes_external_drag_release` is true while a tab drag is active, and the screen then does not also click whatever is underneath. Escape calls `drag_cancel_event`, clears the drag, and sets `m_suppress_drag_release` so the later mouse-up is not a click. An ordinary window move does not take that path. This does not use `begin_drag` (`begin_drag` clears `m_drag_active`).

`Window::set_drop_highlight` is `None`, `Outline`, or `Band`. `Window::draw` strokes it after `Widget::draw`, scissor reset, so a cached child list cannot freeze it. The color is `Color(48, 168, 72, 230)`, 2px, at `m_window_corner_radius`. `Band` is the bottom third of the body under the title, with a fill of `Color(48, 168, 72, 70)` plus the stroke. The setter calls `propagate_cache_dirty()` when the value changes.

`TabWidget::transfer_tab` reparents the page and `insert_tab`s it. The destination assigns a new id. The close callback does not run. The source callback runs only when the removed tab was the active one and tabs remain. Both sides `perform_layout` and `propagate_cache_dirty`. Tool strips call `set_remove_children(false)`, so a later `remove_tab` cannot queue the page. `reparent` moves the child-list reference; it does not `inc_ref` or `dec_ref`.

`example1` (`CreateDraggableTabs`) opens "Tab group A" (Alpha, Beta) and "Tab group B" (Gamma, Delta). A tab merges, tears off onto empty space, and the target takes the green outline. The front window under the pointer wins, including one that has no draggable tabs. The band and the vertical split are PageMade only. The Misc window's "+" tabs stay non-draggable. The new methods are not in the Python bindings.

## PageMade

A `ToolPanel` is the page: `title`, `refresh`, `shown`, `floor_size`, `persist_on_close` (true, except Type), and `preferred_content_height` (−1, except Type and Layers). `preferred_size` returns the floor, so a scroll pane inside does not ratchet the window up to the screen.

`ToolTabsWindow` holds one `TabWidget`, or two when split. `nanogui::Split` with `Orientation::Vertical` puts the first child on top; the existing strip is reparented first, then the new one is made, and the drag position starts at 0.55. One level. An entry is `(panel, tab id, host)`. Each strip's ids start at 0, so `tab_changed` matches the host and the id, and a split cannot `shown()` the wrong page. `insert_tab` announces a strip's first tab before the caller has stored that id. `adopt` pushes a placeholder `{panel, -1, dest}`, stores the id `transfer_tab` returns, selects it, and calls `tab_changed` itself. `set_selected_id` does not fire the callback.

The drag hit-tests with `Screen::find_widget` and walks parents. The front widget wins, so a swatch, a popup, or the page does not pass the drop through onto a palette behind it. Another tool window draws an outline, or a band when it is not yet split and the pointer is in the bottom third of the body (the same line the stroke uses, from `absolute_position()`). Release on the outline inserts into the half under the pointer (`tab_index_at`, or the end when the pointer is on the body). Release on the band calls `ensure_split` and appends to the lower strip. A frame that is already split stays an outline and inserts into the half under the pointer. Release over the source leaves the tab where the reorder put it. Release anywhere else tears it into a new undocked window at the cursor, sized from `floor_size()`, then `fit_floating`. Highlights are cleared on every live window at the start of each move, then the target is set, before the next draw.

An empty half is unwrapped on the next `async` turn: the keeper is reparented onto the window and the `Split` (and the empty strip) is removed. An empty window calls `s_on_empty`. That retire undocks, drops the float and dock maps, nulls `m_assets` only when that pointer is the empty shell, and `async`-disposes. It does not null a panel that moved. The dispose is deferred because the callback runs inside the tab's mouse handler.

Close follows the panels. If any of them persists, the window hides, so a mix keeps Type constructed and keeps the image scan thread. A Type-only window saves `m_type_dock`, `m_type_dock_w`, and `m_type_float_size` from the window, nulls `m_type_panel`, and retires. Image Assets is made once, the first time, and only when `m_images_panel` is null; reopening finds the window that holds the panel, which may no longer be `m_assets` after a tear-off. Layers hides the same way. `show_panel` selects the tab and also catches the other half of a split up. `refresh` and `shown` cover both strips. `current()` is the upper strip's selection. `~PagemadeApp` clears the born and empty hooks, disposes every live tool window parented to the app, and `do_widget_cleanup()` before the image stores die. The scan joins in `~ImageAssetsPanel`.

`fit_floating` runs only when the window is undocked, not split, and holds exactly one panel whose `preferred_content_height` is at least 0. Images and Layouts return −1, so a merged or split window is not shrunk to Type's field height. A title double-click does not maximize (`m_last_title_click = 0`). A floating resize that changes the width refits. Dock maps key the `ToolTabsWindow`. Dragging the title still undocks the whole frame (`m_drag`). A tab drag returns true and does not undock. Docked windows are the visible live ones whose parent is the app, in the order they were created. The frame's minimum is the widest panel's floor, and at least 150 wide.

## Still open

Hand check, in a window: merge Type onto Image Assets, switch tabs, edit a field, dock the frame, tear Type off, drop Layers on the bottom third, close one tab, reopen each from the menu. The folder scan keeps running across a hide and a reparent, and joins when the panel is actually destroyed. A modal still takes the clicks, the motion, and the file drops. Compose tests do not open these windows. `example1` covers the widget merge, the tear-off, and the outline. `pagemade_app` and `example1` both build.


