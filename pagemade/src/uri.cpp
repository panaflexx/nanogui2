/*
 * pagemade/uri.cpp — see uri.h.
 */
#include "uri.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(__linux__)
#  include <unistd.h>
#endif

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

std::vector<std::string> uri_list_entries(const std::string &text) {
    std::vector<std::string> out;
    size_t at = 0;
    bool first = true;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t' ||
                                 line.back() == '\0'))
            line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t'))
            ++lead;
        line.erase(0, lead);
        const bool action = first && (line == "copy" || line == "cut");   // GNOME's first line
        first = false;
        if (line.empty() || line[0] == '#' || action)
            continue;
        out.push_back(line);
    }
    return out;
}

namespace {

bool same_ci(const std::string &a, const std::string &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char) a[i]) != std::tolower((unsigned char) b[i]))
            return false;
    return true;
}

/* "scheme://user@host:port/path" in pieces; the path keeps its escapes */
bool split_uri(const std::string &uri, std::string &scheme, std::string &user, std::string &host,
               std::string &port, std::string &path) {
    const size_t colon = uri.find("://");
    if (colon == std::string::npos || colon == 0)
        return false;
    scheme = uri.substr(0, colon);
    for (char &c : scheme)
        c = (char) std::tolower((unsigned char) c);
    const size_t auth = colon + 3;
    const size_t slash = uri.find('/', auth);
    std::string a = uri.substr(auth, slash == std::string::npos ? std::string::npos : slash - auth);
    path = slash == std::string::npos ? std::string("/") : uri.substr(slash);
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos)
        path.erase(q);
    const size_t atsign = a.rfind('@');
    if (atsign != std::string::npos) {
        user = percent_decode(a.substr(0, atsign));
        const size_t semi = user.find(';');      // smb://DOMAIN;user@host
        if (semi != std::string::npos)
            user = user.substr(semi + 1);
        const size_t pw = user.find(':');
        if (pw != std::string::npos)
            user.erase(pw);
        a = a.substr(atsign + 1);
    }
    const size_t pc = a.rfind(':');
    if (pc != std::string::npos && a.find(']') == std::string::npos) {
        port = a.substr(pc + 1);
        a.erase(pc);
    }
    host = percent_decode(a);
    return !host.empty();
}

/* A gvfs mount name, "type:key=value,key=value", in pieces */
bool split_mount(const std::string &name, std::string &type,
                 std::vector<std::pair<std::string, std::string>> &keys) {
    const size_t colon = name.find(':');
    if (colon == std::string::npos)
        return false;
    type = name.substr(0, colon);
    size_t at = colon + 1;
    while (at < name.size()) {
        size_t comma = name.find(',', at);
        if (comma == std::string::npos)
            comma = name.size();
        const std::string kv = name.substr(at, comma - at);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos)
            keys.emplace_back(kv.substr(0, eq), percent_decode(kv.substr(eq + 1)));
        at = comma + 1;
    }
    return true;
}

std::string mount_key(const std::vector<std::pair<std::string, std::string>> &keys, const char *k) {
    for (const auto &kv : keys)
        if (kv.first == k)
            return kv.second;
    return {};
}

} // namespace

bool gvfs_path_from_uri(const std::string &uri, const std::string &gvfs_root, std::string &path) {
    std::string scheme, user, host, port, upath;
    if (gvfs_root.empty() || !split_uri(uri, scheme, user, host, port, upath) || scheme == "file")
        return false;
    /* The path's segments, still escaped; decoded one by one below */
    std::vector<std::string> segs;
    for (size_t at = 0; at < upath.size();) {
        size_t slash = upath.find('/', at);
        if (slash == std::string::npos)
            slash = upath.size();
        if (slash > at)
            segs.push_back(percent_decode(upath.substr(at, slash - at)));
        at = slash + 1;
    }
    for (const std::string &s : segs)
        if (s == ".." || s.find('/') != std::string::npos || s.find('\0') != std::string::npos)
            return false;                // never out of the mount
    std::error_code ec;
    for (std::filesystem::directory_iterator it(gvfs_root, ec), end; !ec && it != end; it.increment(ec)) {
        std::string type;
        std::vector<std::pair<std::string, std::string>> keys;
        if (!split_mount(it->path().filename().string(), type, keys))
            continue;
        size_t skip = 0;                 // segments the mount itself stands for
        if (scheme == "smb") {
            if (type != "smb-share" || segs.empty() || !same_ci(mount_key(keys, "server"), host) ||
                !same_ci(mount_key(keys, "share"), segs[0]))
                continue;
            skip = 1;
        } else {
            /* sftp, ftp, dav, ... : the scheme names the mount type
             * (davs is dav with ssl=true) */
            const std::string want = scheme == "davs" ? "dav" : scheme;
            if (type != want || !same_ci(mount_key(keys, "host"), host))
                continue;
        }
        const std::string mu = mount_key(keys, "user");
        if (!user.empty() && !mu.empty() && mu != user)
            continue;
        const std::string mp = mount_key(keys, "port");
        if (!port.empty() && !mp.empty() && mp != port)
            continue;
        std::string p = it->path().string();
        for (size_t i = skip; i < segs.size(); ++i)
            p += "/" + segs[i];
        if (std::filesystem::exists(p, ec)) {
            path = p;
            return true;
        }
    }
    return false;
}

bool local_path_from_uri(const std::string &entry, std::string &path) {
    std::error_code ec;
    std::string p;
    if (file_path_from_uri(entry, p) || (!entry.empty() && entry[0] == '/' && !uri_has_scheme(entry))) {
        if (p.empty())
            p = entry;
        if (std::filesystem::is_regular_file(p, ec)) {
            path = p;
            return true;
        }
        return false;
    }
#if defined(__linux__)
    const std::string root = "/run/user/" + std::to_string((unsigned long) getuid()) + "/gvfs";
    if (gvfs_path_from_uri(entry, root, p) && std::filesystem::is_regular_file(p, ec)) {
        path = p;
        return true;
    }
#endif
    return false;
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
