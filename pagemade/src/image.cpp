/*
 * pagemade/image.cpp — see image.h.
 *
 * PNG and JPEG parse their own headers for resolution, orientation and the
 * ICC profile (stb_image skips all of that). Pixels go through stb and the
 * same EXIF transform nanovgd uses, so a photo stands the same way here as
 * it does in the rest of the toolkit. The source buffer is never rewritten.
 */
#include "image.h"

#include "nanovg_exif.h"

#include <stb_image.h>
#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pagemade {

namespace {

uint32_t be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
unsigned be16(const uint8_t *p) { return ((unsigned) p[0] << 8) | p[1]; }

unsigned tiff_u16(const uint8_t *t, int off, bool le) {
    return le ? (unsigned) t[off] | ((unsigned) t[off + 1] << 8)
              : (unsigned) t[off + 1] | ((unsigned) t[off] << 8);
}
uint32_t tiff_u32(const uint8_t *t, int off, bool le) {
    return le ? (uint32_t) t[off] | ((uint32_t) t[off + 1] << 8) |
                    ((uint32_t) t[off + 2] << 16) | ((uint32_t) t[off + 3] << 24)
              : (uint32_t) t[off + 3] | ((uint32_t) t[off + 2] << 8) |
                    ((uint32_t) t[off + 1] << 16) | (uint32_t) t[off];
}

/* IFD0 of a TIFF header: orientation, and resolution when it is there.
 * Offsets are from `t`. A bad header leaves the outputs alone. */
void read_tiff_ifd0(const uint8_t *t, size_t n, int &orientation, double &ppi_x, double &ppi_y) {
    if (n < 8)
        return;
    bool le;
    if (t[0] == 'I' && t[1] == 'I')
        le = true;
    else if (t[0] == 'M' && t[1] == 'M')
        le = false;
    else
        return;
    if (tiff_u16(t, 2, le) != 42)
        return;
    uint32_t ifd = tiff_u32(t, 4, le);
    if (ifd < 8 || (size_t) ifd + 2 > n)
        return;
    unsigned count = tiff_u16(t, (int) ifd, le);
    if ((size_t) ifd + 2 + (size_t) count * 12 > n)
        return;

    int orient = 0, unit = 2;
    double xres = 0, yres = 0;
    for (unsigned i = 0; i < count; ++i) {
        int e = (int) ifd + 2 + (int) i * 12;
        unsigned tag = tiff_u16(t, e, le);
        unsigned type = tiff_u16(t, e + 2, le);
        uint32_t nval = tiff_u32(t, e + 4, le);
        if (tag == 0x0112 && type == 3 && nval >= 1) {
            int v = (int) tiff_u16(t, e + 8, le);
            if (v >= 1 && v <= 8)
                orient = v;
        } else if (tag == 0x0128 && type == 3 && nval >= 1) {
            unit = (int) tiff_u16(t, e + 8, le);
        } else if ((tag == 0x011A || tag == 0x011B) && type == 5 && nval >= 1) {
            uint32_t off = tiff_u32(t, e + 8, le);
            if ((size_t) off + 8 <= n) {
                double num = tiff_u32(t, (int) off, le);
                double den = tiff_u32(t, (int) off + 4, le);
                if (den > 0) {
                    if (tag == 0x011A) xres = num / den;
                    else yres = num / den;
                }
            }
        }
    }
    if (orient)
        orientation = orient;
    if (xres > 0 && (unit == 2 || unit == 3)) {
        double scale = unit == 3 ? 2.54 : 1.0;   // centimetre -> inch
        ppi_x = xres * scale;
        ppi_y = (yres > 0 ? yres : xres) * scale;
    }
}

void apply_orientation_size(ImageMetadata &m) {
    if (m.orientation < 1 || m.orientation > 8)
        m.orientation = 1;
    if (m.orientation >= 5) {
        m.width_px = m.stored_height_px;
        m.height_px = m.stored_width_px;
    } else {
        m.width_px = m.stored_width_px;
        m.height_px = m.stored_height_px;
    }
}

bool inflate_zlib(const uint8_t *src, size_t n, std::vector<uint8_t> *out) {
    if (!n || n > 8 * 1024 * 1024)
        return false;
    mz_ulong cap = (mz_ulong) std::max(n * 4, (size_t) 4096);
    for (int i = 0; i < 8; ++i) {
        if (cap > 16 * 1024 * 1024)
            return false;
        out->resize(cap);
        mz_ulong dest = cap;
        int rc = mz_uncompress(out->data(), &dest, src, (mz_ulong) n);
        if (rc == MZ_OK) {
            out->resize(dest);
            return true;
        }
        if (rc != MZ_BUF_ERROR)
            return false;
        cap *= 2;
    }
    return false;
}

/* stb plus the toolkit's EXIF transform. `orient` is applied even when the
 * file is a PNG whose eXIf chunk carried it. */
bool decode_rgba(const uint8_t *data, size_t n, int orient, DecodedImage *out, std::string *error) {
    if (n > (size_t) INT32_MAX) {
        if (error) *error = "picture is too large to decode";
        return false;
    }
    int w = 0, h = 0, comp = 0;
    stbi_set_unpremultiply_on_load(1);
    stbi_convert_iphone_png_to_rgb(1);
    unsigned char *img = stbi_load_from_memory(data, (int) n, &w, &h, &comp, 4);
    if (!img) {
        if (error)
            *error = stbi_failure_reason() ? stbi_failure_reason() : "couldn't decode the picture";
        return false;
    }
    unsigned char *rotated = nvg__exifApply(img, &w, &h, 4, orient);
    if (rotated) {
        stbi_image_free(img);
        img = rotated;
    }
    if (w <= 0 || h <= 0) {
        if (rotated) std::free(img);
        else stbi_image_free(img);
        if (error) *error = "picture has no pixels";
        return false;
    }
    out->width = w;
    out->height = h;
    out->rgba.assign(img, img + (size_t) w * (size_t) h * 4);
    if (rotated) std::free(img);
    else stbi_image_free(img);
    return true;
}

void downsample_rgba(const uint8_t *src, int sw, int sh, int dw, int dh, std::vector<uint8_t> &dst) {
    dst.resize((size_t) dw * (size_t) dh * 4);
    for (int y = 0; y < dh; ++y) {
        int y0 = (int) ((long long) y * sh / dh);
        int y1 = (int) ((long long) (y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; ++x) {
            int x0 = (int) ((long long) x * sw / dw);
            int x1 = (int) ((long long) (x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned sum[4] = {};
            int count = 0;
            for (int yy = y0; yy < y1 && yy < sh; ++yy)
                for (int xx = x0; xx < x1 && xx < sw; ++xx) {
                    const uint8_t *s = src + ((size_t) yy * sw + xx) * 4;
                    for (int c = 0; c < 4; ++c)
                        sum[c] += s[c];
                    ++count;
                }
            uint8_t *o = dst.data() + ((size_t) y * dw + x) * 4;
            if (count < 1) count = 1;
            for (int c = 0; c < 4; ++c)
                o[c] = (uint8_t) (sum[c] / (unsigned) count);
        }
    }
}

/* ---- PNG ---------------------------------------------------------------- */

bool png_sniff(const uint8_t *d, size_t n) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    return n >= 8 && std::memcmp(d, sig, 8) == 0;
}

class PngDecoder : public ImageDecoder {
public:
    const char *id() const override { return "png"; }
    bool sniff(const uint8_t *data, size_t n) const override { return png_sniff(data, n); }

    bool probe(const uint8_t *data, size_t n, ImageMetadata *meta, std::string *error) const override {
        if (!png_sniff(data, n)) {
            if (error) *error = "not a PNG";
            return false;
        }
        ImageMetadata m;
        m.format = id();
        bool ihdr = false;
        size_t p = 8;
        while (p + 12 <= n) {
            uint32_t len = be32(data + p);
            if ((size_t) len > n - p - 12)
                break;
            const uint8_t *type = data + p + 4;
            const uint8_t *chunk = data + p + 8;
            if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
                m.stored_width_px = (int) be32(chunk);
                m.stored_height_px = (int) be32(chunk + 4);
                m.bit_depth = chunk[8];
                switch (chunk[9]) {
                case 0: m.model = ColorModel::Gray; m.channels = 1; break;
                case 2: m.model = ColorModel::RGB;  m.channels = 3; break;
                case 3: m.model = ColorModel::RGB;  m.channels = 1; break; // palette
                case 4: m.model = ColorModel::Gray; m.channels = 2; m.alpha = true; break;
                case 6: m.model = ColorModel::RGB;  m.channels = 4; m.alpha = true; break;
                default: m.model = ColorModel::Unknown; break;
                }
                ihdr = m.stored_width_px > 0 && m.stored_height_px > 0;
            } else if (std::memcmp(type, "pHYs", 4) == 0 && len >= 9 && chunk[8] == 1) {
                m.ppi_x = (double) be32(chunk) * 0.0254;
                m.ppi_y = (double) be32(chunk + 4) * 0.0254;
            } else if (std::memcmp(type, "iCCP", 4) == 0) {
                size_t k = 0;
                while (k < len && chunk[k])
                    ++k;
                if (k + 2 < len && chunk[k + 1] == 0)
                    inflate_zlib(chunk + k + 2, len - (k + 2), &m.icc);
            } else if (std::memcmp(type, "eXIf", 4) == 0) {
                read_tiff_ifd0(chunk, len, m.orientation, m.ppi_x, m.ppi_y);
            } else if (std::memcmp(type, "IEND", 4) == 0) {
                break;
            }
            p += 12 + (size_t) len;
        }
        if (!ihdr) {
            if (error) *error = "PNG has no image header";
            return false;
        }
        apply_orientation_size(m);
        *meta = std::move(m);
        return true;
    }

    bool decode(const uint8_t *data, size_t n, DecodedImage *out, std::string *error) const override {
        ImageMetadata meta;
        if (!probe(data, n, &meta, error))
            return false;
        if (!decode_rgba(data, n, meta.orientation, out, error))
            return false;
        meta.width_px = out->width;
        meta.height_px = out->height;
        out->meta = std::move(meta);
        return true;
    }
};

/* ---- JPEG --------------------------------------------------------------- */

bool jpeg_sniff(const uint8_t *d, size_t n) {
    return n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
}

bool sof_marker(unsigned marker) {
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

class JpegDecoder : public ImageDecoder {
public:
    const char *id() const override { return "jpeg"; }
    bool sniff(const uint8_t *data, size_t n) const override { return jpeg_sniff(data, n); }

    bool probe(const uint8_t *data, size_t n, ImageMetadata *meta, std::string *error) const override {
        if (!jpeg_sniff(data, n)) {
            if (error) *error = "not a JPEG";
            return false;
        }
        ImageMetadata m;
        m.format = id();
        bool have_size = false;
        double jfif_x = 0, jfif_y = 0;
        bool jfif = false;
        struct IccPart { int seq = 0, count = 0; std::vector<uint8_t> data; };
        std::vector<IccPart> icc_parts;

        size_t p = 2;
        while (p + 1 < n) {
            if (data[p] != 0xFF) {
                if (have_size)
                    break;
                if (error) *error = "JPEG markers are out of order";
                return false;
            }
            while (p < n && data[p] == 0xFF)
                ++p;
            if (p >= n)
                break;
            unsigned marker = data[p++];
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
                continue;
            if (marker == 0xD9 || marker == 0xDA)
                break;                   // EOI, or the scan: headers are done
            if (p + 2 > n)
                break;
            unsigned seg = be16(data + p);
            if (seg < 2 || p + seg > n)
                break;
            const uint8_t *pay = data + p + 2;
            size_t paylen = seg - 2;
            p += seg;

            if (sof_marker(marker) && paylen >= 6) {
                m.bit_depth = pay[0];
                m.stored_height_px = (int) be16(pay + 1);
                m.stored_width_px = (int) be16(pay + 3);
                m.channels = pay[5];
                if (m.channels == 1) m.model = ColorModel::Gray;
                else if (m.channels == 4) m.model = ColorModel::CMYK;
                else m.model = ColorModel::RGB;
                have_size = m.stored_width_px > 0 && m.stored_height_px > 0;
            } else if (marker == 0xE0 && paylen >= 14 &&
                       std::memcmp(pay, "JFIF", 5) == 0) {
                unsigned units = pay[7];
                unsigned xd = be16(pay + 8), yd = be16(pay + 10);
                if (units == 1 || units == 2) {
                    double scale = units == 2 ? 2.54 : 1.0;
                    jfif_x = xd * scale;
                    jfif_y = yd * scale;
                    jfif = true;
                }
            } else if (marker == 0xE1 && paylen >= 14 &&
                       std::memcmp(pay, "Exif\0\0", 6) == 0) {
                double x = 0, y = 0;
                int orient = m.orientation;
                read_tiff_ifd0(pay + 6, paylen - 6, orient, x, y);
                m.orientation = orient;
                if (x > 0) { m.ppi_x = x; m.ppi_y = y > 0 ? y : x; }
            } else if (marker == 0xE2 && paylen > 15 &&
                       std::memcmp(pay, "ICC_PROFILE", 12) == 0 && pay[12] == 0) {
                IccPart part;
                part.seq = pay[13];
                part.count = pay[14];
                part.data.assign(pay + 15, pay + paylen);
                if (part.seq >= 1 && part.count >= 1 && part.seq <= part.count && part.count < 32)
                    icc_parts.push_back(std::move(part));
            }
        }
        if (!have_size) {
            if (error) *error = "JPEG has no image size";
            return false;
        }
        /* A real JFIF density fills in when EXIF didn't. 1×1 is the
         * placeholder a lot of writers emit, not a resolution. */
        if (jfif && jfif_x > 1.5 && m.ppi_x <= 0) {
            m.ppi_x = jfif_x;
            m.ppi_y = jfif_y > 0 ? jfif_y : jfif_x;
        }
        /* nanovgd's reader is the one the display transform agrees with. */
        int nvg_orient = nvg__exifOrientation(data, (int) std::min(n, (size_t) INT32_MAX));
        if (nvg_orient >= 1 && nvg_orient <= 8)
            m.orientation = nvg_orient;

        if (!icc_parts.empty()) {
            int count = icc_parts.front().count;
            bool complete = true;
            std::vector<uint8_t> icc;
            for (int seq = 1; seq <= count; ++seq) {
                const IccPart *part = nullptr;
                for (const IccPart &c : icc_parts)
                    if (c.seq == seq && c.count == count)
                        part = &c;
                if (!part) { complete = false; break; }
                icc.insert(icc.end(), part->data.begin(), part->data.end());
            }
            if (complete)
                m.icc = std::move(icc);
        }
        apply_orientation_size(m);
        *meta = std::move(m);
        return true;
    }

    bool decode(const uint8_t *data, size_t n, DecodedImage *out, std::string *error) const override {
        ImageMetadata meta;
        if (!probe(data, n, &meta, error))
            return false;
        if (!decode_rgba(data, n, meta.orientation, out, error))
            return false;
        meta.width_px = out->width;
        meta.height_px = out->height;
        out->meta = std::move(meta);
        return true;
    }
};

/* Where a stored-image unit point (u right, v up, first row at v = 1)
 * lands in the upright rectangle, as fractions of that rectangle. */
void oriented_uv(int orient, float u, float v, float &ox, float &oy) {
    float sx = u, sy = 1.f - v;          // stored, origin at the top left
    switch (orient) {
    case 2: ox = 1.f - sx; oy = sy; break;
    case 3: ox = 1.f - sx; oy = 1.f - sy; break;
    case 4: ox = sx;       oy = 1.f - sy; break;
    case 5: ox = sy;       oy = sx; break;
    case 6: ox = 1.f - sy; oy = sx; break;
    case 7: ox = 1.f - sy; oy = 1.f - sx; break;
    case 8: ox = sy;       oy = 1.f - sx; break;
    default: ox = sx;      oy = sy; break;
    }
}

uint64_t g_store_gen = 0;

bool read_file(const std::string &path, std::vector<uint8_t> *out, std::string *error) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "couldn't open " + path;
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        if (error) *error = "couldn't read " + path;
        return false;
    }
    long n = std::ftell(f);
    if (n < 0) {
        std::fclose(f);
        if (error) *error = "couldn't read " + path;
        return false;
    }
    std::rewind(f);
    out->resize((size_t) n);
    size_t got = n ? std::fread(out->data(), 1, (size_t) n, f) : 0;
    std::fclose(f);
    if ((long) got != n) {
        if (error) *error = "couldn't read " + path;
        return false;
    }
    return true;
}

} // namespace

