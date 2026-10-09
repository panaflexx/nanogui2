/*
 * pagemade/font_menu_ui.h — fill a Dropdown from a FontMenuModel.
 * The compose test does not link this; it checks the model directly.
 */
#pragma once

#include "font_menu.h"

#include <functional>
#include <string>
#include <vector>

namespace nanogui {
class Dropdown;
}

namespace pagemade {

struct FontMenuView {
    FontMenuModel *model = nullptr;
    std::vector<std::string> installed;
    std::vector<std::string> document;
    std::string query;
    std::string current;
    bool mixed = false;
};

/* Headings are disabled rows. Family rows call `pick` when chosen and
 * `star` when the star is clicked. A search leaves the closed caption
 * alone; an empty query selects `current`, or an em dash when `mixed`. */
void refill_font_menu(nanogui::Dropdown *menu, const FontMenuView &view,
                      const std::function<void(const std::string &)> &pick,
                      const std::function<void(const std::string &)> &star);

} // namespace pagemade
