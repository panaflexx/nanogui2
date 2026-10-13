/*
 * pagemade/image_color.h — RGB color profiles for pictures.
 *
 * Phones shoot Display P3: the same tone curve and white as sRGB, wider
 * primaries. Shown as if it were sRGB, a P3 photo looks flat and its reds
 * go brown. The screen is treated as sRGB, so the display view converts
 * to it. PDF export keeps the file's own samples and attaches the profile
 * (ICCBased), so a viewer or RIP maps the full gamut itself.
 *
 * Only matrix/TRC RGB profiles are understood: sRGB, Display P3, Adobe RGB,
 * ProPhoto and the like, which is what cameras and phones write. Anything
 * else (CMYK, LUT-based, gray) is left alone.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pagemade {

/* An RGB display profile with a well-formed header. Says nothing about
 * whether the tags parse. */
bool icc_is_rgb(const std::vector<uint8_t> &icc);

/* True when `icc` is a matrix/TRC RGB profile that differs from sRGB
 * enough to matter on screen. */
bool icc_needs_conversion(const std::vector<uint8_t> &icc);

/* Converts straight RGBA, `pixels` long, from `icc` to sRGB in place.
 * Alpha is untouched. A profile that is sRGB already, or one that is not
 * understood, leaves the pixels as they are. */
void icc_to_srgb(const std::vector<uint8_t> &icc, uint8_t *rgba, size_t pixels);

/* Tone curves an H.273 (nclx) description can name. */
enum class Transfer { SRGB, Gamma22, Gamma28, Linear };

/* A small ICC v2 display profile from CIE xy primaries and white, for files
 * that describe their color with numbers instead of a profile (HEIF nclx).
 * Empty when the primaries and curve are sRGB's, which needs no profile. */
std::vector<uint8_t> icc_from_primaries(const float red[2], const float green[2],
                                        const float blue[2], const float white[2],
                                        Transfer transfer, const std::string &description);

/* Baseline JPEG of straight RGBA (alpha dropped), quality 1..100. */
bool encode_jpeg(const uint8_t *rgba, int width, int height, int quality, std::string *out);

} // namespace pagemade