const char *color_model_name(ColorModel m) {
    switch (m) {
    case ColorModel::Gray: return "gray";
    case ColorModel::RGB:  return "rgb";
    case ColorModel::CMYK: return "cmyk";
    case ColorModel::Unknown: break;
    }
    return "unknown";
}

ColorModel color_model_from(const std::string &s) {
    if (s == "gray") return ColorModel::Gray;
    if (s == "rgb") return ColorModel::RGB;
    if (s == "cmyk") return ColorModel::CMYK;
    return ColorModel::Unknown;
}

float image_print_points(int px, double ppi) {
    double use = ppi > 0 ? ppi : 72.0;
    return (float) ((double) px * 72.0 / use);
}

void image_pdf_matrix(int orientation, float x, float y, float w, float h, float m[6]) {
    auto at = [&](float u, float v) {
        float ox, oy;
        oriented_uv(orientation, u, v, ox, oy);
        return std::pair<float, float>(x + ox * w, y + oy * h);
    };
    auto o = at(0, 0), ux = at(1, 0), vy = at(0, 1);
    m[0] = ux.first - o.first;
    m[1] = ux.second - o.second;
    m[2] = vy.first - o.first;
    m[3] = vy.second - o.second;
    m[4] = o.first;
    m[5] = o.second;
}

