/*
 * Files Finder hands the app (Open With, double-click, `open file.pagemade`).
 * macOS sends those as an Apple Event, not as argv. No-ops off the Mac.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#if defined(__APPLE__)
std::vector<std::string> take_pending_mac_opens();
void set_mac_open_callback(const std::function<void(const std::string &)> &cb);
#else
inline std::vector<std::string> take_pending_mac_opens() { return {}; }
inline void set_mac_open_callback(const std::function<void(const std::string &)> &) {}
#endif
