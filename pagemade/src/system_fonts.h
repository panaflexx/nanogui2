/*
 * pagemade/system_fonts.h — installed faces for the font menu.
 *
 * Serif, Sans and Display stay the built-in families. Everything else the
 * system lists is added beside them and loaded the first time a story uses
 * it. Linux asks fc-list. macOS reads the name tables under the system
 * font folders, so a bundled app does not need fontconfig on PATH.
 */
#pragma once

#include "composer/font.h"

namespace pagemade {

void register_system_fonts(FontLibrary &lib);

} // namespace pagemade