void ImageLoader::add(std::unique_ptr<ImageDecoder> decoder) {
    if (decoder)
        m_decoders.push_back(std::move(decoder));
}

const ImageDecoder *ImageLoader::match(const uint8_t *data, size_t n) const {
    for (const auto &d : m_decoders)
        if (d->sniff(data, n))
            return d.get();
    return nullptr;
}

bool ImageLoader::probe(const uint8_t *data, size_t n, ImageMetadata *meta, std::string *error) const {
    const ImageDecoder *d = match(data, n);
    if (!d) {
        if (error)
            *error = "no picture decoder accepted this file. PNG and JPEG are built in; "
                     "TIFF, HEIC and others register the same way";
        return false;
    }
    return d->probe(data, n, meta, error);
}

bool ImageLoader::decode(const uint8_t *data, size_t n, DecodedImage *out, std::string *error) const {
    const ImageDecoder *d = match(data, n);
    if (!d) {
        if (error)
            *error = "no picture decoder accepted this file. PNG and JPEG are built in; "
                     "TIFF, HEIC and others register the same way";
        return false;
    }
    return d->decode(data, n, out, error);
}

void register_builtin_decoders(ImageLoader &loader) {
    loader.add(std::make_unique<PngDecoder>());
    loader.add(std::make_unique<JpegDecoder>());
}

