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
 * frame. The screen's display view is not what gets written. A linked
 * picture the store has unloaded is read from its file for the export.
 *
 * With PdfSettings::flatten, a page that draws anything see-through is
 * rendered by the caller (PdfExportHooks::rasterize) and written as one
 * opaque JPEG at flatten_ppi; the other pages stay vector.
 */
#pragma once

#include "drawlist.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pagemade {

struct Composition;
struct PageDoc;
class ImageStore;

struct PdfSettings;
class VectorStore;

/* A picture for rasterizing a flattened page: upright, straight-alpha RGBA,
 * at the resolution the page is rendered. */
struct PdfRasterPicture {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

/* Hooks for a long export. Both are optional and are called on the thread
 * running export_pdf. */
struct PdfExportHooks {
    /* Between steps: how far along (0 to 1) and what is being done. Return
     * false to stop; export_pdf then returns false and writes no file. */
    std::function<bool(float done, const std::string &what)> progress;
    /* Flatten: render `list` (one page, page points, y down) over the
     * rectangle at (x, y), w_pt by h_pt, into `rgba`: width x height pixels,
     * top row first, opaque over white. `pictures` holds the pixels for
     * each DrawImage's asset. False writes the page unflattened. */
    std::function<bool(const DrawList &list, const std::map<uint32_t, PdfRasterPicture> &pictures,
                       float x, float y, float w_pt, float h_pt, int width, int height,
                       std::vector<uint8_t> &rgba)>
        rasterize;
};

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
                const VectorStore *vectors = nullptr,
                const PdfExportHooks *hooks = nullptr);

/* Does this page's draw list paint anything see-through: a translucent
 * color, gradient stop or picture opacity, or a picture with an alpha
 * channel? `doc` supplies the pictures' metadata. */
bool draws_transparency(const DrawList &list, const PageDoc &doc);

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
