/*
 * pagemade/default_fonts.h — fonts for the demo document.
 *
 * Families "Serif", "Sans" and "Display" map to the URW base35 clones of
 * the LaserWriter fonts (Palatino, Helvetica, ITC Avant Garde Gothic) when
 * they're installed, then to Liberation / DejaVu, then to the Roboto files
 * in nanogui's resources/ directory.
 */
#pragma once

#include "composer/font.h"

#include <string>

namespace pagemade {

/* Directory of the built-in Roboto faces. resources/ beside the executable
 * when that copy is present, otherwise the compile-time source tree. */
std::string bundled_font_dir();

/* Directory that contains hyphenation/. Same search order. */
std::string bundled_data_dir();

void register_default_fonts(FontLibrary &lib);

} // namespace pagemade
