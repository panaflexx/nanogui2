/*
 * pagemade/swatch_library.h — the shared Colors palette.
 *
 * Like the font menu's favorites, the user's custom swatches live in a
 * line-oriented config file (colors.conf beside fonts.conf). The first
 * time that file is missing it is seeded with the starter set. Built-in
 * swatches ([Paper], [Black], [Registration]) are always present and
 * locked: they are not stored in the file and cannot be edited or
 * deleted. CMYK is the truth; RGB is derived for the screen.
 */
#pragma once

#include "page.h"

#include <functional>
#include <string>
#include <vector>

namespace pagemade {

struct SwatchLibrary {
    std::vector<Swatch> custom;   // user swatches only, file order
};

/* False when `path` does not exist yet. An empty file loads: a user who
 * deleted every custom color. Built-ins are never in the file. */
bool swatch_library_load(SwatchLibrary &library, const std::string &path);
bool swatch_library_save(const SwatchLibrary &library, const std::string &path);

/* Missing file: seed the starter set and save. An existing file is left
 * as it is, even when empty. */
void swatch_library_load_or_seed(SwatchLibrary &library, const std::string &path);

/* $XDG_CONFIG_HOME/pagemade/colors.conf, else ~/.config/pagemade/colors.conf. */
std::string swatch_library_config_path();

/* Built-ins followed by the library's custom swatches, ids intact. */
std::vector<Swatch> swatch_library_swatches(const SwatchLibrary &library);

/* Copy the library into a document's table: swatches whose name the
 * document already has are skipped (the document wins), the rest are
 * appended, remapping ids that the document already uses. References
 * already in the document keep resolving to the same colors. */
void swatch_library_seed(PageDoc &doc, const SwatchLibrary &library);

/* A custom swatch with this name, or nullptr. Built-ins are never renamed
 * through the library. */
const Swatch *swatch_library_find(const SwatchLibrary &library, const std::string &name);

/* Add (ids above everything in `doc_swatches`), rename, recolor or
 * remove a custom swatch. Built-in ids are refused. Each saves the file
 * and returns false when nothing changed. */
bool swatch_library_add(SwatchLibrary &library, const std::string &path,
                        const std::vector<Swatch> &doc_swatches,
                        const std::string &name, const Cmyk &cmyk, SwatchId *id = nullptr);
bool swatch_library_set(SwatchLibrary &library, const std::string &path, SwatchId id,
                        const std::string &name, const Cmyk &cmyk);
/* Add the name, or recolor it when it is already there. For the globe
 * button: a document color joins the global palette. */
bool swatch_library_upsert(SwatchLibrary &library, const std::string &path,
                           const std::vector<Swatch> &doc_swatches,
                           const std::string &name, const Cmyk &cmyk);
bool swatch_library_remove(SwatchLibrary &library, const std::string &path, SwatchId id);

} // namespace pagemade
