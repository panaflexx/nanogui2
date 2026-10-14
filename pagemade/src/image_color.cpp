/*
 * pagemade/image_color.cpp — see image_color.h.
 *
 * Both profiles go through the ICC profile connection space (XYZ, D50), so
 * the source colorants and sRGB's colorants are already adapted to the same
 * white; converting is one 3x3 matrix in linear light between the curves.
 */
#include "image_color.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../ext/glfw/deps/stb_image_write.h"

namespace pagemade {

namespace {

uint32_t be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}
unsigned be16(const uint8_t *p) { return ((unsigned) p[0] << 8) | p[1]; }
double s15f16(const uint8_t *p) { return (double) (int32_t) be32(p) / 65536.0; }

/* sRGB's colorants in the PCS (from the IEC 61966-2.1 profile). */
const double kSrgbD50[9] = {
    0.436066, 0.385147, 0.143066,
    0.222488, 0.716873, 0.060608,
    0.013916, 0.097076, 0.714096,
};
const double kD50[3] = {0.9642, 1.0, 0.8249};

double srgb_to_linear(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double linear_to_srgb(double v) {
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

void mul3(const double a[9], const double b[9], double out[9]) {
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    std::memcpy(out, r, sizeof r);
}

bool inv3(const double m[9], double out[9]) {
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                 m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-12)
        return false;
    double r[9] = {
        (m[4] * m[8] - m[5] * m[7]), -(m[1] * m[8] - m[2] * m[7]), (m[1] * m[5] - m[2] * m[4]),
        -(m[3] * m[8] - m[5] * m[6]), (m[0] * m[8] - m[2] * m[6]), -(m[0] * m[5] - m[2] * m[3]),
        (m[3] * m[7] - m[4] * m[6]), -(m[0] * m[7] - m[1] * m[6]), (m[0] * m[4] - m[1] * m[3]),
    };
    for (int i = 0; i < 9; ++i)
        out[i] = r[i] / det;
    return true;
}

/* One parsed matrix/TRC profile: colorants as matrix columns, and each
 * channel's curve sampled at the 256 eight-bit codes. */
struct MatrixProfile {
    double colorants[9];
    float  linear[3][256];
};

const uint8_t *find_tag(const std::vector<uint8_t> &icc, const char *sig, size_t *len) {
    const uint8_t *d = icc.data();
    const size_t n = icc.size();
    uint32_t count = be32(d + 128);
    if (count > 1000 || 132 + (size_t) count * 12 > n)
        return nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = d + 132 + i * 12;
        if (std::memcmp(e, sig, 4) != 0)
            continue;
        uint32_t off = be32(e + 4), size = be32(e + 8);
        if (off >= n || size > n - off || size < 8)
            return nullptr;
        *len = size;
        return d + off;
    }
    return nullptr;
}

bool read_xyz(const std::vector<uint8_t> &icc, const char *sig, double out[3]) {
    size_t len = 0;
    const uint8_t *t = find_tag(icc, sig, &len);
    if (!t || len < 20 || std::memcmp(t, "XYZ ", 4) != 0)
        return false;
    for (int i = 0; i < 3; ++i)
        out[i] = s15f16(t + 8 + i * 4);
    return true;
}

/* curv (identity, gamma, or a table) and para (the five ICC functions). */
bool read_trc(const std::vector<uint8_t> &icc, const char *sig, float out[256]) {
    size_t len = 0;
    const uint8_t *t = find_tag(icc, sig, &len);
    if (!t || len < 12)
        return false;
    if (std::memcmp(t, "curv", 4) == 0) {
        uint32_t count = be32(t + 8);
        if (count > (len - 12) / 2)
            return false;
        if (count == 0) {
            for (int i = 0; i < 256; ++i)
                out[i] = i / 255.f;
            return true;
        }
        if (count == 1) {
            double g = be16(t + 12) / 256.0;
            for (int i = 0; i < 256; ++i)
                out[i] = (float) std::pow(i / 255.0, g);
            return true;
        }
        for (int i = 0; i < 256; ++i) {
            double at = i / 255.0 * (count - 1);
            uint32_t k = (uint32_t) at;
            double f = at - k;
            double a = be16(t + 12 + k * 2) / 65535.0;
            double b = k + 1 < count ? be16(t + 12 + (k + 1) * 2) / 65535.0 : a;
            out[i] = (float) (a + (b - a) * f);
        }
        return true;
    }
    if (std::memcmp(t, "para", 4) == 0) {
        static const int nparams[5] = {1, 3, 4, 5, 7};
        unsigned type = be16(t + 8);
        if (type > 4 || len < 12 + (size_t) nparams[type] * 4)
            return false;
        double p[7] = {1, 1, 0, 0, 0, 0, 0};
        for (int i = 0; i < nparams[type]; ++i)
            p[i] = s15f16(t + 12 + i * 4);
        const double g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
        for (int i = 0; i < 256; ++i) {
            double x = i / 255.0, y;
            switch (type) {
            case 0: y = std::pow(x, g); break;
            case 1: y = (a != 0 && x >= -b / a) ? std::pow(a * x + b, g) : 0; break;
            case 2: y = (a != 0 && x >= -b / a) ? std::pow(a * x + b, g) + c : c; break;
            case 3: y = x >= d ? std::pow(a * x + b, g) : c * x; break;
            default: y = x >= d ? std::pow(a * x + b, g) + e : c * x + f; break;
            }
            out[i] = (float) std::clamp(y, 0.0, 1.0);
        }
        return true;
    }
    return false;
}

bool parse_matrix_profile(const std::vector<uint8_t> &icc, MatrixProfile *out) {
    if (!icc_is_rgb(icc))
        return false;
    double r[3], g[3], b[3];
    if (!read_xyz(icc, "rXYZ", r) || !read_xyz(icc, "gXYZ", g) || !read_xyz(icc, "bXYZ", b))
        return false;
    for (int i = 0; i < 3; ++i) {
        out->colorants[i * 3] = r[i];
        out->colorants[i * 3 + 1] = g[i];
        out->colorants[i * 3 + 2] = b[i];
    }
    return read_trc(icc, "rTRC", out->linear[0]) && read_trc(icc, "gTRC", out->linear[1]) &&
           read_trc(icc, "bTRC", out->linear[2]);
}

/* Source RGB (linear) -> sRGB (linear). False if the profile is sRGB's. */
bool conversion(const MatrixProfile &p, double m[9]) {
    double to_srgb[9];
    if (!inv3(kSrgbD50, to_srgb))
        return false;
    mul3(to_srgb, p.colorants, m);
    bool identity = true;
    for (int i = 0; i < 9 && identity; ++i)
        identity = std::fabs(m[i] - (i % 4 == 0 ? 1.0 : 0.0)) < 0.01;
    for (int c = 0; c < 3 && identity; ++c)
        for (int i = 0; i < 256 && identity; ++i)
            identity = std::fabs(p.linear[c][i] - srgb_to_linear(i / 255.0)) < 0.004;
    return !identity;
}

/* ---- writing a profile ---------------------------------------------------- */

void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back((uint8_t) (x >> 24)); v.push_back((uint8_t) (x >> 16));
    v.push_back((uint8_t) (x >> 8));  v.push_back((uint8_t) x);
}
void put16(std::vector<uint8_t> &v, unsigned x) {
    v.push_back((uint8_t) (x >> 8)); v.push_back((uint8_t) x);
}
void put_sig(std::vector<uint8_t> &v, const char *s) { v.insert(v.end(), s, s + 4); }
void put_s15f16(std::vector<uint8_t> &v, double x) { put32(v, (uint32_t) (int32_t) std::lround(x * 65536.0)); }

