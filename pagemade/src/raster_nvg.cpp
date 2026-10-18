/*
 * pagemade/raster_nvg.cpp — see raster_nvg.h.
 */
#include "raster_nvg.h"
#include "render_nvg.h"

#include <nanogui/opengl.h>
#include <nanovg.h>

#include <algorithm>
#include <cstring>

namespace pagemade {

#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)

namespace {

/* A framebuffer the size of one tile: RGBA color and the stencil NanoVG
 * fills with. The previous framebuffer and viewport come back after. */
struct Target {
    GLuint fbo = 0, color = 0, stencil = 0;
    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};

    bool make(int w, int h) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_VIEWPORT, prev_viewport);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        glGenRenderbuffers(1, &stencil);
        glBindRenderbuffer(GL_RENDERBUFFER, stencil);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, stencil);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    ~Target() {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo);
        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
        if (stencil)
            glDeleteRenderbuffers(1, &stencil);
        if (color)
            glDeleteRenderbuffers(1, &color);
        if (fbo)
            glDeleteFramebuffers(1, &fbo);
    }
};

} // namespace

bool rasterize_page(NVGcontext *ctx, const DrawList &list,
                    const std::map<uint32_t, PdfRasterPicture> &pictures, float x, float y,
                    float w_pt, float h_pt, int width, int height, std::vector<uint8_t> &rgba) {
    if (!ctx || width <= 0 || height <= 0 || !(w_pt > 0.f) || !(h_pt > 0.f))
        return false;
    GLint max_rb = 0, max_vp[2] = {0, 0};
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_vp);
    const int tile = std::max(256, std::min({4096, (int) max_rb, (int) max_vp[0], (int) max_vp[1]}));
    const int tw = std::min(tile, width), th = std::min(tile, height);

    Target target;
    if (!target.make(tw, th))
        return false;

    /* The pictures as textures, for every tile, then gone */
    std::map<uint32_t, int> textures;
    for (const auto &kv : pictures)
        if (kv.second.width > 0 && kv.second.height > 0 &&
            kv.second.rgba.size() == (size_t) kv.second.width * kv.second.height * 4)
            textures[kv.first] =
                nvgCreateImageRGBA(ctx, kv.second.width, kv.second.height, 0, kv.second.rgba.data());

    const float sx = (float) width / w_pt, sy = (float) height / h_pt;   // pixels per point
    const float px = 1.f / std::max(sx, sy);
    rgba.assign((size_t) width * height * 4, 255);
    std::vector<uint8_t> row((size_t) tw * th * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int ty = 0; ty < height; ty += th) {
        for (int tx = 0; tx < width; tx += tw) {
            const int cw = std::min(tw, width - tx), ch = std::min(th, height - ty);
            glViewport(0, 0, tw, th);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(1.f, 1.f, 1.f, 1.f);
            glClearStencil(0);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            nvgBeginFrame(ctx, (float) tw, (float) th, 1.f);
            nvgTranslate(ctx, (float) -tx, (float) -ty);
            nvgScale(ctx, sx, sy);
            nvgTranslate(ctx, -x, -y);
            draw_list(ctx, list, px, [&](const DrawImage &im) {
                auto it = textures.find(im.asset);
                return it == textures.end() ? 0 : it->second;
            });
            nvgEndFrame(ctx);
            /* GL rows run bottom up; the tile's top row is its last */
            glReadPixels(0, th - ch, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, row.data());
            for (int r = 0; r < ch; ++r)
                std::memcpy(&rgba[((size_t) (ty + r) * width + tx) * 4],
                            &row[(size_t) (ch - 1 - r) * cw * 4], (size_t) cw * 4);
        }
    }
    for (const auto &kv : textures)
        nvgDeleteImage(ctx, kv.second);
    return true;
}

#else

bool rasterize_page(NVGcontext *, const DrawList &, const std::map<uint32_t, PdfRasterPicture> &,
                    float, float, float, float, int, int, std::vector<uint8_t> &) {
    return false;
}

#endif

} // namespace pagemade
