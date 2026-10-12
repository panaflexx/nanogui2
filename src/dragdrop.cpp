/*
    src/dragdrop.cpp -- Drag and drop data types (the session lives in Screen).

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/dragdrop.h>

NAMESPACE_BEGIN(nanogui)

void DragData::set(const std::string &mime, std::string bytes) {
    for (auto &f : m_formats)
        if (f.first == mime) {
            f.second = std::move(bytes);
            return;
        }
    m_formats.emplace_back(mime, std::move(bytes));
}

const std::string *DragData::get(const std::string &mime) const {
    for (const auto &f : m_formats)
        if (f.first == mime)
            return &f.second;
    return nullptr;
}

std::vector<std::string> DragData::formats() const {
    std::vector<std::string> out;
    for (const auto &f : m_formats)
        out.push_back(f.first);
    return out;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<std::string> DragData::file_paths() const {
    std::vector<std::string> out;
    const std::string *list = get("text/uri-list");
    if (!list)
        return out;
    size_t pos = 0;
    while (pos < list->size()) {
        size_t end = list->find('\n', pos);
        if (end == std::string::npos)
            end = list->size();
        std::string line = list->substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#' || line.compare(0, 5, "file:") != 0)
            continue;                       /* comments, non-file URIs */
        /* file:///path or file://host/path; keep the path */
        size_t start = 5;
        if (line.compare(5, 2, "//") == 0) {
            start = line.find('/', 7);
            if (start == std::string::npos)
                continue;
        }
        std::string path;
        for (size_t i = start; i < line.size(); ++i) {
            int hi, lo;
            if (line[i] == '%' && i + 2 < line.size() &&
                (hi = hex_digit(line[i + 1])) >= 0 && (lo = hex_digit(line[i + 2])) >= 0) {
                path += (char) (hi * 16 + lo);
                i += 2;
            } else {
                path += line[i];
            }
        }
#if defined(_WIN32)
        if (path.size() > 2 && path[0] == '/' && path[2] == ':')
            path.erase(0, 1);               /* /C:/dir -> C:/dir */
#endif
        out.push_back(path);
    }
    return out;
}

std::string DragData::uri_list(const std::vector<std::string> &paths) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const auto &path : paths) {
        out += "file://";
#if defined(_WIN32)
        out += '/';
#endif
        for (unsigned char c : path) {
            bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '/' || c == '-' ||
                         c == '_' || c == '.' || c == '~' || c == ':';
            if (c == '\\') {
                out += '/';
            } else if (plain) {
                out += (char) c;
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
        out += "\r\n";
    }
    return out;
}

bool DragEvent::accept(Widget *w, DropAction preferred) {
    DropAction a = DropNone;
    if (allowed & preferred)
        a = preferred;
    else if (allowed & DropCopy)
        a = DropCopy;
    else if (allowed & DropMove)
        a = DropMove;
    else if (allowed & DropLink)
        a = DropLink;
    if (a == DropNone)
        return false;
    target = w;
    action = a;
    return true;
}

NAMESPACE_END(nanogui)