std::vector<uint8_t> xyz_tag(const double xyz[3]) {
    std::vector<uint8_t> t;
    put_sig(t, "XYZ ");
    put32(t, 0);
    for (int i = 0; i < 3; ++i)
        put_s15f16(t, xyz[i]);
    return t;
}

/* Linear RGB -> XYZ for xy primaries, scaled so RGB white is `white`. */
bool primaries_matrix(const float r[2], const float g[2], const float b[2], const float w[2],
                      double out[9]) {
    auto xyz = [](const float c[2], double v[3]) {
        v[0] = c[0] / c[1]; v[1] = 1.0; v[2] = (1.0 - c[0] - c[1]) / c[1];
    };
    if (r[1] <= 0 || g[1] <= 0 || b[1] <= 0 || w[1] <= 0)
        return false;
    double R[3], G[3], B[3], W[3];
    xyz(r, R); xyz(g, G); xyz(b, B); xyz(w, W);
    double m[9] = {R[0], G[0], B[0], R[1], G[1], B[1], R[2], G[2], B[2]}, inv[9];
    if (!inv3(m, inv))
        return false;
    double s[3];
    for (int i = 0; i < 3; ++i)
        s[i] = inv[i * 3] * W[0] + inv[i * 3 + 1] * W[1] + inv[i * 3 + 2] * W[2];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 3 + j] = m[i * 3 + j] * s[j];
    return true;
}

