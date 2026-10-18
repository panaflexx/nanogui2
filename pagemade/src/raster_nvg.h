/*
 * pagemade/raster_nvg.h — draw a page into pixels with NanoVG, off screen.
 *
 * For flattening a PDF page (PdfSettings::flatten): the page's draw list is
 * drawn by the same draw_list() the screen uses, into an OpenGL framebuffer
 * in tiles no larger than the GPU allows, over white, and read back.
 *
 * It needs the window's GL context, so it runs on the UI thread and between
 * frames (from nanogui::async), never inside a draw. On a back-end without
 * OpenGL (Metal) it returns false and the page is written unflattened.
 */
#pragma once

#include "pdf.h"

struct NVGcontext;

namespace pagemade {

/* `list` over the rectangle at (x, y), w_pt by h_pt page points, into
 * `rgba`: width x height pixels, top row first, opaque. `pictures` holds
 * the pixels for each DrawImage's asset; one that's absent draws the
 * missing-picture stand-in. */
bool rasterize_page(NVGcontext *ctx, const DrawList &list,
                    const std::map<uint32_t, PdfRasterPicture> &pictures, float x, float y,
                    float w_pt, float h_pt, int width, int height, std::vector<uint8_t> &rgba);

} // namespace pagemade
