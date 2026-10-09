/*
 * pagemade/pdf.h — the draw list's second backend: a PDF document.
 *
 * export_pdf() writes every page of the document at its trim size in
 * points, with fills, strokes and text drawn by the same positioned glyph
 * runs the screen shows. Text stays real text: each used face is subset
 * with hb-subset (retaining glyph ids, so character codes are glyph ids
 * under Identity-H) and embedded as CIDFontType0 (CFF) or CIDFontType2
 * (TrueType), with a ToUnicode map for search and copy. Colors are device
 * RGB for now; CMYK/spot and trim/bleed/marks come later.
 */
#pragma once

#include <string>
#include <vector>

namespace pagemade {

struct Composition;
struct PageDoc;

/* `comps` holds one composition per story, in PageDoc::stories order.
 * False when the file can't be written or a font fails to subset. */
bool export_pdf(const std::string &path, const PageDoc &doc,
                const std::vector<Composition> &comps);

} // namespace pagemade