/* Bradford adaptation from white `w` (xy) to D50. */
void bradford_to_d50(const float w[2], double out[9]) {
    static const double mb[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367,
                                 0.0389, -0.0685, 1.0296};
    double mbi[9];
    inv3(mb, mbi);
    double src[3] = {w[0] / w[1], 1.0, (1.0 - w[0] - w[1]) / w[1]};
    double cs[3], cd[3];
    for (int i = 0; i < 3; ++i) {
        cs[i] = mb[i * 3] * src[0] + mb[i * 3 + 1] * src[1] + mb[i * 3 + 2] * src[2];
        cd[i] = mb[i * 3] * kD50[0] + mb[i * 3 + 1] * kD50[1] + mb[i * 3 + 2] * kD50[2];
    }
    double d[9] = {cd[0] / cs[0], 0, 0, 0, cd[1] / cs[1], 0, 0, 0, cd[2] / cs[2]};
    double t[9];
    mul3(d, mb, t);
    mul3(mbi, t, out);
}

} // namespace

bool icc_is_rgb(const std::vector<uint8_t> &icc) {
    if (icc.size() < 132)
        return false;
    const uint8_t *d = icc.data();
    return be32(d) <= icc.size() && std::memcmp(d + 16, "RGB ", 4) == 0 &&
           std::memcmp(d + 36, "acsp", 4) == 0;
}

bool icc_needs_conversion(const std::vector<uint8_t> &icc) {
    MatrixProfile p;
    double m[9];
    return parse_matrix_profile(icc, &p) && conversion(p, m);
}

void icc_to_srgb(const std::vector<uint8_t> &icc, uint8_t *rgba, size_t pixels) {
    MatrixProfile p;
    double m[9];
    if (!rgba || !parse_matrix_profile(icc, &p) || !conversion(p, m))
        return;
    static uint8_t encode[4096];
    static const bool ready = [] {
        for (int i = 0; i < 4096; ++i)
            encode[i] = (uint8_t) std::lround(linear_to_srgb(i / 4095.0) * 255.0);
        return true;
    }();
    (void) ready;
    float mf[9];
    for (int i = 0; i < 9; ++i)
        mf[i] = (float) m[i];
    auto out = [&](float v) {
        int k = (int) (std::clamp(v, 0.f, 1.f) * 4095.f + 0.5f);
        return encode[k];
    };
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t *px = rgba + i * 4;
        float r = p.linear[0][px[0]], g = p.linear[1][px[1]], b = p.linear[2][px[2]];
        px[0] = out(mf[0] * r + mf[1] * g + mf[2] * b);
        px[1] = out(mf[3] * r + mf[4] * g + mf[5] * b);
        px[2] = out(mf[6] * r + mf[7] * g + mf[8] * b);
    }
}

static std::vector<uint8_t> build_profile(const float red[2], const float green[2],
                                          const float blue[2], const float white[2],
                                          Transfer transfer, const std::string &description,
                                          bool force);

std::vector<uint8_t> icc_from_primaries(const float red[2], const float green[2],
                                        const float blue[2], const float white[2],
                                        Transfer transfer, const std::string &description) {
    return build_profile(red, green, blue, white, transfer, description, false);
}

const std::vector<uint8_t> &icc_srgb() {
    static const std::vector<uint8_t> profile = [] {
        const float r[2] = {0.64f, 0.33f}, g[2] = {0.30f, 0.60f}, b[2] = {0.15f, 0.06f},
                    w[2] = {0.3127f, 0.3290f};
        return build_profile(r, g, b, w, Transfer::SRGB, "sRGB IEC61966-2.1", true);
    }();
    return profile;
}

bool icc_info(const std::vector<uint8_t> &icc, std::string *space, std::string *description) {
    if (icc.size() < 132 || be32(icc.data()) > icc.size() ||
        std::memcmp(icc.data() + 36, "acsp", 4) != 0)
        return false;
    if (space)
        space->assign((const char *) icc.data() + 16, 4);
    if (description) {
        description->clear();
        size_t len = 0;
        if (const uint8_t *t = find_tag(icc, "desc", &len)) {
            if (std::memcmp(t, "desc", 4) == 0 && len >= 12) {
                /* v2: ASCII count, then the text */
                const uint32_t n = be32(t + 8);
                if (n > 0 && 12 + (size_t) n <= len)
                    description->assign((const char *) t + 12, strnlen((const char *) t + 12, n));
            } else if (std::memcmp(t, "mluc", 4) == 0 && len >= 28) {
                /* v4: the first record, UTF-16BE; keep the ASCII */
                const uint32_t size = be32(t + 20), off = be32(t + 24);
                for (uint32_t i = 0; i + 1 < size && off + i + 1 < len; i += 2) {
                    const unsigned c = be16(t + off + i);
                    if (c >= 32 && c < 127)
                        description->push_back((char) c);
                }
            }
        }
    }
    return true;
}

