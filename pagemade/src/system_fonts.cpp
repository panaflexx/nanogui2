/*
 * pagemade/system_fonts.cpp — see system_fonts.h.
 *
 * fc-list is fontconfig's query tool. The dev library is not required:
 * one shot at startup records family, style, file, weight and slant, and
 * FontLibrary loads a file only when that cut is shaped.
 */
#include "system_fonts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <cstring>
#include <dirent.h>
#include <hb-ot.h>
#include <hb.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pagemade {

namespace {

int css_weight(int fc) {
    struct P { int fc, css; };
    static const P p[] = {
        {0, 100}, {40, 200}, {50, 300}, {55, 350}, {75, 380},
        {80, 400}, {100, 500}, {180, 600}, {200, 700},
        {205, 800}, {210, 900}, {215, 950}
    };
    const int n = (int) (sizeof p / sizeof p[0]);
    if (fc <= p[0].fc)
        return p[0].css;
    for (int i = 1; i < n; ++i) {
        if (fc <= p[i].fc) {
            double t = double(fc - p[i - 1].fc) / double(p[i].fc - p[i - 1].fc);
            return (int) std::lround(p[i - 1].css + t * (p[i].css - p[i - 1].css));
        }
    }
    return 950;
}

struct Item {
    std::string family, style, path;
    unsigned index = 0;
    int weight = 400;
    bool italic = false;
    bool ranged = false;   // fontconfig's whole-variable-font entry
};

std::string field_key(const Item &it) { return it.family + "\n" + it.style; }

#ifdef __APPLE__
/* fc-list is fontconfig. A Finder launch does not have Homebrew on PATH,
 * so the Mac build reads the name tables of the fonts installed on the
 * machine. Catalog entries stay unloaded until a story uses them. */

std::string hb_name(hb_face_t *face, hb_ot_name_id_t id) {
    char buf[512];
    unsigned cap = sizeof buf;
    unsigned n = hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &cap, buf);
    if (n == 0)
        return {};
    /* cap is the bytes written, not counting the NUL. Clamp if a long name
     * reports its full length instead. */
    if (cap >= sizeof buf)
        cap = sizeof buf - 1;
    return std::string(buf, cap);
}

bool outline_face(hb_face_t *face) {
    const hb_tag_t tags[] = {
        HB_TAG('g', 'l', 'y', 'f'), HB_TAG('C', 'F', 'F', ' '), HB_TAG('C', 'F', 'F', '2')
    };
    for (hb_tag_t tag : tags) {
        hb_blob_t *b = hb_face_reference_table(face, tag);
        unsigned n = hb_blob_get_length(b);
        hb_blob_destroy(b);
        if (n > 0)
            return true;
    }
    return false;
}

bool skip_family(const std::string &family) {
    return family.find("Emoji") != std::string::npos ||
           family.find("LastResort") != std::string::npos ||
           family.find("Last Resort") != std::string::npos;
}

void read_os2(hb_face_t *face, int &weight, bool &italic) {
    hb_blob_t *os2 = hb_face_reference_table(face, HB_TAG('O', 'S', '/', '2'));
    unsigned len = 0;
    const char *data = hb_blob_get_data(os2, &len);
    if (data && len >= 6) {
        int w = ((unsigned char) data[4] << 8) | (unsigned char) data[5];
        if (w >= 1 && w <= 1000)
            weight = w;
    }
    if (data && len >= 64) {
        unsigned sel = ((unsigned char) data[62] << 8) | (unsigned char) data[63];
        if (sel & 1)
            italic = true;
    }
    hb_blob_destroy(os2);
}

