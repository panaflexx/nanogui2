/*
 * pagemade/system_fonts.h — installed faces for the font menu.
 *
 * Serif, Sans and Display stay the built-in families. Everything else the
 * system lists (through fc-list) is added beside them and loaded the first
 * time a story uses it.
 */
#pragma once

#include "composer/font.h"

namespace pagemade {

void register_system_fonts(FontLibrary &lib);

} // namespace pagemade
