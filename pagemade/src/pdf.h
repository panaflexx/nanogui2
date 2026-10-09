/*
 * pagemade/pdf.h — the draw list's second backend: a PDF document.
 *
 * export_pdf() writes every page of the document at its trim size in
 * points, with fills, strokes and text drawn by the same positioned glyph
 * runs the screen shows. Hidden pages are left out (a document whose every
 * page is hidden still writes one blank page). Text stays real text: each
 * used face is subset
 * with hb-subset (retaining glyph ids, so character codes are glyph ids
 * under Identity-H) and embedded as CIDFontType0 (CFF) or CIDFontType2
 * (TrueType), with a ToUnicode map for search and copy. Colors are device
 * RGB for now; CMYK/spot and trim/bleed/marks come later.
 * Pictures are embedded from the source file in `images` (a JPEG's own
 * DCT stream, anything else the full-resolution pixels), clipped to the
 * frame. The screen's display view is not what gets written.
 */
#pragma once

#include <string>
#include <vector>

namespace pagemade {

struct Composition;
struct PageDoc;
class ImageStore;

/* `comps` holds one composition per story, in PageDoc::stories order.
 * `images` supplies picture sources; null draws a stand-in for each one.
 * False when the file can't be written or a font fails to subset. */
bool export_pdf(const std::string &path, const PageDoc &doc,
                const std::vector<Composition> &comps,
                const ImageStore *images = nullptr);

} // namespace pagemade
