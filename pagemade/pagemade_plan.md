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
  switches itself when the text tool is chosen). Font, size, weight
  (Regular / Bold / Italic / Bold Italic), leading (Auto, or a point size)
  and baseline shift. A text selection is restyled; a caret changes what
  is typed next; text blocks selected with the pointer restyle the type in
  those blocks; with nothing selected, the fields are the defaults for a
  new text block.
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

## Next, in order

1. **Images, with cropping**: placing PNG/JPEG makes an image item, linked or
   embedded through the file format's asset table. Cropping is a clip, so this
   step brings clipping to a path to nanovgd (stencil clip; PDF clips natively
   with `W n`).
2. **SVG import**: nanosvg makes a group of path items in the shape format. This
   step brings even-odd fill and per-subpath winding to nanovgd. The fill rule
   reaches the backend through the `renderFill` hook, which the Metal backend
   (`ext/nanovg_metal`) implements too, so Mac builds need the matching change.
   Multi-stop gradients come with it.
3. **Text wrap**: lines ask which horizontal spans are free at their height,
   from the wrap outlines of items in front of the text.
4. **Print dialog**: choose the printer, copies and page range. Linux first (the
   CUPS API with our own dialog, or GTK's), then Mac (NSPrintOperation on the
   PDF), then Windows. Today printing exports a PDF and hands it to `lp`.

## Later and known gaps

- Small text on screen: a nanovgd entry point that draws cached glyph bitmaps by
  glyph id at exact composed positions; cache drawn pages as display lists.
- Paragraph-at-a-time (Knuth-Plass) composer; right-to-left text.
- Text colors as swatches (character styles still carry RGB).
- Groups; master pages; spreads or a continuous page view (the view shows one
  page at a time).
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