void add_font_file(const std::string &path, std::vector<Item> &items) {
    hb_blob_t *blob = hb_blob_create_from_file_or_fail(path.c_str());
    if (!blob)
        return;
    unsigned count = hb_face_count(blob);
    if (count > 64)
        count = 64;
    for (unsigned i = 0; i < count; ++i) {
        hb_face_t *face = hb_face_create(blob, i);
        if (!face)
            continue;
        if (hb_face_get_glyph_count(face) == 0 || !outline_face(face)) {
            hb_face_destroy(face);
            continue;
        }
        std::string family = hb_name(face, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
        if (family.empty())
            family = hb_name(face, HB_OT_NAME_ID_FONT_FAMILY);
        std::string style = hb_name(face, HB_OT_NAME_ID_TYPOGRAPHIC_SUBFAMILY);
        if (style.empty())
            style = hb_name(face, HB_OT_NAME_ID_FONT_SUBFAMILY);
        if (style.empty())
            style = "Regular";
        if (family.empty() || skip_family(family)) {
            hb_face_destroy(face);
            continue;
        }
        Item it;
        it.family = std::move(family);
        it.style = style;
        it.path = path;
        it.index = i;
        it.weight = 400;
        it.italic = style.find("Italic") != std::string::npos ||
                    style.find("Oblique") != std::string::npos;
        read_os2(face, it.weight, it.italic);
        items.push_back(std::move(it));
        hb_face_destroy(face);
    }
    hb_blob_destroy(blob);
}

void walk_fonts(const std::string &dir, int depth, std::vector<Item> &items,
                std::vector<std::string> &seen) {
    if (depth > 3)
        return;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *ent = readdir(d)) {
        std::string name = ent->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            walk_fonts(path, depth + 1, items, seen);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
        std::string lower = name;
        for (char &c : lower)
            c = (char) std::tolower((unsigned char) c);
        auto ends = [&](const char *ext) {
            size_t n = std::strlen(ext);
            return lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0;
        };
        if (!ends(".ttf") && !ends(".otf") && !ends(".ttc") && !ends(".otc"))
            continue;
        if (lower.find("emoji") != std::string::npos || lower.find("lastresort") != std::string::npos)
            continue;
        char real[PATH_MAX];
        if (!realpath(path.c_str(), real))
            continue;
        if (std::find(seen.begin(), seen.end(), real) != seen.end())
            continue;
        seen.emplace_back(real);
        add_font_file(real, items);
    }
    closedir(d);
}

void collect_mac_fonts(std::vector<Item> &items) {
    std::vector<std::string> seen;
    walk_fonts("/System/Library/Fonts", 0, items, seen);
    walk_fonts("/Library/Fonts", 0, items, seen);
    if (const char *home = std::getenv("HOME"))
        walk_fonts(std::string(home) + "/Library/Fonts", 0, items, seen);
}
#endif

} // namespace

void register_system_fonts(FontLibrary &lib) {
    std::vector<Item> items;
    if (FILE *fp = popen(
            "fc-list -f '%{family[0]}\t%{style[0]}\t%{file}\t%{index}\t%{weight}\t%{slant}\t%{fontformat}\t%{outline}\t%{color}\n' 2>/dev/null",
            "r")) {
    char buf[8192];
    while (fgets(buf, sizeof buf, fp)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        std::string f[9];
        size_t at = 0;
        int n = 0;
        for (; n < 9; ++n) {
            size_t tab = line.find('\t', at);
            if (tab == std::string::npos) {
                f[n++] = line.substr(at);
                break;
            }
            f[n] = line.substr(at, tab - at);
            at = tab + 1;
        }
        if (n < 9 || f[0].empty() || f[2].empty())
            continue;
        if (f[6] != "TrueType" && f[6] != "CFF")
            continue;
        if (f[7] == "False" || f[8] == "True")
            continue;                    // bitmap, or a color font (emoji)
        Item it;
        it.family = f[0];
        it.style = f[1].empty() ? "Regular" : f[1];
        it.path = f[2];
        it.index = (unsigned) std::strtoul(f[3].c_str(), nullptr, 10);
        int slant = (int) std::strtol(f[5].c_str(), nullptr, 10);
        /* A variable font is listed once per named instance and once more
         * with its weight range ("[80 205]") and no style. That extra entry
         * is the default instance; it must not displace the real Regular. */
        it.ranged = !f[4].empty() && f[4][0] == '[';
        it.weight = it.ranged ? 400 : css_weight((int) std::strtol(f[4].c_str(), nullptr, 10));
        it.italic = slant >= 100 || it.style.find("Italic") != std::string::npos ||
                    it.style.find("Oblique") != std::string::npos;
        items.push_back(std::move(it));
    }
    pclose(fp);
    }
#ifdef __APPLE__
    /* fontconfig is not part of macOS. The .app must not need Homebrew's
     * fc-list, which Finder would not find anyway. */
    if (items.empty())
        collect_mac_fonts(items);
#endif
    if (items.empty())
        return;

    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        auto fold = [](std::string s) {
            for (char &c : s)
                c = (char) std::tolower((unsigned char) c);
            return s;
        };
        const std::string af = fold(a.family), bf = fold(b.family);
        if (af != bf) return af < bf;
        if (a.family != b.family) return a.family < b.family;
        if (a.weight != b.weight) return a.weight < b.weight;
        if (a.italic != b.italic) return !a.italic && b.italic;
        if (a.style != b.style) return a.style < b.style;
        return !a.ranged && b.ranged;
    });

    std::string prev;
    for (const Item &it : items) {
        const std::string key = field_key(it);
        if (key == prev)
            continue;
        prev = key;
        lib.add_catalog(it.family, it.style, it.path, it.index, it.weight, it.italic);
    }
}

} // namespace pagemade
