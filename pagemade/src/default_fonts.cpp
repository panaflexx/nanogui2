/*
 * pagemade/default_fonts.cpp — see default_fonts.h.
 */
#include "default_fonts.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <limits.h>
#include <mach-o/dyld.h>
#endif

#ifndef PAGEMADE_RESOURCE_DIR
#define PAGEMADE_RESOURCE_DIR "resources"
#endif
#ifndef PAGEMADE_DATA_DIR
#define PAGEMADE_DATA_DIR "pagemade/resources"
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

#ifdef __APPLE__
/* .../PageMade.app/Contents/MacOS/pagemade -> .../Contents/Resources.
 * Empty when the process is a loose binary. */
std::string bundle_resources_dir() {
    char buf[PATH_MAX];
    uint32_t len = sizeof buf;
    if (_NSGetExecutablePath(buf, &len) != 0)
        return {};
    char real[PATH_MAX];
    const char *exe = realpath(buf, real) ? real : buf;
    const std::string path(exe);
    const std::string marker = ".app/Contents/MacOS/";
    const size_t at = path.rfind(marker);
    if (at == std::string::npos)
        return {};
    return path.substr(0, at) + ".app/Contents/Resources";
}
#endif

bool readable(const std::string &path) {
#ifdef _WIN32
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp)
        return false;
    std::fclose(fp);
    return true;
#else
    return access(path.c_str(), R_OK) == 0;
#endif
}

} // namespace

std::string resource_dir() {
#ifdef __APPLE__
    const std::string bundled = bundle_resources_dir();
    if (!bundled.empty() && readable(bundled + "/resources/Roboto-Regular.ttf"))
        return bundled + "/resources";
#endif
    return PAGEMADE_RESOURCE_DIR;
}

std::string data_dir() {
#ifdef __APPLE__
    const std::string bundled = bundle_resources_dir();
    if (!bundled.empty() &&
        readable(bundled + "/pagemade/resources/hyphenation/hyph-en-us.pat.txt"))
        return bundled + "/pagemade/resources";
#endif
    return PAGEMADE_DATA_DIR;
}

void prepare_app_bundle() {
#ifdef __APPLE__
    const std::string res = bundle_resources_dir();
    if (res.empty())
        return;
    /* theme.cpp opens "resources/NotoColorEmoji.ttf" relative to CWD. */
    if (::chdir(res.c_str()) != 0)
        std::fprintf(stderr, "pagemade: could not chdir to %s\n", res.c_str());
    /* libheif dlopens codec plugins. The copy in the bundle is not on the
     * link line, so the library's compiled-in Homebrew path would miss it
     * once this app leaves the build machine. */
    const std::string plugins = res + "/../Frameworks/heif";
    char real[PATH_MAX];
    if (realpath(plugins.c_str(), real))
        ::setenv("LIBHEIF_PLUGIN_PATH", real, 1);
#else
    return;
#endif
}

void add_family(FontLibrary &fonts, const std::string &family, const std::vector<Face> &faces) {
    for (const Face &f : faces)
        for (const std::string &p : f.paths)
            if (fonts.add_file(family, f.style, p))
                break;
}

void register_default_fonts(FontLibrary &fonts) {
    const std::string res = resource_dir() + "/";
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
