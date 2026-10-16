/*
 * pagemade/swatch_library.cpp — see swatch_library.h.
 */
#include "swatch_library.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <nanogui/fscompat.h>

namespace pagemade {
namespace {

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

/* Split a line into whitespace-separated words, honoring "quotes". */
std::vector<std::string> split_words(const std::string &line) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= line.size())
            break;
        std::string word;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"')
                word += line[i++];
            if (i < line.size())
                ++i;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t')
                word += line[i++];
        }
        words.push_back(word);
    }
    return words;
}

bool parse_percent(const std::string &word, float &out) {
    if (word.empty())
        return false;
    char *end = nullptr;
    const float value = std::strtof(word.c_str(), &end);
    if (end == word.c_str() || !end || *end != '\0')
        return false;
    out = std::clamp(value, 0.f, 100.f);
    return true;
}

bool is_builtin(SwatchId id) {
    return id == kPaper || id == kBlack || id == kRegistration;
}

/* The next free id above the built-ins, the library and the open document:
 * library and document ids share one space, so a swatch added while a
 * document is open never collides with one it already holds. */
SwatchId next_id(const SwatchLibrary &library, const std::vector<Swatch> &doc_swatches) {
    SwatchId id = kRegistration;
    for (const Swatch &s : library.custom)
        id = std::max(id, s.id);
    for (const Swatch &s : doc_swatches)
        id = std::max(id, s.id);
    return id + 1;
}

/* The PageMaker-ish starter set from the mockup: pure process inks,
 * then a few tints. */
std::vector<Swatch> starter_set() {
    struct Seed { const char *name; float c, m, y, k; };
    static const Seed seeds[] = {
        {"Cyan", 100, 0, 0, 0},     {"Magenta", 0, 100, 0, 0},
        {"Yellow", 0, 0, 100, 0},   {"Red", 0, 100, 100, 0},
        {"Green", 100, 0, 100, 0},  {"Blue", 100, 100, 0, 0},
        {"Orange", 0, 50, 100, 0},  {"Purple", 60, 90, 0, 0},
        {"Teal", 100, 0, 40, 0},    {"Brown", 0, 60, 100, 30},
        {"Gray 50", 0, 0, 0, 50},   {"Slate", 60, 40, 20, 40},
    };
    std::vector<Swatch> out;
    SwatchId id = kRegistration;
    for (const Seed &seed : seeds) {
        Swatch s;
        s.id = ++id;
        s.name = seed.name;
        s.cmyk = {seed.c, seed.m, seed.y, seed.k};
        s.rgb = cmyk_to_rgb(s.cmyk);
        out.push_back(s);
    }
    return out;
}

} // namespace

bool swatch_library_load(SwatchLibrary &library, const std::string &path) {
    std::ifstream in(path);
    if (!in)
        return false;
    SwatchLibrary next;
    SwatchId id = kRegistration;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> words = split_words(line);
        if (words.size() != 6 || words[0] != "color")
            continue;
        float c, m, y, k;
        if (!parse_percent(words[2], c) || !parse_percent(words[3], m) ||
            !parse_percent(words[4], y) || !parse_percent(words[5], k))
            continue;
        if (words[1].empty())
            continue;
        bool duplicate = false;
        for (const Swatch &s : next.custom)
            duplicate |= s.name == words[1];
        if (duplicate)
            continue;
        Swatch s;
        s.id = ++id;
        s.name = words[1];
        s.cmyk = {c, m, y, k};
        s.rgb = cmyk_to_rgb(s.cmyk);
        next.custom.push_back(s);
    }
    library = std::move(next);
    return true;
}

bool swatch_library_save(const SwatchLibrary &library, const std::string &path) {
    if (path.empty())
        return false;
    try {
        const std::string parent = nanogui::fs::parent_path(path);
        if (!parent.empty())
            nanogui::fs::create_directories(parent);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return false;
        out << "# pagemade colors. One per line: color \"Name\" C M Y K (0-100).\n";
        char buf[128];
        for (const Swatch &s : library.custom) {
            if (is_builtin(s.id))
                continue;
            std::snprintf(buf, sizeof buf, "color \"%s\" %g %g %g %g\n", s.name.c_str(),
                          s.cmyk.c, s.cmyk.m, s.cmyk.y, s.cmyk.k);
            out << buf;
        }
        return (bool) out;
    } catch (...) {
        return false;
    }
}

