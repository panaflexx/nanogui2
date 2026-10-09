/*
 * pagemade/font_menu_ui.cpp — see font_menu_ui.h.
 */
#include "font_menu_ui.h"

#include <nanogui/menu.h>
#include <nanogui/screen.h>

using namespace nanogui;

namespace pagemade {

void refill_font_menu(Dropdown *menu, const FontMenuView &view,
                      const std::function<void(const std::string &)> &pick,
                      const std::function<void(const std::string &)> &star) {
    if (!menu || !menu->popup() || !view.model)
        return;
    const std::string caption = menu->caption();
    const std::vector<FontMenuRow> rows =
        view.model->rows(view.installed, view.document, view.current, view.query);
    menu->clear_items();
    for (const FontMenuRow &row : rows) {
        if (row.heading) {
            MenuItem *heading = menu->add_item(row.text);
            heading->set_flags(Button::NormalButton);
            heading->set_enabled(false);
            continue;
        }
        const std::string family = row.text;
        MenuItem *item = menu->add_item(std::make_pair(family, std::string()), 0,
                                        [pick, family] {
                                            if (pick)
                                                pick(family);
                                        },
                                        {{0, 0}}, true);
        if (star)
            item->set_mark(row.starred, [star, family] { star(family); });
    }

    const bool searching = !view.query.empty();
    if (searching) {
        menu->set_caption(caption);
    } else if (view.mixed) {
        menu->set_selected_index(-1);
        menu->set_caption("\u2014");
    } else {
        int index = -1;
        for (int i = 0; i < menu->popup()->child_count(); ++i) {
            MenuItem *item = menu->popup()->item(i);
            if (item && item->enabled() && item->caption() == view.current) {
                index = i;
                break;
            }
        }
        if (index >= 0)
            menu->set_selected_index(index);
        else {
            menu->set_selected_index(-1);
            menu->set_caption(view.current.empty() ? std::string("Serif") : view.current);
        }
    }

    if (!menu->popup()->visible())
        return;
    if (Screen *screen = menu->screen()) {
        if (NVGcontext *nvg = screen->nvg_context())
            menu->popup()->perform_layout(nvg);
    }
    if (searching) {
        int first = -1;
        for (int i = 0; i < menu->popup()->child_count(); ++i) {
            MenuItem *item = menu->popup()->item(i);
            if (item && item->enabled()) {
                first = i;
                break;
            }
        }
        menu->popup()->set_highlighted_index(first);
        if (first >= 0)
            menu->popup()->reveal(first);
    } else {
        const int index = menu->selected_index();
        if (index >= 0)
            menu->popup()->reveal(index);
    }
    if (Screen *screen = menu->screen()) {
        if (NVGcontext *nvg = screen->nvg_context())
            menu->popup()->perform_layout(nvg);
    }
    menu->popup()->request_focus();
}

} // namespace pagemade
