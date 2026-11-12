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
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
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

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                s.c_str(), -1, nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0')
        w.pop_back();
    return w;
}

std::string wide_to_utf8(const std::wstring &w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, nullptr, nullptr);
    return s;
}

bool file_exists(const std::string &utf8) {
    std::wstring w = utf8_to_wide(utf8);
    if (w.empty())
        return false;
    for (wchar_t &c : w)
        if (c == L'/')
            c = L'\\';
    const DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/* Directory containing this executable, UTF-8, forward slashes, no trailing slash. */
std::string exe_directory() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size())
        return {};
    buf.resize(n);
    const auto slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    buf.resize(slash);
    for (wchar_t &c : buf)
        if (c == L'\\')
            c = L'/';
    return wide_to_utf8(buf);
}

/* resources/ beside the exe when `marker` is there, else the compile-time dir. */
std::string bundled_or(const char *marker, const char *compiled) {
    const std::string exe = exe_directory();
    if (!exe.empty()) {
        const std::string beside = exe + "/resources";
        if (file_exists(beside + marker))
            return beside;
    }
    return compiled;
}
#endif

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

bool readable(const std::string &path) {
    return access(path.c_str(), R_OK) == 0;
}
#endif


void add_family(FontLibrary &fonts, const std::string &family, const std::vector<Face> &faces) {
    for (const Face &f : faces)
        for (const std::string &p : f.paths)
            if (fonts.add_file(family, f.style, p))
                break;
}

} // namespace

std::string resource_dir() {
#if defined(_WIN32)
    return bundled_or("/Roboto-Regular.ttf", PAGEMADE_RESOURCE_DIR);
#else
#ifdef __APPLE__
    const std::string bundled = bundle_resources_dir();
    if (!bundled.empty() && readable(bundled + "/resources/Roboto-Regular.ttf"))
        return bundled + "/resources";
#endif
    return PAGEMADE_RESOURCE_DIR;
#endif
}

std::string data_dir() {
#if defined(_WIN32)
    return bundled_or("/hyphenation/hyph-en-us.pat.txt", PAGEMADE_DATA_DIR);
#else
#ifdef __APPLE__
    const std::string bundled = bundle_resources_dir();
    if (!bundled.empty() &&
        readable(bundled + "/pagemade/resources/hyphenation/hyph-en-us.pat.txt"))
        return bundled + "/pagemade/resources";
#endif
    return PAGEMADE_DATA_DIR;
#endif
}

std::string bundled_font_dir() { return resource_dir(); }
std::string bundled_data_dir() { return data_dir(); }

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
