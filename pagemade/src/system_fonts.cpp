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
};

std::string field_key(const Item &it) { return it.family + "\n" + it.style; }

} // namespace

void register_system_fonts(FontLibrary &lib) {
    FILE *fp = popen(
        "fc-list -f '%{family[0]}\t%{style[0]}\t%{file}\t%{index}\t%{weight}\t%{slant}\t%{fontformat}\t%{outline}\t%{color}\n' 2>/dev/null",
        "r");
    if (!fp)
        return;

    std::vector<Item> items;
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
        it.weight = css_weight((int) std::strtol(f[4].c_str(), nullptr, 10));
        it.italic = slant >= 100 || it.style.find("Italic") != std::string::npos ||
                    it.style.find("Oblique") != std::string::npos;
        items.push_back(std::move(it));
    }
    pclose(fp);

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
        return a.style < b.style;
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