void swatch_library_load_or_seed(SwatchLibrary &library, const std::string &path) {
    SwatchLibrary loaded;
    if (swatch_library_load(loaded, path)) {
        library = std::move(loaded);
        return;
    }
    library.custom = starter_set();
    swatch_library_save(library, path);
}

std::string swatch_library_config_path() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
        if (xdg[0])
            return std::string(xdg) + "/pagemade/colors.conf";
    if (const char *home = std::getenv("HOME"))
        if (home[0])
            return std::string(home) + "/.config/pagemade/colors.conf";
    return "pagemade-colors.conf";
}

std::vector<Swatch> swatch_library_swatches(const SwatchLibrary &library) {
    std::vector<Swatch> out = default_swatches();
    for (const Swatch &s : library.custom) {
        if (is_builtin(s.id))
            continue;
        bool duplicate = false;
        for (const Swatch &have : out)
            duplicate |= have.name == s.name;
        if (!duplicate)
            out.push_back(s);
    }
    return out;
}

const Swatch *swatch_library_find(const SwatchLibrary &library, const std::string &name) {
    for (const Swatch &s : library.custom)
        if (s.name == name)
            return &s;
    return nullptr;
}

bool swatch_library_add(SwatchLibrary &library, const std::string &path,
                        const std::vector<Swatch> &doc_swatches,
                        const std::string &name, const Cmyk &cmyk, SwatchId *id) {
    if (name.empty())
        return false;
    for (const Swatch &s : library.custom)
        if (s.name == name)
            return false;
    Swatch s;
    s.id = next_id(library, doc_swatches);
    s.name = name;
    s.cmyk = {std::clamp(cmyk.c, 0.f, 100.f), std::clamp(cmyk.m, 0.f, 100.f),
              std::clamp(cmyk.y, 0.f, 100.f), std::clamp(cmyk.k, 0.f, 100.f)};
    s.rgb = cmyk_to_rgb(s.cmyk);
    library.custom.push_back(s);
    if (id)
        *id = s.id;
    return swatch_library_save(library, path);
}

bool swatch_library_set(SwatchLibrary &library, const std::string &path, SwatchId id,
                        const std::string &name, const Cmyk &cmyk) {
    if (is_builtin(id) || name.empty())
        return false;
    Swatch *found = nullptr;
    for (Swatch &s : library.custom)
        if (s.id == id)
            found = &s;
    if (!found)
        return false;
    for (const Swatch &s : library.custom)
        if (s.name == name && s.id != id)
            return false;
    found->name = name;
    found->cmyk = {std::clamp(cmyk.c, 0.f, 100.f), std::clamp(cmyk.m, 0.f, 100.f),
                   std::clamp(cmyk.y, 0.f, 100.f), std::clamp(cmyk.k, 0.f, 100.f)};
    found->rgb = cmyk_to_rgb(found->cmyk);
    return swatch_library_save(library, path);
}

bool swatch_library_upsert(SwatchLibrary &library, const std::string &path,
                           const std::vector<Swatch> &doc_swatches,
                           const std::string &name, const Cmyk &cmyk) {
    if (name.empty())
        return false;
    for (const Swatch &s : library.custom)
        if (s.name == name)
            return swatch_library_set(library, path, s.id, name, cmyk);
    return swatch_library_add(library, path, doc_swatches, name, cmyk);
}

void swatch_library_seed(PageDoc &doc, const SwatchLibrary &library) {
    for (const Swatch &lib : library.custom) {
        /* Names bridge the two tables: the document's own color wins, and
         * anything new takes a fresh document id (the library's ids live
         * in a different table and must never move a document reference). */
        bool named = false;
        SwatchId id = kRegistration;
        for (const Swatch &have : doc.swatches) {
            named |= have.name == lib.name;
            id = std::max(id, have.id);
        }
        if (named)
            continue;
        Swatch s = lib;
        s.id = id + 1;
        doc.swatches.push_back(s);
    }
}

bool swatch_library_remove(SwatchLibrary &library, const std::string &path, SwatchId id) {
    if (is_builtin(id))
        return false;
    const size_t before = library.custom.size();
    library.custom.erase(
        std::remove_if(library.custom.begin(), library.custom.end(),
                       [id](const Swatch &s) { return s.id == id; }),
        library.custom.end());
    if (library.custom.size() == before)
        return false;
    return swatch_library_save(library, path);
}

} // namespace pagemade