ImageLoader &image_loader() {
    static ImageLoader loader;
    static bool ready = (register_builtin_decoders(loader), true);
    (void) ready;
    return loader;
}

ImageStore::ImageStore() { bump(); }

ImageStore::ImageStore(ImageStore &&o) noexcept
    : m_entries(std::move(o.m_entries)), m_next_id(o.m_next_id) {
    o.m_entries.clear();
    o.m_next_id = 0;
    bump();
    o.bump();
}

ImageStore &ImageStore::operator=(ImageStore &&o) noexcept {
    if (this != &o) {
        m_entries = std::move(o.m_entries);
        m_next_id = o.m_next_id;
        o.m_entries.clear();
        o.m_next_id = 0;
        bump();
        o.bump();
    }
    return *this;
}

void ImageStore::bump() { m_gen = ++g_store_gen; }

void ImageStore::clear() {
    m_entries.clear();
    m_next_id = 0;
    bump();
}

uint32_t ImageStore::take_id() { return ++m_next_id; }

void ImageStore::note_id(uint32_t id) {
    if (id > m_next_id)
        m_next_id = id;
}

bool ImageStore::add(uint32_t id, std::vector<uint8_t> bytes, ImageMetadata *meta, std::string *error) {
    if (id == 0) {
        if (error) *error = "picture id is missing";
        return false;
    }
    ImageMetadata probed;
    if (!image_loader().probe(bytes.data(), bytes.size(), &probed, error))
        return false;
    note_id(id);
    bool replace = m_entries.count(id) != 0;
    Entry e;
    e.source = std::move(bytes);
    e.meta = probed;
    m_entries[id] = std::move(e);
    if (meta) *meta = probed;
    if (replace)
        bump();                      // a texture cached for this id is stale
    return true;
}

