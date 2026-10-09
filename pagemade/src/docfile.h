/*
 * pagemade/docfile.h — the native file format.
 *
 * A publication is a zip package:
 *   mimetype          "application/vnd.pagemade+zip", first and uncompressed,
 *                     so the file can be recognized without unzipping it
 *   document.json     setup, swatches, assets, stories, pages and items
 *   assets/fonts/...  font programs, when assets are embedded
 *
 * document.json lists the assets the publication uses (fonts now; images and
 * SVGs later) by URI (uri.h): an embedded copy is a reference relative to the
 * package root ("assets/fonts/P052-Roman.otf"), a linked one an absolute
 * file: URI. Each entry also keeps "source", the URI the asset came from, so
 * it can be relinked or refreshed. A font loaded from another publication has
 * no file to link to, so it is always embedded.
 *
 * Styles and items write only what differs from the defaults, and reading
 * starts from the defaults, so files from older and newer versions open.
 * Opening also repairs what it can (dangling ids, frames missing from a
 * thread) rather than refusing the file.
 *
 * Saving writes a temporary file next to the target and renames it over the
 * target, so a failed save never damages the previous version.
 */
#pragma once

#include "page.h"

#include <memory>
#include <string>
#include <vector>

namespace pagemade {

constexpr const char *kPublicationExtension = "pagemade";
constexpr const char *kPublicationMimeType = "application/vnd.pagemade+zip";
constexpr int kFormatVersion = 1;

struct SaveOptions {
    bool embed_assets = false;       // copy fonts (later images, SVGs) into the file
};

/* A face the publication asked for, found when it was opened. */
struct DocumentFont {
    std::string family, style;
    std::shared_ptr<Font> font;
    bool embedded = false;           // from the package (else a linked file)
};

struct OpenResult {
    bool ok = false;
    std::string error;
    PageDoc doc;
    bool embed_assets = false;       // how the file was saved
    /* Faces to install as document fonts (FontLibrary::add_document_font). */
    std::vector<DocumentFont> fonts;
    /* "Family Style" for faces neither embedded nor installed; the composer
     * falls back to another face. */
    std::vector<std::string> missing_fonts;
    std::vector<std::string> warnings;
};

bool save_document(const PageDoc &doc, const FontLibrary &fonts, const std::string &path,
                   const SaveOptions &opts, std::string *error = nullptr);
/* `installed` decides which linked fonts are needed: a family already
 * installed is used as is. */
OpenResult open_document(const std::string &path, const FontLibrary &installed);

/* The document.json text for a document (tests and debugging). */
std::string document_json(const PageDoc &doc, const FontLibrary &fonts, const SaveOptions &opts);

} // namespace pagemade
