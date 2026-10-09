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

## Next, in order

1. **NanoVG rendering fixes**: per-subpath winding and even-odd fill, clipping to
   a path (cropping, non-rectangular frames). Multi-stop gradients wait for SVG
   import. (Dashed strokes are done: `render_nvg` cuts dashes on the CPU.)
2. **PDF export**: the second consumer of the draw list. Fonts subset with
   hb-subset, text stays real text, CMYK/spot later, trim/bleed and marks.
3. **Images and SVG**: placing PNG/JPEG makes an image item, cropping is a clip;
   SVG through nanosvg becomes a group of path items in the shape format.
4. **Text wrap**: lines ask which horizontal spans are free at their height,
   from the wrap outlines of items in front of the text.

## Later and known gaps

- Small text on screen: a nanovgd entry point that draws cached glyph bitmaps by
  glyph id at exact composed positions; cache drawn pages as display lists.
- Paragraph-at-a-time (Knuth-Plass) composer; right-to-left text.
- Text colors as swatches (character styles still carry RGB).
- Groups; multiple pages in the view; master pages and spreads.
- Native file format (zip of JSON plus assets, via miniz).
- Import: Markdown/HTML (gumbo), RTF, DOCX; old PageMaker files via libpagemaker.
- Story editor; Styles, Colors and full Control palettes.
- Fonts on macOS and Windows (only Linux paths, then Roboto from `resources/`).
- Status bar doesn't refresh after a pinch zoom.
- Resizing a multiple selection as one group (each item resizes on its own today).
- Ruler guides (dragged from rulers) to snap to; rulers themselves.
- Adding and editing swatches (the Colors palette); the palette's swatch menus
  are built when a document is loaded.
- Fills and strokes for text blocks (PageMaker 7 frames).
