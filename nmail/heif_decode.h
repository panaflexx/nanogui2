/*
 * nmail/heif_decode.h — Optional libheif decoding for HEIC/AVIF photo
 * attachments (phone pictures).
 *
 * Compiled in only when CMake finds libheif (NMAIL_HAVE_HEIF); without it
 * both functions are stubs and nmail previews only the formats stb_image
 * knows (JPEG/PNG/GIF/BMP). Mirrors the decoder in pagemade/src/image.cpp.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef NMAIL_HAVE_HEIF
#include <libheif/heif.h>
#endif

/* True when the bytes carry a HEIF ftyp brand: heic/heix/heim/heis/hevc/
 * hevx, avif/avis, or the generic mif1/msf1. Cheap enough to run on every
 * image (non-HEIF files fail the "ftyp" check immediately). */
inline bool nmail_heif_sniff(const uint8_t *d, size_t n) {
#ifdef NMAIL_HAVE_HEIF
    if (n < 16 || std::memcmp(d + 4, "ftyp", 4) != 0)
        return false;
    auto be32 = [](const uint8_t *p) {
        return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
               ((uint32_t) p[2] << 8) | (uint32_t) p[3];
    };
    const size_t box = std::min((size_t) be32(d), n);
    auto has = [&](const char *b) {
        if (std::memcmp(d + 8, b, 4) == 0)
            return true;
        for (size_t p = 16; p + 4 <= box; p += 4)    // compatible brands
            if (std::memcmp(d + p, b, 4) == 0)
                return true;
        return false;
    };
    for (const char *b : {"heic", "heix", "heim", "heis", "hevc", "hevx",
                          "avif", "avis", "mif1", "msf1"})
        if (has(b))
            return true;
#else
    (void) d; (void) n;
#endif
    return false;
}

/* Decode a HEIC/AVIF file to straight 8-bit RGBA, top row first. False
 * when the build has no libheif or the decode fails. libheif applies the
 * irot/imir rotation itself, so the pixels come out upright. */
inline bool nmail_heif_decode_rgba(const uint8_t *data, size_t n,
                                   std::vector<uint8_t> &out,
                                   int &out_w, int &out_h) {
#ifdef NMAIL_HAVE_HEIF
    static const bool init = (heif_init(nullptr), true);
    (void) init;
    heif_context *ctx = heif_context_alloc();
    if (!ctx)
        return false;
    heif_error e = heif_context_read_from_memory_without_copy(ctx, data, n,
                                                              nullptr);
    heif_image_handle *handle = nullptr;
    if (e.code == heif_error_Ok)
        e = heif_context_get_primary_image_handle(ctx, &handle);
    if (e.code != heif_error_Ok || !handle) {
        if (handle)
            heif_image_handle_release(handle);
        heif_context_free(ctx);
        return false;
    }
    heif_image *img = nullptr;
    e = heif_decode_image(handle, &img, heif_colorspace_RGB,
                          heif_chroma_interleaved_RGBA, nullptr);
    bool ok = false;
    if (e.code == heif_error_Ok && img) {
        int stride = 0;
        const uint8_t *px =
            heif_image_get_plane_readonly(img, heif_channel_interleaved,
                                          &stride);
        const int w = heif_image_get_width(img, heif_channel_interleaved);
        const int h = heif_image_get_height(img, heif_channel_interleaved);
        if (px && w > 0 && h > 0 && stride >= w * 4) {
            out.resize((size_t) w * (size_t) h * 4);
            for (int y = 0; y < h; ++y)
                std::memcpy(out.data() + (size_t) y * w * 4,
                            px + (size_t) y * stride, (size_t) w * 4);
            out_w = w;
            out_h = h;
            ok = true;
        }
    }
    if (img)
        heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return ok;
#else
    (void) data; (void) n; (void) out; (void) out_w; (void) out_h;
    return false;
#endif
}
