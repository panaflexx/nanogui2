/*
 * pagemade/user_config.h — where per-user settings are stored.
 *
 * Windows: %APPDATA%\pagemade\<name>, else
 * %USERPROFILE%\AppData\Roaming\pagemade\<name>. An installed shortcut
 * starts in Program Files, which a normal user cannot write, and Windows
 * sets neither XDG_CONFIG_HOME nor HOME.
 * Elsewhere: $XDG_CONFIG_HOME/pagemade/<name>, else
 * ~/.config/pagemade/<name>.
 * When none of those are set, the bare name pagemade-<name> in the working
 * directory. Callers create the directory when they save.
 */
#pragma once

#include <cstdlib>
#include <string>

namespace pagemade {

inline std::string user_config_file(const char *name) {
#ifdef _WIN32
    const char *base = std::getenv("APPDATA");
    if (base && base[0])
        return std::string(base) + "\\pagemade\\" + name;
    const char *profile = std::getenv("USERPROFILE");
    if (profile && profile[0])
        return std::string(profile) + "\\AppData\\Roaming\\pagemade\\" + name;
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
        if (xdg[0])
            return std::string(xdg) + "/pagemade/" + name;
    if (const char *home = std::getenv("HOME"))
        if (home[0])
            return std::string(home) + "/.config/pagemade/" + name;
#endif
    return std::string("pagemade-") + name;
}

} // namespace pagemade
