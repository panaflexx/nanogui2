/*
 * pagemade/font_menu.cpp — see font_menu.h.
 */
#include "font_menu.h"

#include "page.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace pagemade {
namespace {

bool contains(const std::vector<std::string> &list, const std::string &name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

std::string trim(const std::string &line) {
    size_t begin = 0;
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t' ||
                                   line[begin] == '\r'))
        ++begin;
    size_t end = line.size();
    while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                           line[end - 1] == '\r' || line[end - 1] == '\n'))
        --end;
    return line.substr(begin, end - begin);
}

std::string fold_ascii(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = (char) (c - 'A' + 'a');
    return text;
}

bool show_family(const std::string &name, const std::vector<std::string> &installed,
                 const std::vector<std::string> &document, const std::string &current) {
    if (name.empty())
        return false;
    if (name == current || contains(document, name))
        return true;
    return contains(installed, name);
}

void append_utf8(std::string &text, unsigned codepoint) {
    if (codepoint < 0x80) {
        text.push_back((char) codepoint);
    } else if (codepoint < 0x800) {
        text.push_back((char) (0xC0 | (codepoint >> 6)));
        text.push_back((char) (0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        text.push_back((char) (0xE0 | (codepoint >> 12)));
        text.push_back((char) (0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back((char) (0x80 | (codepoint & 0x3F)));
    } else {
        text.push_back((char) (0xF0 | (codepoint >> 18)));
        text.push_back((char) (0x80 | ((codepoint >> 12) & 0x3F)));
        text.push_back((char) (0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back((char) (0x80 | (codepoint & 0x3F)));
    }
}

void pop_utf8(std::string &text) {
    if (text.empty())
        return;
    size_t i = text.size() - 1;
    while (i > 0 && ((unsigned char) text[i] & 0xC0) == 0x80)
        --i;
    text.erase(i);
}

const char *kEnglish[] = {
    "Serif", "Sans", "Display",
    "Liberation Serif", "Liberation Sans", "Liberation Mono",
    "DejaVu Serif", "DejaVu Sans", "DejaVu Sans Mono",
    "Nimbus Roman", "Nimbus Sans", "Nimbus Mono PS",
    "TeX Gyre Termes", "TeX Gyre Pagella", "TeX Gyre Heros", "TeX Gyre Cursor",
    "FreeSerif", "FreeSans", "FreeMono",
    "Noto Serif", "Noto Sans", "Noto Sans Mono", "Noto Mono",
    "Times New Roman", "Georgia", "Palatino Linotype", "Palatino",
    "Garamond", "Book Antiqua", "Cambria", "Constantia",
    "Arial", "Helvetica", "Verdana", "Tahoma", "Calibri", "Candara", "Corbel",
    "Trebuchet MS", "Segoe UI", "Gill Sans",
    "Courier New", "Courier", "Consolas", "Lucida Console",
    "Roboto", "Roboto Mono", "Roboto Serif",
    "Source Serif 4", "Source Sans 3", "Source Sans Pro", "Source Code Pro",
    "Ubuntu", "Ubuntu Mono", "Cantarell",
    "P052", "C059", "URW Gothic", "URW Bookman",
    "Linux Libertine O", "Linux Biolinum O",
    "Charter", "Bitstream Charter",
};

} // namespace

bool FontMenuModel::load(const std::string &path, const std::vector<std::string> &) {
    m_path = path;
    std::ifstream in(path);
    if (!in)
        return false;
    FontMenuList next;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const size_t space = line.find(' ');
        const std::string key = space == std::string::npos ? line : line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : trim(line.substr(space + 1));
        if (key == "recent_limit") {
            if (rest.empty())
                continue;
            char *end = nullptr;
            const long value = std::strtol(rest.c_str(), &end, 10);
            if (end != rest.c_str() && end && *end == '\0' && value >= 0 && value <= 100000)
                next.recent_limit = (int) value;
        } else if (key == "favorite") {
            if (!rest.empty() && !contains(next.favorites, rest))
                next.favorites.push_back(rest);
        } else if (key == "recent") {
            if (!rest.empty() && !contains(next.recents, rest))
                next.recents.push_back(rest);
        }
    }
    if ((int) next.recents.size() > next.recent_limit)
        next.recents.resize((size_t) next.recent_limit);
    m_list = std::move(next);
    return true;
}

bool FontMenuModel::save() const {
    if (m_path.empty())
        return false;
    try {
        const std::filesystem::path file(m_path);
        if (file.has_parent_path() && !file.parent_path().empty())
            std::filesystem::create_directories(file.parent_path());
        std::ofstream out(m_path, std::ios::trunc);
        if (!out)
            return false;
        out << "recent_limit " << m_list.recent_limit << "\n";
        for (const std::string &name : m_list.favorites)
            out << "favorite " << name << "\n";
        for (const std::string &name : m_list.recents)
            out << "recent " << name << "\n";
        return (bool) out;
    } catch (...) {
        return false;
    }
}

void FontMenuModel::note_use(const std::string &family) {
    if (family.empty())
        return;
    auto &recents = m_list.recents;
    recents.erase(std::remove(recents.begin(), recents.end(), family), recents.end());
    recents.insert(recents.begin(), family);
    if (m_list.recent_limit < 0)
        m_list.recent_limit = 0;
    if ((int) recents.size() > m_list.recent_limit)
        recents.resize((size_t) m_list.recent_limit);
    save();
    if (m_listener)
        m_listener();
}

void FontMenuModel::toggle_star(const std::string &family) {
    if (family.empty())
        return;
    auto &favorites = m_list.favorites;
    auto it = std::find(favorites.begin(), favorites.end(), family);
    if (it != favorites.end())
        favorites.erase(it);
    else
        favorites.push_back(family);
    save();
    if (m_listener)
        m_listener();
}

bool FontMenuModel::starred(const std::string &family) const {
    return contains(m_list.favorites, family);
}

void FontMenuModel::set_listener(std::function<void()> listener) {
    m_listener = std::move(listener);
}

std::vector<FontMenuRow> FontMenuModel::rows(const std::vector<std::string> &installed,
                                            const std::vector<std::string> &document,
                                            const std::string &current,
                                            const std::string &query) const {
    std::vector<FontMenuRow> out;
    if (!query.empty()) {
        FontMenuRow heading;
        heading.heading = true;
        heading.text = "Search: " + query;
        out.push_back(heading);
        const std::string folded = fold_ascii(query);
        std::vector<std::string> seen;
        auto consider = [&](const std::string &name) {
            if (!show_family(name, installed, document, current) || contains(seen, name))
                return;
            if (fold_ascii(name).find(folded) == std::string::npos)
                return;
            seen.push_back(name);
            FontMenuRow row;
            row.text = name;
            row.starred = contains(m_list.favorites, name);
            out.push_back(row);
        };
        for (const std::string &name : installed)
            consider(name);
        for (const std::string &name : document)
            consider(name);
        consider(current);
        if (seen.empty()) {
            FontMenuRow none;
            none.heading = true;
            none.text = "No matching fonts";
            out.push_back(none);
        }
        return out;
    }

    std::vector<std::string> seen;
    auto add_family = [&](const std::string &name) {
        if (!show_family(name, installed, document, current) || contains(seen, name))
            return;
        seen.push_back(name);
        FontMenuRow row;
        row.text = name;
        row.starred = contains(m_list.favorites, name);
        out.push_back(row);
    };
    auto add_section = [&](const char *title, const std::vector<std::string> &names) {
        std::vector<std::string> shown;
        for (const std::string &name : names)
            if (show_family(name, installed, document, current) && !contains(seen, name) &&
                !contains(shown, name))
                shown.push_back(name);
        if (shown.empty())
            return;
        FontMenuRow heading;
        heading.heading = true;
        heading.text = title;
        out.push_back(heading);
        for (const std::string &name : shown)
            add_family(name);
    };

    const bool in_document = contains(document, current);
    const bool in_recent = contains(m_list.recents, current);
    const bool in_favorites = contains(m_list.favorites, current);
    if (!current.empty() && !in_document && !in_recent && !in_favorites)
        add_family(current);
    add_section("In this publication", document);
    add_section("Recent", m_list.recents);
    add_section("Favorites", m_list.favorites);
    return out;
}

FontMenuList font_menu_defaults(const std::vector<std::string> &installed) {
    FontMenuList list;
    for (const char *name : kEnglish)
        if (contains(installed, name))
            list.favorites.push_back(name);
    return list;
}

void font_menu_load_or_seed(FontMenuModel &model, const std::string &path,
                            const std::vector<std::string> &installed) {
    if (!model.load(path, installed)) {
        model.set_list(font_menu_defaults(installed));
        model.save();
    }
}

std::string font_menu_config_path() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
        if (xdg[0])
            return std::string(xdg) + "/pagemade/fonts.conf";
    if (const char *home = std::getenv("HOME"))
        if (home[0])
            return std::string(home) + "/.config/pagemade/fonts.conf";
    return "pagemade-fonts.conf";
}

std::vector<std::string> document_font_families(const PageDoc &doc) {
    std::vector<std::string> out;
    for (const StoryEntry &entry : doc.stories)
        for (const Paragraph &paragraph : entry.story.paragraphs)
            for (const Run &run : paragraph.runs) {
                if (run.style.family.empty() || contains(out, run.style.family))
                    continue;
                out.push_back(run.style.family);
            }
    return out;
}

bool font_menu_query_edit(std::string &query, unsigned codepoint) {
    if (codepoint == 0x1B) {
        if (query.empty())
            return false;
        query.clear();
        return true;
    }
    if (codepoint == 0) {
        pop_utf8(query);
        return true;
    }
    if (codepoint < 32)
        return false;
    append_utf8(query, codepoint);
    return true;
}

} // namespace pagemade
