/*
 * pagemade/default_fonts.cpp — see default_fonts.h.
 */
#include "default_fonts.h"

#include <string>
#include <vector>

#ifndef PAGEMADE_RESOURCE_DIR
#define PAGEMADE_RESOURCE_DIR "resources"
#endif

namespace pagemade {

namespace {

struct Face {
    const char *style;
    std::vector<std::string> paths;   // first that loads wins
};

const std::string urw = "/usr/share/fonts/opentype/urw-base35/";
const std::string lib = "/usr/share/fonts/truetype/liberation/";
const std::string dv  = "/usr/share/fonts/truetype/dejavu/";
const std::string res = std::string(PAGEMADE_RESOURCE_DIR) + "/";

void add_family(FontLibrary &fonts, const std::string &family, const std::vector<Face> &faces) {
    for (const Face &f : faces)
        for (const std::string &p : f.paths)
            if (fonts.add_file(family, f.style, p))
                break;
}

} // namespace

void register_default_fonts(FontLibrary &fonts) {
    add_family(fonts, "Serif", {
        {"Regular",     {urw + "P052-Roman.otf", lib + "LiberationSerif-Regular.ttf",
                         dv + "DejaVuSerif.ttf", res + "Roboto-Regular.ttf"}},
        {"Bold",        {urw + "P052-Bold.otf", lib + "LiberationSerif-Bold.ttf",
                         dv + "DejaVuSerif-Bold.ttf", res + "Roboto-Bold.ttf"}},
        {"Italic",      {urw + "P052-Italic.otf", lib + "LiberationSerif-Italic.ttf",
                         res + "Roboto-Italic.ttf"}},
        {"Bold Italic", {urw + "P052-BoldItalic.otf", lib + "LiberationSerif-BoldItalic.ttf",
                         res + "Roboto-BoldItalic.ttf"}},
    });
    add_family(fonts, "Sans", {
        {"Regular",     {urw + "NimbusSans-Regular.otf", lib + "LiberationSans-Regular.ttf",
                         dv + "DejaVuSans.ttf", res + "Roboto-Regular.ttf"}},
        {"Bold",        {urw + "NimbusSans-Bold.otf", lib + "LiberationSans-Bold.ttf",
                         dv + "DejaVuSans-Bold.ttf", res + "Roboto-Bold.ttf"}},
        {"Italic",      {urw + "NimbusSans-Italic.otf", lib + "LiberationSans-Italic.ttf",
                         res + "Roboto-Italic.ttf"}},
        {"Bold Italic", {urw + "NimbusSans-BoldItalic.otf", lib + "LiberationSans-BoldItalic.ttf",
                         res + "Roboto-BoldItalic.ttf"}},
    });
    add_family(fonts, "Display", {
        {"Regular",     {urw + "URWGothic-Book.otf", lib + "LiberationSans-Regular.ttf",
                         res + "Roboto-Regular.ttf"}},
        {"Bold",        {urw + "URWGothic-Demi.otf", lib + "LiberationSans-Bold.ttf",
                         res + "Roboto-Bold.ttf"}},
        {"Italic",      {urw + "URWGothic-BookOblique.otf", lib + "LiberationSans-Italic.ttf",
                         res + "Roboto-Italic.ttf"}},
        {"Bold Italic", {urw + "URWGothic-DemiOblique.otf", lib + "LiberationSans-BoldItalic.ttf",
                         res + "Roboto-BoldItalic.ttf"}},
    });
}

} // namespace pagemade
