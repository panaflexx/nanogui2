/*
 * pagemade/svg_export.h — items as an SVG document (Copy to other apps).
 *
 * The selection keeps its place and size: the SVG is measured in points
 * (width and height in pt) and its viewBox is the items' bounds in page
 * coordinates. Pagemade groups become <g> groups.
 *
 *   Shapes, rules, borders and placed graphics: paths, from the same draw
 *   list the screen and PDF use, so line styles, dashes, clips and
 *   gradients come out as drawn.
 *   Pictures: an embedded JPEG (PNG when there is transparency), clipped to
 *   the frame. The file goes in as it is when it is already sRGB, upright
 *   and not much larger than 300 ppi as placed; otherwise it is decoded,
 *   converted to sRGB, turned upright and sized to 300 ppi. Empty frames
 *   draw only their border.
 *   Text: editable <text>, one element per composed line, so line breaks
 *   stay where pagemade put them. Each style change is a <tspan> at its
 *   pen position; justified lines position each word. Fonts are named by
 *   the family in the font file itself ("P052", "Nimbus Sans"), not
 *   pagemade's Serif / Sans aliases, with a generic fallback. Tracking is
 *   letter-spacing; all caps are written as capitals; bullets, numbers,
 *   tab leaders and break hyphens are text.
 */
#pragma once

#include "page.h"

#include <string>
#include <vector>

namespace pagemade {

class ImageStore;
class VectorStore;

/* `ids` on `page`; others are skipped. `comps` holds one composition per
 * story, in PageDoc::stories order. Empty when nothing visible is chosen. */
std::string items_svg(const PageDoc &doc, size_t page, const std::vector<ItemId> &ids,
                      const std::vector<Composition> &comps, const ImageStore *images,
                      const VectorStore *vectors);

/* The text the chosen text blocks show, in stacking order, for a plain-text
 * paste: paragraphs end in a newline, line breaks inside one are newlines,
 * tabs stay, list markers lead their paragraphs, page markers are the
 * numbers they show, discretionary hyphens are dropped. Only what is
 * composed into those blocks (not overset text). Empty when no text block
 * is chosen. */
std::string items_text(const PageDoc &doc, size_t page, const std::vector<ItemId> &ids,
                       const std::vector<Composition> &comps);

} // namespace pagemade
