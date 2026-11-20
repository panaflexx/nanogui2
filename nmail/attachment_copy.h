/*
 * attachment_copy.h — Copy an image attachment onto the clipboard in the
 * form pagemade pastes.
 *
 * pagemade reads image/svg+xml (not a bare image/png or image/jpeg) and
 * lifts one upright embedded picture out as a picture frame. The SVG's
 * width and height are the upright pixel size, in points: a paste shrinks
 * a photo that is larger than the page and keeps the file's own bytes.
 *
 * JPEG and PNG go through unchanged. A phone JPEG's EXIF orientation is
 * 5..8 when the stored pixels are sideways; the box is swapped so it
 * matches the upright pixels pagemade draws. HEIC/AVIF go through too
 * when this build has libheif (the handle size is already upright).
 * GIF, BMP and the other formats stb_image reads are rewritten as PNG,
 * because pagemade's picture loader does not take them.
 *
 * The translation unit includes stb_image_write.h (with
 * STB_IMAGE_WRITE_IMPLEMENTATION) before this header, so
 * stbi_write_png_to_func is available. This header does not include it:
 * that header compiles its body on every include.
 */
#pragma once

#include "attachment_widgets.h"
#include "heif_decode.h"

#include <nanogui/clipboard.h>
#include <stb_image.h>
#include "../ext/nanovgd/src/nanovg_exif.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

/* True when Copy can build a picture pagemade will place. */
inline bool attachment_can_copy_image(const MailAttachment &a) {
    if (!attachment_is_photo(a) || a.data.size() < 16)
        return false;
    const auto *d = reinterpret_cast<const unsigned char *>(a.data.data());
    if (a.data.size() <= (size_t) INT32_MAX) {
        int w = 0, h = 0, n = 0;
        if (stbi_info_from_memory(d, (int) a.data.size(), &w, &h, &n) &&
            w > 0 && h > 0)
            return true;
    }
    return nmail_heif_sniff(d, a.data.size());
}

namespace {

inline std::string att_b64(const uint8_t *data, size_t n) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned) data[i] << 16;
        const bool b1 = i + 1 < n, b2 = i + 2 < n;
        if (b1) v |= (unsigned) data[i + 1] << 8;
        if (b2) v |= (unsigned) data[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += b1 ? T[(v >> 6) & 63] : '=';
        out += b2 ? T[v & 63] : '=';
    }
    return out;
}

inline void att_png_sink(void *ctx, void *data, int size) {
    if (size > 0)
        static_cast<std::string *>(ctx)->append(static_cast<const char *>(data),
                                                (size_t) size);
}

/* One picture pagemade can place: the bytes, their MIME type, and the
 * upright pixel size of the SVG box. */
struct AttCopyImage {
    const uint8_t *bytes = nullptr;
    size_t size = 0;
    std::string owned;                   // set when the bytes were rewritten
    const char *mime = nullptr;
    int w = 0, h = 0;
};

inline bool att_prepare_copy(const MailAttachment &a, AttCopyImage &im) {
    const auto *d = reinterpret_cast<const uint8_t *>(a.data.data());
    const size_t n = a.data.size();
    const bool jpeg = n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
    const bool png = n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0;
    if ((jpeg || png) && n <= (size_t) INT32_MAX) {
        int w = 0, h = 0, c = 0;
        if (stbi_info_from_memory(d, (int) n, &w, &h, &c) && w > 0 && h > 0) {
            if (jpeg) {
                const int orient = nvg__exifOrientation(d, (int) n);
                if (orient >= 5)
                    std::swap(w, h);
            }
            im.bytes = d;
            im.size = n;
            im.mime = jpeg ? "image/jpeg" : "image/png";
            im.w = w;
            im.h = h;
            return true;
        }
    }
    int hw = 0, hh = 0;
    if (nmail_heif_size(d, n, hw, hh)) {
        im.bytes = d;
        im.size = n;
        im.mime = nmail_heif_mime(d, n);
        im.w = hw;
        im.h = hh;
        return true;
    }
    /* stb formats pagemade does not read (GIF, BMP, TGA): a PNG of the
     * first frame. A raster past 64 megapixels is refused rather than
     * decoded into a huge buffer. */
    if (n > (size_t) INT32_MAX)
        return false;
    int w = 0, h = 0, c = 0;
    if (!stbi_info_from_memory(d, (int) n, &w, &h, &c) || w <= 0 || h <= 0)
        return false;
    if ((size_t) w * (size_t) h > (size_t) 64 << 20)
        return false;
    unsigned char *px = stbi_load_from_memory(d, (int) n, &w, &h, &c, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px)
            stbi_image_free(px);
        return false;
    }
    std::string png_bytes;
    const int ok = stbi_write_png_to_func(att_png_sink, &png_bytes, w, h, 4,
                                          px, w * 4);
    stbi_image_free(px);
    if (!ok || png_bytes.size() < 8)
        return false;
    im.owned = std::move(png_bytes);
    im.bytes = reinterpret_cast<const uint8_t *>(im.owned.data());
    im.size = im.owned.size();
    im.mime = "image/png";
    im.w = w;
    im.h = h;
    return true;
}

inline std::string att_image_svg(const AttCopyImage &im) {
    const std::string b64 = att_b64(im.bytes, im.size);
    const std::string ws = std::to_string(im.w);
    const std::string hs = std::to_string(im.h);
    std::string svg;
    svg.reserve(b64.size() + 256);
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"";
    svg += ws;
    svg += "\" height=\"";
    svg += hs;
    svg += "\" viewBox=\"0 0 ";
    svg += ws;
    svg += " ";
    svg += hs;
    svg += "\"><image x=\"0\" y=\"0\" width=\"";
    svg += ws;
    svg += "\" height=\"";
    svg += hs;
    svg += "\" preserveAspectRatio=\"none\" xlink:href=\"data:";
    svg += im.mime;
    svg += ";base64,";
    svg += b64;
    svg += "\"/></svg>";
    return svg;
}

} // namespace

/* image/svg+xml first, then the picture bytes. pagemade asks for the SVG.
 * An image editor that asks for the picture type gets the bytes. */
inline bool copy_attachment_image(GLFWwindow *win, const MailAttachment &a) {
    if (!win)
        return false;
    AttCopyImage im;
    if (!att_prepare_copy(a, im))
        return false;
    std::string svg = att_image_svg(im);
    if (svg.empty())
        return false;
    std::vector<std::pair<std::string, std::string>> formats;
    formats.emplace_back("image/svg+xml", std::move(svg));
    formats.emplace_back(im.mime, std::string(reinterpret_cast<const char *>(im.bytes),
                                              im.size));
    nanogui::write_clipboard(win, formats);
    return true;
}