static std::vector<uint8_t> build_profile(const float red[2], const float green[2],
                                          const float blue[2], const float white[2],
                                          Transfer transfer, const std::string &description,
                                          bool force) {
    double rgb_xyz[9], adapt[9], pcs[9];
    if (!primaries_matrix(red, green, blue, white, rgb_xyz))
        return {};
    bradford_to_d50(white, adapt);
    mul3(adapt, rgb_xyz, pcs);
    if (transfer == Transfer::SRGB && !force) {
        bool same = true;
        for (int i = 0; i < 9 && same; ++i)
            same = std::fabs(pcs[i] - kSrgbD50[i]) < 0.002;
        if (same)
            return {};
    }

    /* Tag data, then the table pointing at it. rTRC, gTRC and bTRC share
     * one curve, as most display profiles do. */
    std::vector<uint8_t> desc;
    put_sig(desc, "desc");
    put32(desc, 0);
    put32(desc, (uint32_t) description.size() + 1);
    desc.insert(desc.end(), description.begin(), description.end());
    desc.push_back(0);
    put32(desc, 0); put32(desc, 0);           // no Unicode
    put16(desc, 0); desc.push_back(0);        // no ScriptCode
    desc.insert(desc.end(), 67, 0);

    const std::string notice = "No copyright, use freely";
    std::vector<uint8_t> cprt;
    put_sig(cprt, "text");
    put32(cprt, 0);
    cprt.insert(cprt.end(), notice.begin(), notice.end());
    cprt.push_back(0);

    std::vector<uint8_t> curv;
    put_sig(curv, "curv");
    put32(curv, 0);
    if (transfer == Transfer::SRGB) {
        const int n = 1024;
        put32(curv, n);
        for (int i = 0; i < n; ++i)
            put16(curv, (unsigned) std::lround(srgb_to_linear(i / (double) (n - 1)) * 65535.0));
    } else {
        const double g = transfer == Transfer::Gamma22 ? 2.2
                       : transfer == Transfer::Gamma28 ? 2.8 : 1.0;
        put32(curv, 1);
        put16(curv, (unsigned) std::lround(g * 256.0));
    }

    double r[3] = {pcs[0], pcs[3], pcs[6]}, g[3] = {pcs[1], pcs[4], pcs[7]},
           b[3] = {pcs[2], pcs[5], pcs[8]};
    struct Tag { const char *sig; std::vector<uint8_t> data; };
    std::vector<Tag> tags = {
        {"desc", desc}, {"cprt", cprt}, {"wtpt", xyz_tag(kD50)},
        {"rXYZ", xyz_tag(r)}, {"gXYZ", xyz_tag(g)}, {"bXYZ", xyz_tag(b)},
        {"rTRC", curv},
    };

    std::vector<uint8_t> body;
    std::vector<uint32_t> offsets;
    const uint32_t table = 128 + 4 + 9 * 12;  // seven tags plus gTRC and bTRC
    for (const Tag &t : tags) {
        while (body.size() % 4)
            body.push_back(0);
        offsets.push_back(table + (uint32_t) body.size());
        body.insert(body.end(), t.data.begin(), t.data.end());
    }
    while (body.size() % 4)
        body.push_back(0);

    std::vector<uint8_t> icc;
    put32(icc, table + (uint32_t) body.size());
    put32(icc, 0);                            // preferred CMM
    put32(icc, 0x02100000);                   // version 2.1
    put_sig(icc, "mntr");
    put_sig(icc, "RGB ");
    put_sig(icc, "XYZ ");
    icc.insert(icc.end(), 12, 0);             // date
    put_sig(icc, "acsp");
    icc.insert(icc.end(), 4 + 4 + 4 + 4 + 8, 0); // platform, flags, maker, model, attributes
    put32(icc, 0);                            // perceptual
    for (double v : kD50)
        put_s15f16(icc, v);
    icc.insert(icc.end(), 128 - icc.size(), 0); // creator, id, reserved

    put32(icc, (uint32_t) tags.size() + 2);
    for (size_t i = 0; i < tags.size(); ++i) {
        put_sig(icc, tags[i].sig);
        put32(icc, offsets[i]);
        put32(icc, (uint32_t) tags[i].data.size());
    }
    for (const char *sig : {"gTRC", "bTRC"}) {
        put_sig(icc, sig);
        put32(icc, offsets.back());
        put32(icc, (uint32_t) tags.back().data.size());
    }
    icc.insert(icc.end(), body.begin(), body.end());
    return icc;
}

bool encode_jpeg(const uint8_t *rgba, int width, int height, int quality, std::string *out) {
    if (!rgba || width <= 0 || height <= 0 || !out)
        return false;
    out->clear();
    return stbi_write_jpg_to_func(
               [](void *ctx, void *data, int size) {
                   static_cast<std::string *>(ctx)->append((const char *) data, (size_t) size);
               },
               out, width, height, 4, rgba, std::clamp(quality, 1, 100)) != 0 &&
           !out->empty();
}

} // namespace pagemade
