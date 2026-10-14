/*
    nanogui/fscompat.h -- A minimal std::filesystem subset.

    macOS 10.12 (and other older platforms) ship a libc++ without
    <filesystem>, so the nanogui apps use these free functions instead of
    std::filesystem directly. On Unix they are plain POSIX (stat, dirent,
    getcwd, mkdir); on Windows they forward to std::filesystem, which MSVC
    always provides. All paths are UTF-8 std::strings; errors are swallowed
    (the *_with_ec-style callers in this tree ignored them anyway).

        fs::filename("a/b.jpg")        -> "b.jpg"
        fs::extension("a/b.jpg")       -> ".jpg"   ("" for none / dotfiles)
        fs::stem("a/b.jpg")            -> "b"
        fs::parent_path("a/b.jpg")     -> "a"      ("" if none)
        fs::join("a", "b")             -> "a/b"
        fs::exists / fs::is_directory / fs::is_regular_file
        fs::current_path()
        fs::create_directories(path)   -> mkdir -p
        fs::remove_all(path)           -> recursive delete
        fs::walk_files(root, cb)       -> recursive regular-file walk;
                                          cb returns false to stop early;
                                          directory symlinks not followed

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/common.h>

#include <cstring>
#include <functional>
#include <string>

#if defined(_WIN32)
#include <filesystem>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

NAMESPACE_BEGIN(nanogui)
namespace fs {

#if defined(_WIN32)

inline std::string filename(const std::string &p) {
    return std::filesystem::path(p).filename().string();
}
inline std::string extension(const std::string &p) {
    return std::filesystem::path(p).extension().string();
}
inline std::string stem(const std::string &p) {
    return std::filesystem::path(p).stem().string();
}
inline std::string parent_path(const std::string &p) {
    return std::filesystem::path(p).parent_path().string();
}
inline std::string join(const std::string &a, const std::string &b) {
    return (std::filesystem::path(a) / b).string();
}
inline bool exists(const std::string &p) {
    std::error_code ec; return std::filesystem::exists(p, ec);
}
inline bool is_directory(const std::string &p) {
    std::error_code ec; return std::filesystem::is_directory(p, ec);
}
inline bool is_regular_file(const std::string &p) {
    std::error_code ec; return std::filesystem::is_regular_file(p, ec);
}
inline std::string current_path() {
    std::error_code ec; return std::filesystem::current_path(ec).string();
}
inline bool create_directories(const std::string &p) {
    std::error_code ec;
    return std::filesystem::create_directories(p, ec) ||
           std::filesystem::is_directory(p, ec);
}
inline void remove_all(const std::string &p) {
    std::error_code ec; std::filesystem::remove_all(p, ec);
}
inline bool walk_files(const std::string &root,
                       const std::function<bool(const std::string &)> &cb) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator
        it(root, std::filesystem::directory_options::skip_permission_denied, ec),
        end;
    while (!ec && it != end) {
        const std::filesystem::directory_entry entry = *it;
        it.increment(ec);
        if (ec) break;
        std::error_code ec2;
        if (entry.is_regular_file(ec2) && !ec2 && !cb(entry.path().string()))
            return false;
    }
    return true;
}

#else /* POSIX */

inline std::string filename(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string extension(const std::string &path) {
    const std::string name = filename(path);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)   /* ".hidden" has no extension */
        return std::string();
    return name.substr(dot);
}

inline std::string stem(const std::string &path) {
    const std::string name = filename(path);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return name;
    return name.substr(0, dot);
}

inline std::string parent_path(const std::string &path) {
    if (path.empty())
        return std::string();
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/')     /* ignore trailing slashes */
        --end;
    const size_t slash = path.rfind('/', end - 1);
    if (slash == std::string::npos)
        return std::string();
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

inline std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (b.front() == '/') return b;
    if (a.back() == '/') return a + b;
    return a + '/' + b;
}

inline bool exists(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
inline bool is_directory(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
inline bool is_regular_file(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

inline std::string current_path() {
    char buf[4096];
    return ::getcwd(buf, sizeof buf) ? std::string(buf) : std::string();
}

/* mkdir -p. True when the directory exists afterwards. */
inline bool create_directories(const std::string &path) {
    if (path.empty())
        return false;
    /* mkdir each prefix that ends at a component boundary: "a", "a/b", ... */
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/')
            continue;
        if (path[i - 1] == '/')                 /* root, "//", trailing '/' */
            continue;
        const std::string cur = path.substr(0, i);
        if (::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST)
            return false;
    }
    return is_directory(path);
}

/* Recursive delete; errors are ignored (best effort, like the test and
   cleanup call sites want). Symlinks are unlinked, never followed. */
inline void remove_all(const std::string &path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = ::opendir(path.c_str())) {
            while (dirent *e = ::readdir(d)) {
                if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
                    continue;
                remove_all(join(path, e->d_name));
            }
            ::closedir(d);
        }
        ::rmdir(path.c_str());
    } else {
        ::unlink(path.c_str());
    }
}

/* Recursive regular-file walk, skipping anything that can't be opened
   (the skip_permission_denied behavior the call sites used). The callback
   returns false to stop the walk early; walk_files returns false then. */
inline bool walk_files(const std::string &root,
                       const std::function<bool(const std::string &)> &cb) {
    DIR *d = ::opendir(root.c_str());
    if (!d)
        return true;
    bool go = true;
    while (go) {
        dirent *e = ::readdir(d);
        if (!e)
            break;
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        const std::string path = join(root, e->d_name);
        /* Like recursive_directory_iterator: symlinked files count, but
           symlinked directories aren't descended (no cycles). */
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0)
            continue;
        if (S_ISLNK(st.st_mode) &&
            (::stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)))
            continue;
        if (S_ISDIR(st.st_mode))
            go = walk_files(path, cb);
        else if (S_ISREG(st.st_mode))
            go = cb(path);
    }
    ::closedir(d);
    return go;
}

#endif

} // namespace fs
NAMESPACE_END(nanogui)
