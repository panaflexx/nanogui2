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
 * (TrueType), with a ToUnicode map for search and copy. Colors are RGB:
 * device RGB, or tagged sRGB for PDF/X-4 (with its output intent, trim and
 * bleed boxes and metadata). An alpha below 1 selects an ExtGState (/ca and
 * /CA). Bleed and crop marks widen the sheet past the trim. CMYK and spot
 * colors come later.
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

struct PdfSettings;
class VectorStore;

/* `comps` holds one composition per story, in PageDoc::stories order.
 * `images` supplies picture sources; null draws a stand-in for each one.
 * `settings` (null: the document's own) choose the pages, picture
 * resolution and compression, color, PDF/X-4, bleed and crop marks, and
 * the title and author. `vectors` supplies placed SVG scenes; null draws
 * those frames as nothing but still draws their borders. False when the
 * file can't be written or a font fails to subset. */
bool export_pdf(const std::string &path, const PageDoc &doc,
                const std::vector<Composition> &comps,
                const ImageStore *images = nullptr,
                const PdfSettings *settings = nullptr,
                const VectorStore *vectors = nullptr);

/* What may print badly with these settings, one sentence each: pictures
 * below the minimum resolution as placed, missing pictures, transparency,
 * overset text, hidden layers, an odd page count. Empty when all is well. */
std::vector<std::string> pdf_preflight(const PageDoc &doc, const std::vector<Composition> &comps,
                                       const ImageStore *images, const PdfSettings &settings);

/* Profiles an output intent can name: the built-in sRGB (empty path)
 * first, then the CMYK printing profiles installed on this computer
 * ("Coated FOGRA39 (FOGRA39)"). */
struct PdfOutputProfile {
    std::string path;            // empty: built-in sRGB
    std::string name;            // for a menu
    std::string condition;       // the registry name ("FOGRA39")
    bool cmyk = false;
};
std::vector<PdfOutputProfile> pdf_output_profiles();

} // namespace pagemade
