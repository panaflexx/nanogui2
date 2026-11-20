/*
 * pagemade/uri.h — URI references for a publication's assets (RFC 3986).
 *
 * A linked asset is an absolute file: URI (RFC 8089), percent-encoded:
 *   file:///usr/share/fonts/opentype/urw-base35/P052-Roman.otf
 *   file:///C:/Users/me/Pictures/My%20Photo.jpg
 * An embedded asset is a reference relative to the root of the publication's
 * package, the way ODF and EPUB address their parts:
 *   assets/fonts/P052-Roman.otf
 * Other schemes (http:, https:) are recognized but not fetched yet.
 */
#pragma once

#include <string>
#include <vector>

namespace pagemade {

/* Percent-encode everything but unreserved characters, '/' and the other
 * characters a path segment may hold as-is (: @ ! $ & ' ( ) * + , ; =). */
std::string percent_encode_path(const std::string &s);
/* %XX -> byte. Invalid escapes are kept as they are. */
std::string percent_decode(const std::string &s);

/* "/a b/c.otf" -> "file:///a%20b/c.otf"; "C:\x\y" -> "file:///C:/x/y". */
std::string file_uri(const std::string &path);
/* The local path of a file: URI (file:/p, file:///p, file://localhost/p);
 * false for other schemes and remote hosts. */
bool file_path_from_uri(const std::string &uri, std::string &path);

/* The entries of a file list off the clipboard, in order: a text/uri-list
 * (RFC 2483: one URI a line, '#' comments), GNOME's
 * x-special/gnome-copied-files (a "copy" or "cut" line, then URIs), or
 * plain text with one URI or path a line. Blank lines go; nothing is
 * decoded. */
std::vector<std::string> uri_list_entries(const std::string &text);
/* A network URI (smb://host/share/a.jpg, sftp://user@host/x/y.png, ...) as
 * the path GNOME's gvfs gives it under `gvfs_root` (a mount named like
 * "smb-share:server=host,share=share" or "sftp:host=host,user=user").
 * False when no mount there matches, or the file isn't in it. */
bool gvfs_path_from_uri(const std::string &uri, const std::string &gvfs_root, std::string &path);
/* A clipboard entry as a local file that exists: a file: URI, an absolute
 * path, or (on Linux) a network URI through gvfs in /run/user/<uid>/gvfs. */
bool local_path_from_uri(const std::string &entry, std::string &path);

/* "file:", "http:", ... (a scheme starts with a letter and ends at ':'). */
bool uri_has_scheme(const std::string &uri);
/* A reference into the package: no scheme, not absolute, no ".." segment. */
bool is_package_ref(const std::string &uri);
/* The zip entry a package reference names (percent-decoded). */
std::string package_entry(const std::string &ref);
/* A package reference for a file in a package folder ("assets/fonts"). */
std::string package_ref(const std::string &folder, const std::string &file_name);

} // namespace pagemade
