/*
 * pagemade/default_fonts.h — fonts for the demo document.
 *
 * Families "Serif", "Sans" and "Display" map to the URW base35 clones of
 * the LaserWriter fonts (Palatino, Helvetica, ITC Avant Garde Gothic) when
 * they're installed, then to Liberation / DejaVu, then to the Roboto files
 * in nanogui's resources/ directory.
 *
 * On macOS the app is a bundle. resource_dir() and data_dir() prefer the
 * copies inside Contents/Resources, and prepare_app_bundle() points the
 * process at that folder before NanoGUI loads "resources/*.ttf".
 */
#pragma once

#include "composer/font.h"

#include <string>

namespace pagemade {

/* Roboto and the icon font. A .app uses Contents/Resources/resources. */
std::string resource_dir();
/* Hyphenation patterns. A .app uses Contents/Resources/pagemade/resources. */
std::string data_dir();
/* Finder starts a bundle with "/" as the working directory. Move to
 * Contents/Resources, and point libheif at the codecs shipped beside the
 * executable. No-op when this process is not a .app. */
void prepare_app_bundle();

void register_default_fonts(FontLibrary &lib);

} // namespace pagemade
