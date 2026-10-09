/*
 * pagemade/font_menu.h — which families the font menu shows.
 *
 * The menu stays short: families used in the publication, then recently
 * used faces, then starred favorites. Typing searches the whole catalog.
 * Favorites, recents and how many recents to keep live in a line-oriented
 * config file. The first time that file is missing, favorites are the
 * common English families that are actually installed.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace pagemade {

struct PageDoc;

struct FontMenuList {
    std::vector<std::string> favorites; // starred, in the order they were pinned
    std::vector<std::string> recents;   // newest first
    int recent_limit = 8;
};

struct FontMenuRow {
    std::string text;
    bool starred = false;
    bool heading = false; // not a family; the row cannot be chosen
};

class FontMenuModel {
public:
    /* False when `path` does not exist yet. A file that lists no favorites
     * still loads: that is a user who unstarred everything. Names that are
     * not installed stay in the file. */
    bool load(const std::string &path, const std::vector<std::string> &installed);
    bool save() const;

    void note_use(const std::string &family);
    void toggle_star(const std::string &family);
    bool starred(const std::string &family) const;

    /* Called after note_use and toggle_star, once the file is saved.
     * The app schedules a menu refill from here; the model does not. */
    void set_listener(std::function<void()> listener);

    /* `installed` is catalog order. `document` is story order. An empty
     * `query` is the short list; anything else is a case-insensitive
     * substring search of the whole catalog. */
    std::vector<FontMenuRow> rows(const std::vector<std::string> &installed,
                                  const std::vector<std::string> &document,
                                  const std::string &current,
                                  const std::string &query) const;

    const FontMenuList &list() const { return m_list; }
    void set_list(FontMenuList list) { m_list = std::move(list); }
    const std::string &path() const { return m_path; }

private:
    FontMenuList m_list;
    std::string m_path;
    std::function<void()> m_listener;
};

/* Serif, Sans, Display and the ordinary Latin families from `installed`,
 * in a fixed English order. Noto script families are not in this set. */
FontMenuList font_menu_defaults(const std::vector<std::string> &installed);

/* Missing file: seed defaults and save. An existing file is left as it is. */
void font_menu_load_or_seed(FontMenuModel &model, const std::string &path,
                            const std::vector<std::string> &installed);

/* $XDG_CONFIG_HOME/pagemade/fonts.conf, else ~/.config/pagemade/fonts.conf. */
std::string font_menu_config_path();

/* Unique CharStyle::family values in story order. */
std::vector<std::string> document_font_families(const PageDoc &doc);

/* One keystroke of the font-menu search. `codepoint` 0 is Backspace,
 * 0x1B is Escape. Returns false when Escape should close the menu. */
bool font_menu_query_edit(std::string &query, unsigned codepoint);

} // namespace pagemade