bool ImageStore::load_file(uint32_t id, const std::string &path, ImageMetadata *meta, std::string *error) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes, error))
        return false;
    return add(id, std::move(bytes), meta, error);
}

const ImageStore::Entry *ImageStore::find(uint32_t id) const {
    auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : &it->second;
}

bool ImageStore::display(uint32_t id, int long_edge, const uint8_t **rgba, int *w, int *h) {
    auto it = m_entries.find(id);
    if (it == m_entries.end())
        return false;
    Entry &e = it->second;
    int src_long = std::max(e.meta.width_px, e.meta.height_px);
    if (src_long < 1)
        src_long = 1;
    int want = long_edge < 1 ? src_long : std::min(long_edge, src_long);
    if (!e.proxy.empty() && e.proxy_edge >= want && rgba && w && h) {
        *rgba = e.proxy.data();
        *w = e.proxy_w;
        *h = e.proxy_h;
        return true;
    }
    DecodedImage full;
    std::string err;
    if (!image_loader().decode(e.source.data(), e.source.size(), &full, &err))
        return false;
    int longest = std::max(full.width, full.height);
    int dw = full.width, dh = full.height;
    if (longest > want && longest > 0) {
        dw = std::max(1, (int) ((long long) full.width * want / longest));
        dh = std::max(1, (int) ((long long) full.height * want / longest));
        downsample_rgba(full.rgba.data(), full.width, full.height, dw, dh, e.proxy);
    } else {
        e.proxy = std::move(full.rgba);
    }
    e.proxy_w = dw;
    e.proxy_h = dh;
    e.proxy_edge = std::max(dw, dh);
    if (rgba) *rgba = e.proxy.data();
    if (w) *w = dw;
    if (h) *h = dh;
    return true;
}

} // namespace pagemade
