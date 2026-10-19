/*
 * pagemade/render_bench.cpp — time the screen draw path without a window.
 *
 *   pagemade_render_bench file.pagemade [page_width_px] [frames] [first-last]
 *   pagemade_render_bench --decode long_edge picture...
 *
 * Opens and composes a publication, then draws every page the way the
 * composer view does (build_page, then draw_list) into a NanoVG context
 * whose back-end draws nothing. What is timed is the CPU side: the draw
 * lists and NanoVG's tessellation. `page_width_px` is how wide a page is
 * on screen (the zoom); the default, 120, is about Show All Pages. A page
 * range (1-based, "1-2" for a spread) draws only those pages.
 */
#include "default_fonts.h"
#include "image.h"
#include "image_color.h"
#include "docfile.h"
#include "drawlist.h"
#include "render_nvg.h"
#include "system_fonts.h"
#include "vector.h"
#include "composer/hyphenator.h"

#include <nanovg.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pagemade;

namespace {

/* A back-end that keeps nothing. Textures are 256 x 1 so gradient ramps
 * are reused rather than remade every frame. */
int g_next_tex = 0;
int null_create(void *) { return 1; }
int null_create_texture(void *, int, int, int, int, const unsigned char *) { return ++g_next_tex; }
int null_delete_texture(void *, int) { return 1; }
int null_update_texture(void *, int, int, int, int, int, const unsigned char *) { return 1; }
int null_texture_size(void *, int, int *w, int *h) {
    *w = 256;
    *h = 1;
    return 1;
}
void null_viewport(void *, float, float, float) {}
void null_cancel(void *) {}
void null_flush(void *) {}
void null_fill(void *, NVGpaint *, NVGcompositeOperationState, NVGscissor *, float, const float *,
               const NVGpath *, int, int) {}
void null_stroke(void *, NVGpaint *, NVGcompositeOperationState, NVGscissor *, float, float,
                 const NVGpath *, int) {}
void null_triangles(void *, NVGpaint *, NVGcompositeOperationState, NVGscissor *,
                    const NVGvertex *, int, float) {}
void null_delete(void *) {}

NVGcontext *null_context() {
    NVGparams p{};
    p.renderCreate = null_create;
    p.renderCreateTexture = null_create_texture;
    p.renderDeleteTexture = null_delete_texture;
    p.renderUpdateTexture = null_update_texture;
    p.renderGetTextureSize = null_texture_size;
    p.renderViewport = null_viewport;
    p.renderCancel = null_cancel;
    p.renderFlush = null_flush;
    p.renderFill = null_fill;
    p.renderStroke = null_stroke;
    p.renderTriangles = null_triangles;
    p.renderDelete = null_delete;
    p.edgeAntiAlias = 1;
    return nvgCreateInternal(&p);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

/* --decode: make_display (the screen view a picture gets) against a full
 * decode shrunk afterwards, the way it was before scaled decoding. */
int decode_bench(int argc, char **argv) {
    const int edge = argc > 2 ? std::atoi(argv[2]) : 512;
    for (int i = 3; i < argc; ++i) {
        std::vector<uint8_t> bytes;
        std::string err;
        if (!read_file(argv[i], &bytes, &err)) {
            std::printf("%s: %s\n", argv[i], err.c_str());
            continue;
        }
        auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> view;
        int w = 0, h = 0;
        const bool ok = make_display(bytes.data(), bytes.size(), edge, &view, &w, &h, &err);
        const double fast = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        DecodedImage full;
        image_loader().decode(bytes.data(), bytes.size(), &full, &err);
        if (full.meta.model == ColorModel::RGB && !full.meta.icc.empty())
            icc_to_srgb(full.meta.icc, full.rgba.data(), (size_t) full.width * full.height);
        std::vector<uint8_t> ref;
        const int longest = std::max(full.width, full.height);
        int rw = full.width, rh = full.height;
        if (longest > edge) {
            rw = std::max(1, (int) ((long long) full.width * edge / longest));
            rh = std::max(1, (int) ((long long) full.height * edge / longest));
        }
        resample_rgba(full.rgba.data(), full.width, full.height, rw, rh, ref);
        const double slow = ms_since(t0);
        double diff = 0;
        int worst = 0;
        if (ok && rw == w && rh == h)
            for (size_t k = 0; k < view.size(); ++k) {
                const int d = std::abs((int) view[k] - (int) ref[k]);
                diff += d;
                worst = std::max(worst, d);
            }
        /* Where the view's time goes */
        auto ts = std::chrono::steady_clock::now();
        ImageMetadata pm;
        image_loader().probe(bytes.data(), bytes.size(), &pm, &err);
        const double t_probe = ms_since(ts);
        ts = std::chrono::steady_clock::now();
        DecodedImage pv;
        image_loader().decode_preview(bytes.data(), bytes.size(), edge, &pv, &err);
        const double t_preview = ms_since(ts);
        ts = std::chrono::steady_clock::now();
        if (!pm.icc.empty())
            icc_to_srgb(pm.icc, view.data(), (size_t) w * h);
        const double t_icc = ms_since(ts);
        std::printf("  probe %.1f ms, decode_preview %.1f ms (%dx%d), icc on the view %.1f ms\n",
                    t_probe, t_preview, pv.width, pv.height, t_icc);
        std::printf("%s: %dx%d -> %dx%d  view %.1f ms, full decode %.1f ms, mean diff %.2f, max %d%s\n",
                    argv[i], full.width, full.height, w, h, fast, slow,
                    view.empty() ? 0.0 : diff / (double) view.size(), worst,
                    rw == w && rh == h ? "" : " (sizes differ)");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--decode")
        return decode_bench(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s file.pagemade [page_width_px] [frames]\n", argv[0]);
        return 2;
    }
    const float page_px = argc > 2 ? (float) std::atof(argv[2]) : 120.f;
    const int frames = argc > 3 ? std::atoi(argv[3]) : 10;
    size_t first = 0, last = SIZE_MAX;
    if (argc > 4) {
        unsigned a = 1, b = 0;
        if (std::sscanf(argv[4], "%u-%u", &a, &b) >= 1 && a >= 1) {
            first = a - 1;
            last = b >= a ? b - 1 : a - 1;
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    FontLibrary fonts;
    register_default_fonts(fonts);
    register_system_fonts(fonts);
    Hyphenator hyph;
    const std::string hp = std::string(PAGEMADE_DATA_DIR) + "/hyphenation/hyph-en-us";
    hyph.load(hp + ".pat.txt", hp + ".hyp.txt");
    std::printf("fonts: %.0f ms\n", ms_since(t0));

    t0 = std::chrono::steady_clock::now();
    OpenResult r = open_document(argv[1], fonts);
    if (!r.ok) {
        std::fprintf(stderr, "open: %s\n", r.error.c_str());
        return 1;
    }
    for (const DocumentFont &f : r.fonts)
        fonts.add_document_font(f.family, f.style, f.font);
    std::printf("open: %.0f ms, %zu pages\n", ms_since(t0), r.doc.pages.size());

    t0 = std::chrono::steady_clock::now();
    std::vector<Composition> comps;
    for (const StoryEntry &s : r.doc.stories)
        comps.push_back(compose(s.story, r.doc.thread_frames(s), fonts, &hyph));
    std::printf("compose: %.0f ms, %zu stories\n", ms_since(t0), comps.size());

    NVGcontext *ctx = null_context();
    const float scale = page_px / r.doc.setup.width;     // screen px per point
    const float px = 1.f / scale;
    size_t ops = 0, glyph_ops = 0, glyphs = 0;
    for (size_t p = 0; p < r.doc.pages.size(); ++p)
        for (const DrawOp &op : build_page(r.doc, p, comps, nullptr, false, &r.vectors)) {
            ++ops;
            if (const auto *g = std::get_if<DrawGlyphs>(&op)) {
                ++glyph_ops;
                glyphs += g->run->glyphs.size();
            }
        }
    std::printf("draw lists: %zu ops, %zu glyph runs, %zu glyphs\n", ops, glyph_ops, glyphs);

    double build = 0, draw = 0, draw_glyphs = 0, draw_images = 0, draw_paths = 0;
    for (int f = 0; f < frames; ++f) {
        nvgBeginFrame(ctx, 1920, 1080, 1.f);
        nvgScale(ctx, scale, scale);
        for (size_t p = first; p < r.doc.pages.size() && p <= last; ++p) {
            auto tb = std::chrono::steady_clock::now();
            DrawList list = build_page(r.doc, p, comps, nullptr, false, &r.vectors);
            build += ms_since(tb);
            auto td = std::chrono::steady_clock::now();
            draw_list(ctx, list, px, [](const DrawImage &) { return 1; });
            draw += ms_since(td);
            DrawList only_glyphs;
            for (const DrawOp &op : list)
                if (std::holds_alternative<DrawGlyphs>(op))
                    only_glyphs.push_back(op);
            auto tg = std::chrono::steady_clock::now();
            draw_list(ctx, only_glyphs, px, {});
            draw_glyphs += ms_since(tg);
            DrawList only_images, only_paths;
            for (const DrawOp &op : list) {
                if (std::holds_alternative<DrawImage>(op))
                    only_images.push_back(op);
                else if (!std::holds_alternative<DrawGlyphs>(op))
                    only_paths.push_back(op);
            }
            auto ti = std::chrono::steady_clock::now();
            draw_list(ctx, only_images, px, [](const DrawImage &) { return 1; });
            draw_images += ms_since(ti);
            auto tp = std::chrono::steady_clock::now();
            draw_list(ctx, only_paths, px, {});
            draw_paths += ms_since(tp);
        }
        nvgEndFrame(ctx);
        release_unused_ramps(ctx);
    }
    std::printf("page %.0f px wide: per frame: build_page %.1f ms, draw_list %.1f ms "
                "(glyphs %.1f, pictures %.1f, fills and strokes %.1f)\n",
                page_px, build / frames, draw / frames, draw_glyphs / frames, draw_images / frames,
                draw_paths / frames);
    nvgDeleteInternal(ctx);
    return 0;
}
