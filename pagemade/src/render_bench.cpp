/*
 * pagemade/render_bench.cpp — time the screen draw path without a window.
 *
 *   pagemade_render_bench file.pagemade [page_width_px] [frames] [first-last]
 *
 * Opens and composes a publication, then draws every page the way the
 * composer view does (build_page, then draw_list) into a NanoVG context
 * whose back-end draws nothing. What is timed is the CPU side: the draw
 * lists and NanoVG's tessellation. `page_width_px` is how wide a page is
 * on screen (the zoom); the default, 120, is about Show All Pages. A page
 * range (1-based, "1-2" for a spread) draws only those pages.
 */
#include "default_fonts.h"
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

int main(int argc, char **argv) {
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
