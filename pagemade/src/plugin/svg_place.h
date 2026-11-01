/*
 * pagemade/plugin/svg_place.h — placing an SVG (or any vector file the
 * loader reads) on a page: File > Place, a drop, or a paste.
 *
 * A graphic goes down as one frame, or, when it embeds pictures, as
 * pieces: each picture a picture frame and the drawing around it a graphic,
 * grouped, so Ungroup separates the photo. Paste as Text instead rebuilds
 * the SVG's text as editable text frames. The pieces are stored as SVG
 * slices of the original, so a saved publication reopens them through the
 * same decoder.
 */
#pragma once

#include "vector.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pagemade {

class ImageStore;
class FontLibrary;

/* Decode an SVG pasted from another program: as the SVG decoder does,
 * except that a root width and height without a unit (or in px) are
 * points, the way Illustrator and its kin (VectorCraft) write them. */
bool decode_svg_paste(const uint8_t *data, size_t n, VectorScene *out, std::string *error);

/* Drawing keeps the file as graphics. Text turns each SVG text line into
 * an editable text frame, each picture into a picture, and the paths into
 * a graphic of their own. */
enum class SvgPaste { Drawing, Text };

struct VectorPlacement {
    bool ok = false;
    std::string error;
    std::string note;
    uint32_t asset = 0;                  // the first graphic; 0 when there is none
    ItemId item = 0;                     // one placed piece (a member of `group`)
    std::vector<ItemId> items;           // every piece, back to front
    uint32_t group = 0;                  // set when two or more pieces were placed
};

/* One graphic on `page`, or a group when the file embeds pictures and
 * `images` is set. Each embedded picture is placed on its own, and the
 * drawing before and after it stays a graphic, so Ungroup separates the
 * photo. `SvgPaste::Text` instead makes a text frame for each SVG text
 * line (the characters, editable) and a graphic for the paths, sized to
 * those paths. Without `images`, or with no picture this slice can
 * separate, a drawing paste is one graphic. The caller records undo.
 * `source` is the file: URI (empty when pasted); a group stores each
 * piece itself and does not keep that link. `fonts` measures Paste as Text.
 * `paste`: the bytes came from the clipboard (decode_svg_paste's units, and
 * fitted to the live area rather than a column). */
VectorPlacement place_svg_bytes(PageDoc &doc, VectorStore &store, std::vector<uint8_t> bytes,
                                const std::string &name, const std::string &source,
                                size_t page, const Point *at, ImageStore *images = nullptr,
                                SvgPaste how = SvgPaste::Drawing,
                                const FontLibrary *fonts = nullptr, bool paste = false);

} // namespace pagemade
