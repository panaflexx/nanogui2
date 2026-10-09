/*
 * pagemade/uri.cpp — see uri.h.
 */
#include "uri.h"

#include <cctype>
#include <cstring>

namespace pagemade {

namespace {

bool keep_in_path(unsigned char c) {
    return std::isalnum(c) || std::strchr("-._~/:@!$&'()*+,;=", c);
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool drive_letter(const std::string &s, size_t at) {
    return s.size() >= at + 2 && std::isalpha((unsigned char) s[at]) && s[at + 1] == ':';
}

} // namespace

std::string percent_encode_path(const std::string &s) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c && keep_in_path(c)) {
            out += (char) c;
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

std::string percent_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += (char) (hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string file_uri(const std::string &path) {
    std::string p = path;
    for (char &c : p)
        if (c == '\\')
            c = '/';                     // Windows separators
    if (drive_letter(p, 0))
        p = "/" + p;                     // C:/x -> /C:/x
    return "file://" + percent_encode_path(p);
}

bool file_path_from_uri(const std::string &uri, std::string &path) {
    if (uri.size() < 5 || (uri.compare(0, 5, "file:") != 0 && uri.compare(0, 5, "FILE:") != 0))
        return false;
    std::string rest = uri.substr(5);
    if (rest.compare(0, 2, "//") == 0) {
        rest = rest.substr(2);
        size_t slash = rest.find('/');
        std::string host = rest.substr(0, slash);
        if (!host.empty() && host != "localhost")
            return false;                // a remote host: not a local path
        rest = slash == std::string::npos ? "/" : rest.substr(slash);
    }
    if (rest.empty() || rest[0] != '/')
        return false;
    path = percent_decode(rest);
#if defined(_WIN32)
    if (drive_letter(path, 1))
        path = path.substr(1);           // /C:/x -> C:/x
#endif
    return true;
}

bool uri_has_scheme(const std::string &uri) {
    if (uri.empty() || !std::isalpha((unsigned char) uri[0]))
        return false;
    for (size_t i = 1; i < uri.size(); ++i) {
        unsigned char c = (unsigned char) uri[i];
        if (c == ':')
            return i > 1;                // one letter would be a drive, "C:\..."
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.')
            return false;
    }
    return false;
}

bool is_package_ref(const std::string &uri) {
    if (uri.empty() || uri[0] == '/' || uri_has_scheme(uri))
        return false;
    std::string p = "/" + uri + "/";
    return p.find("/../") == std::string::npos && p.find("/./") == std::string::npos;
}

std::string package_entry(const std::string &ref) {
    return percent_decode(ref);
}

std::string package_ref(const std::string &folder, const std::string &file_name) {
    return percent_encode_path(folder) + "/" + percent_encode_path(file_name);
}

} // namespace pagemade
