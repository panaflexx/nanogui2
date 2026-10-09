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

/* "file:", "http:", ... (a scheme starts with a letter and ends at ':'). */
bool uri_has_scheme(const std::string &uri);
/* A reference into the package: no scheme, not absolute, no ".." segment. */
bool is_package_ref(const std::string &uri);
/* The zip entry a package reference names (percent-decoded). */
std::string package_entry(const std::string &ref);
/* A package reference for a file in a package folder ("assets/fonts"). */
std::string package_ref(const std::string &folder, const std::string &file_name);

} // namespace pagemade
