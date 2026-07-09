//
// Copyright (c) 2025 Roger Davenport
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#ifndef IMAGESTASH_H
#define IMAGESTASH_H

#define IMGS_INVALID -1

// Define maximum name length for images
#define IMGS_MAX_NAME_LEN 64

// Padding around images in atlas to prevent bleeding (1 pixel)
#define IMGS_PAD 1

enum IMGSerrorCode {
    // Image atlas is full.
    IMGS_ATLAS_FULL = 1,
    // Scratch memory is full.
    IMGS_SCRATCH_FULL = 2,
    // Render create callback failed or is missing.
    IMGS_RENDER_CREATE_FAILED = 3,
};

struct IMGSparams {
    int width, height;
    void* userPtr;
    int (*renderCreate)(void* uptr, int width, int height);
    int (*renderResize)(void* uptr, int width, int height);
    void (*renderUpdate)(void* uptr, int* rect, const unsigned char* data);
    void (*renderDraw)(void* uptr, const float* verts, const float* tcoords, const unsigned int* colors, int nverts);
    void (*renderDelete)(void* uptr);
};
typedef struct IMGSparams IMGSparams;

struct IMGSquad {
    float x0, y0, s0, t0;
    float x1, y1, s1, t1;
};
typedef struct IMGSquad IMGSquad;

struct IMGimage {
    struct IMGcontext* ctx;
    int atlasX, atlasY; // Position in atlas, -1 if not uploaded
    int width, height;  // Pixel dimensions (without padding)
    unsigned char* pixels; // RGBA pixels, NULL if using atlas original
    int ownedPixels;    // 1 if pixels are owned and need free
    int dirty;          // 1 if needs upload to atlas
};
typedef struct IMGimage IMGimage;

typedef struct IMGcontext IMGcontext;

// Constructor and destructor.
IMGcontext* imgsCreateInternal(IMGSparams* params);
void imgsDeleteInternal(IMGcontext* s);

void imgsSetErrorCallback(IMGcontext* s, void (*callback)(void* uptr, int error, int val), void* uptr);
// Returns current atlas size.
void imgsGetAtlasSize(IMGcontext* s, int* width, int* height);
// Expands the atlas size.
int imgsExpandAtlas(IMGcontext* s, int width, int height);
// Resets the whole stash.
int imgsResetAtlas(IMGcontext* stash, int width, int height);

// Add images
int imgsAddFile(IMGcontext* s, const char* name, const char* path, int maxWidth, int maxHeight);
int imgsAddPixels(IMGcontext* s, const char* name, unsigned char* data, int width, int height, int freeData);

// Retrieve image copy for manipulation
IMGimage* imgsGet(IMGcontext* s, const char* name);
void imgsDeleteImage(IMGimage* img);

// Filters (modify IMGimage in place)
void imgsFilterGreyscale(IMGimage* img);
void imgsFilterBlur(IMGimage* img, float radius); // Approximate box blur
void imgsFilterResize(IMGimage* img, int newWidth, int newHeight);

// Draw (adds quads to buffer and flushes if needed)
void imgsDraw(IMGcontext* s, const char* name, float x, float y, float w, float h);
void imgsDrawFiltered(IMGimage* img, float x, float y); // Draws at natural size

// Pull texture changes
const unsigned char* imgsGetTextureData(IMGcontext* stash, int* width, int* height);
int imgsValidateTexture(IMGcontext* s, int* dirty);

// Draws the stash texture for debugging
void imgsDrawDebug(IMGcontext* s, float x, float y);

#endif // IMAGESTASH_H

#ifdef IMAGESTASH_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

//#define STB_IMAGE_IMPLEMENTATION
//#include "stb_image.h"
//#define STB_IMAGE_RESIZE_IMPLEMENTATION
//#include "stb_image_resize2.h"

#define IMGS_NOTUSED(v)  (void)sizeof(v)
#define IMGS_HASH_LUT_SIZE 256
#define IMGS_INITIAL_NODES 256
#define IMGS_VERTEX_COUNT 1024 * 6  // Enough for many quads
#define IMGS_SCRATCH_BUF_SIZE 64000

// Simple FNV-1a hash for strings
static unsigned int imgs__hashstr(const char* str) {
    unsigned int hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 16777619u;
    }
    return hash;
}

// Internal image struct for stash storage
struct IMGSimageImpl {
    char name[IMGS_MAX_NAME_LEN];
    short x, y;
    short width, height;
    int next;  // For hash chain
};

struct IMGSatlasNode {
    short x;
    short y;
    short width;
};

struct IMGSatlas {
    int width, height;
    int nnodes;
    int cnodes;
    struct IMGSatlasNode* nodes;
};

struct IMGcontext {
    IMGSparams params;
    float itw, ith;
    unsigned char* texData;
    int dirtyRect[4];
    struct IMGSatlas* atlas;
    struct IMGSimageImpl* images;
    int nimages;
    int cimages;
    int lut[IMGS_HASH_LUT_SIZE];
    // Vertex buffer for drawing
    float* verts;
    float* tcoords;
    unsigned int* colors;
    int nverts;
    // Scratch
    unsigned char* scratch;
    // Error handling
    void (*handleError)(void* uptr, int error, int val);
    void* errorUptr;
};

// Atlas functions (adapted from fontstash, with fixes for packing and dynamic sizing)
static struct IMGSatlas* imgs__allocAtlas(int w, int h, int cnodes) {
    struct IMGSatlas* atlas = (struct IMGSatlas*)malloc(sizeof(struct IMGSatlas));
    if (atlas == NULL) return NULL;
    memset(atlas, 0, sizeof(struct IMGSatlas));
    atlas->width = w;
    atlas->height = h;
    atlas->cnodes = cnodes;
    atlas->nodes = (struct IMGSatlasNode*)malloc(sizeof(struct IMGSatlasNode) * atlas->cnodes);
    if (atlas->nodes == NULL) {
        free(atlas);
        return NULL;
    }
    memset(atlas->nodes, 0, sizeof(struct IMGSatlasNode) * atlas->cnodes);
    atlas->nnodes = 0;
    atlas->nodes[0].x = 0;
    atlas->nodes[0].y = 0;
    atlas->nodes[0].width = (short)w;
    atlas->nnodes++;
    return atlas;
}

static void imgs__deleteAtlas(struct IMGSatlas* atlas) {
    if (atlas != NULL) {
        if (atlas->nodes != NULL) free(atlas->nodes);
        free(atlas);
    }
}

static void imgs__atlasReset(struct IMGSatlas* atlas, int w, int h) {
    atlas->width = w;
    atlas->height = h;
    atlas->nnodes = 0;
    atlas->nodes[0].x = 0;
    atlas->nodes[0].y = 0;
    atlas->nodes[0].width = (short)w;
    atlas->nnodes++;
}

static void imgs__atlasExpand(struct IMGSatlas* atlas, int w, int h) {
    atlas->width = w;
    atlas->height = h;
}

static int imgs__atlasAddRect(struct IMGSatlas* atlas, int rw, int rh, int* rx, int* ry) {
    int besth = INT_MAX, bestw = INT_MAX, besti = -1;
    int i;

    // Validate input dimensions
    if (rw <= 0 || rh <= 0 || rw > atlas->width || rh > atlas->height) {
        // Image too large for current atlas
        return 0;
    }

    // Find best shelf: minimize new max height (y + rh), then tightest width fit
    for (i = 0; i < atlas->nnodes; ++i) {
        int y = atlas->nodes[i].y;
        if (atlas->nodes[i].width >= rw && y + rh <= atlas->height) {
            int thisw = atlas->nodes[i].width;
            int thish = y + rh;
            if (thish < besth || (thish == besth && thisw < bestw)) {
                besti = i;
                bestw = thisw;
                besth = thish;
            }
        }
    }

    if (besti == -1) return 0;

    *rx = atlas->nodes[besti].x;
    *ry = atlas->nodes[besti].y;

    // Split node if remainder > 0
    int old_width = atlas->nodes[besti].width;
    if (old_width - rw > 0) {
        // Realloc if needed
        if (atlas->nnodes + 1 > atlas->cnodes) {
            int old_c = atlas->cnodes;
            atlas->cnodes = atlas->cnodes * 2;
            atlas->nodes = (struct IMGSatlasNode*)realloc(atlas->nodes, sizeof(struct IMGSatlasNode) * atlas->cnodes);
            if (atlas->nodes == NULL) return 0;
            // Zero new memory
            memset(atlas->nodes + old_c, 0, sizeof(struct IMGSatlasNode) * (atlas->cnodes - old_c));
        }

        for (i = atlas->nnodes; i > besti; --i)
            atlas->nodes[i] = atlas->nodes[i-1];

        atlas->nodes[besti+1].x = atlas->nodes[besti].x + rw;
        atlas->nodes[besti+1].y = atlas->nodes[besti].y;
        atlas->nodes[besti+1].width = old_width - rw;
        atlas->nnodes++;
    }

    atlas->nodes[besti].width = rw;
    atlas->nodes[besti].y += rh;

    return 1;
}

static void imgs__addWhiteRect(IMGcontext* stash, int w, int h) {
    int x, y, gx, gy;
    unsigned char* dst;
    if (imgs__atlasAddRect(stash->atlas, w, h, &gx, &gy) == 0) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, IMGS_ATLAS_FULL, 0);
        return;
    }

    // Rasterize white rect directly to texData
    dst = &stash->texData[(gx + IMGS_PAD) + (gy + IMGS_PAD) * stash->params.width * 4];
    for (y = 0; y < h - 2 * IMGS_PAD; y++) {
        for (x = 0; x < w - 2 * IMGS_PAD; x++) {
            dst[x * 4 + 0] = 255; // R
            dst[x * 4 + 1] = 255; // G
            dst[x * 4 + 2] = 255; // B
            dst[x * 4 + 3] = 255; // A
        }
        dst += stash->params.width * 4;
    }

    stash->dirtyRect[0] = (int)fminf(stash->dirtyRect[0], (float)gx);
    stash->dirtyRect[1] = (int)fminf(stash->dirtyRect[1], (float)gy);
    stash->dirtyRect[2] = (int)fmaxf(stash->dirtyRect[2], (float)gx + w);
    stash->dirtyRect[3] = (int)fmaxf(stash->dirtyRect[3], (float)gy + h);

    // Update texture if renderUpdate is available
    if (stash->params.renderUpdate) {
        int rect[4] = {gx, gy, gx + w, gy + h};
        stash->params.renderUpdate(stash->params.userPtr, rect, stash->texData);
    }
}

static void imgs__flush(IMGcontext* stash) {
    if (stash->nverts > 0) {
        if (stash->params.renderDraw)
            stash->params.renderDraw(stash->params.userPtr, stash->verts, stash->tcoords, stash->colors, stash->nverts);
        stash->nverts = 0;
    }
}

static void imgs__vertex(IMGcontext* stash, float x, float y, float u, float v, unsigned int color) {
    if (stash->nverts + 1 > IMGS_VERTEX_COUNT) imgs__flush(stash);
    stash->verts[stash->nverts * 2 + 0] = x;
    stash->verts[stash->nverts * 2 + 1] = y;
    stash->tcoords[stash->nverts * 2 + 0] = u;
    stash->tcoords[stash->nverts * 2 + 1] = v;
    stash->colors[stash->nverts] = color;
    stash->nverts++;
}

static int imgs__getImageIndex(IMGcontext* stash, const char* name) {
    unsigned int hash = imgs__hashstr(name) % IMGS_HASH_LUT_SIZE;
    int i = stash->lut[hash];
    while (i != -1) {
        if (strcmp(stash->images[i].name, name) == 0)
            return i;
        i = stash->images[i].next;
    }
    return -1;
}

static int imgs__allocImage(IMGcontext* stash) {
    if (stash->nimages + 1 > stash->cimages) {
        stash->cimages = stash->cimages ? stash->cimages * 2 : 8;
        stash->images = (struct IMGSimageImpl*)realloc(stash->images, sizeof(struct IMGSimageImpl) * stash->cimages);
        if (stash->images == NULL) {
            if (stash->handleError)
                stash->handleError(stash->errorUptr, IMGS_SCRATCH_FULL, 0);
            return -1;
        }
    }
    return stash->nimages++;
}

static int imgs__addImage(IMGcontext* stash, const char* name, int width, int height, unsigned char* data, int freeData) {
    int idx = imgs__allocImage(stash);
    if (idx == -1) {
        if (freeData) free(data);
        return 0;
    }

    struct IMGSimageImpl* img = &stash->images[idx];
    strncpy(img->name, name, IMGS_MAX_NAME_LEN);
    img->name[IMGS_MAX_NAME_LEN - 1] = '\0';
    img->width = (short)width;
    img->height = (short)height;
    img->next = -1;

    // Insert to hash
    unsigned int hash = imgs__hashstr(name) % IMGS_HASH_LUT_SIZE;
    img->next = stash->lut[hash];
    stash->lut[hash] = idx;

    // Find space in atlas
    int gw = width + 2 * IMGS_PAD;
    int gh = height + 2 * IMGS_PAD;
    int gx = 0, gy = 0;
    while (imgs__atlasAddRect(stash->atlas, gw, gh, &gx, &gy) == 0) {
        // Expand atlas
        int nw = stash->params.width * 2;
        int nh = stash->params.height * 2;
        if (nw > 4096) nw = 4096;  // Cap
        if (nh > 4096) nh = 4096;
        if (nw == stash->params.width && nh == stash->params.height) {
            if (freeData) free(data);
            if (stash->handleError)
                stash->handleError(stash->errorUptr, IMGS_ATLAS_FULL, 0);
            return 0;
        }
        if (!imgsExpandAtlas(stash, nw, nh)) {
            if (freeData) free(data);
            if (stash->handleError)
                stash->handleError(stash->errorUptr, IMGS_ATLAS_FULL, 0);
            return 0;
        }
    }
    img->x = (short)gx;
    img->y = (short)gy;

    // Copy to atlas with clamp borders
    unsigned char* dst = stash->texData + (gy + IMGS_PAD) * stash->params.width * 4 + (gx + IMGS_PAD) * 4;
    unsigned char* src = data;
    int x, y;

    // Inner pixels
    for (y = 0; y < height; ++y) {
        memcpy(dst, src, width * 4);
        dst += stash->params.width * 4;
        src += width * 4;
    }

    // Horizontal borders
    dst = stash->texData + gy * stash->params.width * 4 + gx * 4;
    for (x = 0; x < gw; ++x) {
        memcpy(dst + x * 4, data, 4);  // Top border: first row
        memcpy(dst + (gh - 1) * stash->params.width * 4 + x * 4, data + (height - 1) * width * 4, 4);  // Bottom
    }

    // Vertical borders
    for (y = 0; y < gh; ++y) {
        memcpy(dst + y * stash->params.width * 4, dst + y * stash->params.width * 4 + IMGS_PAD * 4, 4);  // Left
        memcpy(dst + y * stash->params.width * 4 + (gw - 1) * 4, dst + y * stash->params.width * 4 + (IMGS_PAD + width - 1) * 4, 4);  // Right
    }

    // Update texture if renderUpdate is available
    if (stash->params.renderUpdate) {
        int rect[4] = {gx, gy, gx + gw, gy + gh};
        stash->params.renderUpdate(stash->params.userPtr, rect, stash->texData);
    }

    stash->dirtyRect[0] = (int)fminf(stash->dirtyRect[0], (float)gx);
    stash->dirtyRect[1] = (int)fminf(stash->dirtyRect[1], (float)gy);
    stash->dirtyRect[2] = (int)fmaxf(stash->dirtyRect[2], (float)gx + gw);
    stash->dirtyRect[3] = (int)fmaxf(stash->dirtyRect[3], (float)gy + gh);

    if (freeData) free(data);
    return 1;
}

int imgsAddFile(IMGcontext* s, const char* name, const char* path, int maxWidth, int maxHeight) {
    int w, h, n;
    unsigned char* data = stbi_load(path, &w, &h, &n, 4);  // Force RGBA
    if (!data) {
        if (s->handleError)
            s->handleError(s->errorUptr, IMGS_SCRATCH_FULL, 0);
        return 0;
    }
    int bytes_per_pixel = 4; // RGBA

    // Resize if needed
    if (w > maxWidth || h > maxHeight) {
        float scale = fminf((float)maxWidth / w, (float)maxHeight / h);
        int nw = (int)(w * scale);
        int nh = (int)(h * scale);
        unsigned char* resized = (unsigned char*)malloc(nw * nh * 4);
        if (!resized) {
            stbi_image_free(data);
            if (s->handleError)
                s->handleError(s->errorUptr, IMGS_SCRATCH_FULL, 0);
            return 0;
        }
        if (!stbir_resize_uint8_linear(data, w, h, 0, resized, nw, nh, 0, bytes_per_pixel)) {
            free(resized);
            stbi_image_free(data);
            if (s->handleError)
                s->handleError(s->errorUptr, IMGS_SCRATCH_FULL, 0);
            return 0;
        }
        stbi_image_free(data);
        data = resized;
        w = nw;
        h = nh;
    }
	printf("imgsAddFile: added %s\n", name);

    return imgs__addImage(s, name, w, h, data, 1);
}

int imgsAddPixels(IMGcontext* s, const char* name, unsigned char* data, int width, int height, int freeData) {
    return imgs__addImage(s, name, width, height, data, freeData);
}

IMGimage* imgsGet(IMGcontext* s, const char* name) {
    int idx = imgs__getImageIndex(s, name);
    if (idx == -1) return NULL;

    struct IMGSimageImpl* impl = &s->images[idx];
    IMGimage* img = (IMGimage*)malloc(sizeof(IMGimage));
    if (!img) {
        if (s->handleError)
            s->handleError(s->errorUptr, IMGS_SCRATCH_FULL, 0);
        return NULL;
    }

    img->ctx = s;
    img->atlasX = impl->x;
    img->atlasY = impl->y;
    img->width = impl->width;
    img->height = impl->height;
    img->pixels = NULL;
    img->ownedPixels = 0;
    img->dirty = 0;

    return img;
}

void imgsDeleteImage(IMGimage* img) {
    if (img) {
        if (img->ownedPixels && img->pixels) free(img->pixels);
        free(img);
    }
}

static void imgs__ensurePixels(IMGimage* img) {
    if (img->pixels) return;

    img->pixels = (unsigned char*)malloc(img->width * img->height * 4);
    if (!img->pixels) {
        if (img->ctx->handleError)
            img->ctx->handleError(img->ctx->errorUptr, IMGS_SCRATCH_FULL, 0);
        return;
    }

    // Copy from atlas (inner area)
    unsigned char* src = img->ctx->texData + (img->atlasY + IMGS_PAD) * img->ctx->params.width * 4 + (img->atlasX + IMGS_PAD) * 4;
    unsigned char* dst = img->pixels;
    for (int y = 0; y < img->height; ++y) {
        memcpy(dst, src, img->width * 4);
        src += img->ctx->params.width * 4;
        dst += img->width * 4;
    }
    img->ownedPixels = 1;
    img->dirty = 1;  // Mark dirty since we intend to modify
}

void imgsFilterGreyscale(IMGimage* img) {
    if (!img) return;
    imgs__ensurePixels(img);
    if (!img->pixels) return;

    unsigned char* p = img->pixels;
    for (int i = 0; i < img->width * img->height; ++i) {
        unsigned char grey = (unsigned char)(0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]);
        p[0] = p[1] = p[2] = grey;
        p += 4;
    }
    img->dirty = 1;
}

void imgsFilterBlur(IMGimage* img, float radius) {
    if (!img || radius <= 0) return;
    imgs__ensurePixels(img);
    if (!img->pixels) return;

    int kernelSize = (int)(2 * radius) + 1;
    if (kernelSize < 3) kernelSize = 3;  // Min box

    unsigned char* temp = (unsigned char*)malloc(img->width * img->height * 4);
    if (!temp) {
        if (img->ctx->handleError)
            img->ctx->handleError(img->ctx->errorUptr, IMGS_SCRATCH_FULL, 0);
        return;
    }

    // Horizontal pass
    for (int y = 0; y < img->height; ++y) {
        for (int x = 0; x < img->width; ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            int count = 0;
            for (int k = -kernelSize / 2; k <= kernelSize / 2; ++k) {
                int nx = x + k;
                if (nx < 0) nx = 0;
                if (nx >= img->width) nx = img->width - 1;
                unsigned char* p = img->pixels + (y * img->width + nx) * 4;
                r += p[0]; g += p[1]; b += p[2]; a += p[3];
                ++count;
            }
            unsigned char* dst = temp + (y * img->width + x) * 4;
            dst[0] = (unsigned char)(r / count);
            dst[1] = (unsigned char)(g / count);
            dst[2] = (unsigned char)(b / count);
            dst[3] = (unsigned char)(a / count);
        }
    }

    // Vertical pass
    for (int x = 0; x < img->width; ++x) {
        for (int y = 0; y < img->height; ++y) {
            int r = 0, g = 0, b = 0, a = 0;
            int count = 0;
            for (int k = -kernelSize / 2; k <= kernelSize / 2; ++k) {
                int ny = y + k;
                if (ny < 0) ny = 0;
                if (ny >= img->height) ny = img->height - 1;
                unsigned char* p = temp + (ny * img->width + x) * 4;
                r += p[0]; g += p[1]; b += p[2]; a += p[3];
                ++count;
            }
            unsigned char* dst = img->pixels + (y * img->width + x) * 4;
            dst[0] = (unsigned char)(r / count);
            dst[1] = (unsigned char)(g / count);
            dst[2] = (unsigned char)(b / count);
            dst[3] = (unsigned char)(a / count);
        }
    }

    free(temp);
    img->dirty = 1;
}

void imgsFilterResize(IMGimage* img, int newWidth, int newHeight) {
    if (!img || newWidth <= 0 || newHeight <= 0) return;
    imgs__ensurePixels(img);
    if (!img->pixels) return;

    unsigned char* newPixels = (unsigned char*)malloc(newWidth * newHeight * 4);
    if (!newPixels) {
        if (img->ctx->handleError)
            img->ctx->handleError(img->ctx->errorUptr, IMGS_SCRATCH_FULL, 0);
        return;
    }
    int bytes_per_pixel = 4; // RGBA

    if (!stbir_resize_uint8_linear(img->pixels, img->width, img->height, 0, newPixels, newWidth, newHeight, 0, bytes_per_pixel)) {
        free(newPixels);
        if (img->ctx->handleError)
            img->ctx->handleError(img->ctx->errorUptr, IMGS_SCRATCH_FULL, 0);
        return;
    }

    free(img->pixels);
    img->pixels = newPixels;
    img->width = newWidth;
    img->height = newHeight;
    img->atlasX = -1;  // Force re-allocation since size changed
    img->atlasY = -1;
    img->dirty = 1;
}

static void imgs__uploadToAtlas(IMGimage* img) {
    if (!img->dirty || !img->pixels) return;

    int gx, gy;
    int gw = img->width + 2 * IMGS_PAD;
    int gh = img->height + 2 * IMGS_PAD;
    IMGcontext* s = img->ctx;

    if (img->atlasX < 0) {
        // Allocate new space
        while (imgs__atlasAddRect(s->atlas, gw, gh, &gx, &gy) == 0) {
            int nw = s->params.width * 2;
            int nh = s->params.height * 2;
            if (nw > 4096) nw = 4096;
            if (nh > 4096) nh = 4096;
            if (!imgsExpandAtlas(s, nw, nh)) {
                if (s->handleError)
                    s->handleError(s->errorUptr, IMGS_ATLAS_FULL, 0);
                return;
            }
        }
        img->atlasX = gx;
        img->atlasY = gy;
    } else {
        // Update in place (assume size unchanged)
        gx = img->atlasX;
        gy = img->atlasY;
    }

    // Copy with borders (clamp)
    unsigned char* dst = s->texData + (gy + IMGS_PAD) * s->params.width * 4 + (gx + IMGS_PAD) * 4;
    unsigned char* src = img->pixels;
    int x, y;

    for (y = 0; y < img->height; ++y) {
        memcpy(dst, src, img->width * 4);
        dst += s->params.width * 4;
        src += img->width * 4;
    }

    dst = s->texData + gy * s->params.width * 4 + gx * 4;
    for (x = 0; x < gw; ++x) {
        memcpy(dst + x * 4, img->pixels, 4);  // Top
        memcpy(dst + (gh - 1) * s->params.width * 4 + x * 4, img->pixels + (img->height - 1) * img->width * 4, 4);  // Bottom
    }

    for (y = 0; y < gh; ++y) {
        memcpy(dst + y * s->params.width * 4, dst + y * s->params.width * 4 + IMGS_PAD * 4, 4);  // Left
        memcpy(dst + y * s->params.width * 4 + (gw - 1) * 4, dst + y * s->params.width * 4 + (IMGS_PAD + img->width - 1) * 4, 4);  // Right
    }

    s->dirtyRect[0] = (int)fminf(s->dirtyRect[0], (float)gx);
    s->dirtyRect[1] = (int)fminf(s->dirtyRect[1], (float)gy);
    s->dirtyRect[2] = (int)fmaxf(s->dirtyRect[2], (float)gx + gw);
    s->dirtyRect[3] = (int)fmaxf(s->dirtyRect[3], (float)gy + gh);

    // Update texture if renderUpdate is available
    if (s->params.renderUpdate) {
        int rect[4] = {gx, gy, gx + gw, gy + gh};
        s->params.renderUpdate(s->params.userPtr, rect, s->texData);
    }

    img->dirty = 0;
}

static void imgs__drawQuad(IMGcontext* s, float dx, float dy, float dw, float dh, float u0, float v0, float u1, float v1) {
    unsigned int col = 0xffffffff;

    if (s->nverts + 6 > IMGS_VERTEX_COUNT) imgs__flush(s);

    imgs__vertex(s, dx, dy, u0, v0, col);
    imgs__vertex(s, dx + dw, dy, u1, v0, col);
    imgs__vertex(s, dx + dw, dy + dh, u1, v1, col);

    imgs__vertex(s, dx, dy, u0, v0, col);
    imgs__vertex(s, dx + dw, dy + dh, u1, v1, col);
    imgs__vertex(s, dx, dy + dh, u0, v1, col);
}

void imgsDraw(IMGcontext* s, const char* name, float x, float y, float w, float h) {
    int idx = imgs__getImageIndex(s, name);
    if (idx == -1) return;

    struct IMGSimageImpl* impl = &s->images[idx];
    float u0 = (impl->x + IMGS_PAD) * s->itw;
    float v0 = (impl->y + IMGS_PAD) * s->ith;
    float u1 = (impl->x + IMGS_PAD + impl->width) * s->itw;
    float v1 = (impl->y + IMGS_PAD + impl->height) * s->ith;

    imgs__drawQuad(s, x, y, w, h, u0, v0, u1, v1);
}

void imgsDrawFiltered(IMGimage* img, float x, float y) {
    if (!img) return;

    imgs__uploadToAtlas(img);

    if (img->atlasX < 0) return;

    float u0 = (img->atlasX + IMGS_PAD) * img->ctx->itw;
    float v0 = (img->atlasY + IMGS_PAD) * img->ctx->ith;
    float u1 = (img->atlasX + IMGS_PAD + img->width) * img->ctx->itw;
    float v1 = (img->atlasY + IMGS_PAD + img->height) * img->ctx->ith;

    imgs__drawQuad(img->ctx, x, y, (float)img->width, (float)img->height, u0, v0, u1, v1);
}

IMGcontext* imgsCreateInternal(IMGSparams* params) {
    IMGcontext* stash = (IMGcontext*)malloc(sizeof(IMGcontext));
    if (stash == NULL) return NULL;
    memset(stash, 0, sizeof(IMGcontext));

    stash->params = *params;
    stash->itw = 1.0f / stash->params.width;
    stash->ith = 1.0f / stash->params.height;

    stash->texData = (unsigned char*)malloc(stash->params.width * stash->params.height * 4);
    if (stash->texData == NULL) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, IMGS_SCRATCH_FULL, 0);
        goto error;
    }
    memset(stash->texData, 0, stash->params.width * stash->params.height * 4);

    stash->dirtyRect[0] = stash->params.width;
    stash->dirtyRect[1] = stash->params.height;
    stash->dirtyRect[2] = 0;
    stash->dirtyRect[3] = 0;

    stash->atlas = imgs__allocAtlas(stash->params.width, stash->params.height, IMGS_INITIAL_NODES);
    if (stash->atlas == NULL) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, IMGS_SCRATCH_FULL, 0);
        goto error;
    }

    stash->images = NULL;
    stash->nimages = 0;
    stash->cimages = 0;
    memset(stash->lut, -1, sizeof(stash->lut));

    stash->verts = (float*)malloc(sizeof(float) * IMGS_VERTEX_COUNT * 2);
    stash->tcoords = (float*)malloc(sizeof(float) * IMGS_VERTEX_COUNT * 2);
    stash->colors = (unsigned int*)malloc(sizeof(unsigned int) * IMGS_VERTEX_COUNT);
    if (stash->verts == NULL || stash->tcoords == NULL || stash->colors == NULL) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, IMGS_SCRATCH_FULL, 0);
        goto error;
    }
    stash->nverts = 0;

    stash->scratch = (unsigned char*)malloc(IMGS_SCRATCH_BUF_SIZE);
    if (stash->scratch == NULL) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, IMGS_SCRATCH_FULL, 0);
        goto error;
    }

    // Only call renderCreate if provided
    if (stash->params.renderCreate) {
        if (stash->params.renderCreate(stash->params.userPtr, stash->params.width, stash->params.height) == 0) {
            if (stash->handleError)
                stash->handleError(stash->errorUptr, IMGS_RENDER_CREATE_FAILED, 0);
            goto error;
        }
    }

    // Add white rect for debug
    imgs__addWhiteRect(stash, 2, 2);

    return stash;

error:
    imgsDeleteInternal(stash);
    return NULL;
}

void imgsDeleteInternal(IMGcontext* stash) {
    if (stash == NULL) return;

    if (stash->params.renderDelete)
        stash->params.renderDelete(stash->params.userPtr);

    if (stash->atlas) imgs__deleteAtlas(stash->atlas);
    if (stash->images) free(stash->images);
    if (stash->texData) free(stash->texData);
    if (stash->verts) free(stash->verts);
    if (stash->tcoords) free(stash->tcoords);
    if (stash->colors) free(stash->colors);
    if (stash->scratch) free(stash->scratch);
    free(stash);
}

void imgsSetErrorCallback(IMGcontext* stash, void (*callback)(void* uptr, int error, int val), void* uptr) {
    if (stash == NULL) return;
    stash->handleError = callback;
    stash->errorUptr = uptr;
}

void imgsGetAtlasSize(IMGcontext* stash, int* width, int* height) {
    if (stash == NULL) return;
    *width = stash->params.width;
    *height = stash->params.height;
}

int imgsExpandAtlas(IMGcontext* stash, int width, int height) {
    int i, maxy = 0;
    unsigned char* data = NULL;
    if (stash == NULL) return 0;
	printf("imgsExpandAtlas: new size %dx%d\n", width, height);

    imgs__flush(stash);

    int old_w = stash->params.width;
    int old_h = stash->params.height;

    width = (int)fmax(width, old_w);
    height = (int)fmax(height, old_h);

    if (width == old_w && height == old_h)
        return 1;

    if (stash->params.renderResize) {
        if (stash->params.renderResize(stash->params.userPtr, width, height) == 0) {
            if (stash->handleError)
                stash->handleError(stash->errorUptr, IMGS_RENDER_CREATE_FAILED, 0);
            return 0;
        }
    }

    data = (unsigned char*)malloc(width * height * 4);
    if (data == NULL) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, IMGS_SCRATCH_FULL, 0);
        return 0;
    }

    for (i = 0; i < old_h; i++) {
        unsigned char* dst = &data[i * width * 4];
        unsigned char* src = &stash->texData[i * old_w * 4];
        memcpy(dst, src, old_w * 4);
        if (width > old_w)
            memset(dst + old_w * 4, 0, (width - old_w) * 4);
    }
    if (height > old_h)
        memset(&data[old_h * width * 4], 0, (height - old_h) * width * 4);

    free(stash->texData);
    stash->texData = data;

    imgs__atlasExpand(stash->atlas, width, height);

    // Add new free node if width expanded
    if (width > old_w) {
        if (stash->atlas->nnodes + 1 > stash->atlas->cnodes) {
            int old_c = stash->atlas->cnodes;
            stash->atlas->cnodes *= 2;
            stash->atlas->nodes = (struct IMGSatlasNode*)realloc(stash->atlas->nodes, sizeof(struct IMGSatlasNode) * stash->atlas->cnodes);
            if (stash->atlas->nodes == NULL) return 0;
            memset(stash->atlas->nodes + old_c, 0, sizeof(struct IMGSatlasNode) * (stash->atlas->cnodes - old_c));
        }
        stash->atlas->nodes[stash->atlas->nnodes].x = (short)old_w;
        stash->atlas->nodes[stash->atlas->nnodes].y = 0;
        stash->atlas->nodes[stash->atlas->nnodes].width = (short)(width - old_w);
        stash->atlas->nnodes++;
    }

    for (i = 0; i < stash->atlas->nnodes; i++)
        maxy = (int)fmax(maxy, stash->atlas->nodes[i].y);
    stash->dirtyRect[0] = 0;
    stash->dirtyRect[1] = 0;
    stash->dirtyRect[2] = old_w;
    stash->dirtyRect[3] = maxy;

    stash->params.width = width;
    stash->params.height = height;
    stash->itw = 1.0f / stash->params.width;
    stash->ith = 1.0f / stash->params.height;

    // Update texture if renderUpdate is available
    if (stash->params.renderUpdate) {
        int rect[4] = {0, 0, width, maxy};
        stash->params.renderUpdate(stash->params.userPtr, rect, stash->texData);
    }

    return 1;
}

int imgsResetAtlas(IMGcontext* stash, int width, int height) {
    if (stash == NULL) return 0;

    imgs__flush(stash);

    if (stash->params.renderResize) {
        if (stash->params.renderResize(stash->params.userPtr, width, height) == 0) {
            if (stash->handleError)
                stash->handleError(stash->errorUptr, IMGS_RENDER_CREATE_FAILED, 0);
            return 0;
        }
    }

    imgs__atlasReset(stash->atlas, width, height);

    stash->texData = (unsigned char*)realloc(stash->texData, width * height * 4);
    if (stash->texData == NULL) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, IMGS_SCRATCH_FULL, 0);
        return 0;
    }
    memset(stash->texData, 0, width * height * 4);

    stash->dirtyRect[0] = width;
    stash->dirtyRect[1] = height;
    stash->dirtyRect[2] = 0;
    stash->dirtyRect[3] = 0;

    stash->nimages = 0;
    memset(stash->lut, -1, sizeof(stash->lut));

    stash->params.width = width;
    stash->params.height = height;
    stash->itw = 1.0f / stash->params.width;
    stash->ith = 1.0f / stash->params.height;

    imgs__addWhiteRect(stash, 2, 2);

    return 1;
}

const unsigned char* imgsGetTextureData(IMGcontext* stash, int* width, int* height) {
    if (width != NULL) *width = stash->params.width;
    if (height != NULL) *height = stash->params.height;
    return stash->texData;
}

int imgsValidateTexture(IMGcontext* s, int* dirty) {
    if (s->dirtyRect[0] < s->dirtyRect[2] && s->dirtyRect[1] < s->dirtyRect[3]) {
        dirty[0] = s->dirtyRect[0];
        dirty[1] = s->dirtyRect[1];
        dirty[2] = s->dirtyRect[2];
        dirty[3] = s->dirtyRect[3];
        s->dirtyRect[0] = s->params.width;
        s->dirtyRect[1] = s->params.height;
        s->dirtyRect[2] = 0;
        s->dirtyRect[3] = 0;
        // Update texture if renderUpdate is available
        if (s->params.renderUpdate) {
            s->params.renderUpdate(s->params.userPtr, dirty, s->texData);
        }
        return 1;
    }
    return 0;
}

void imgsDrawDebug(IMGcontext* s, float x, float y) {
    int i;
    int w = s->params.width;
    int h = s->params.height;
    float u = w == 0 ? 0 : (1.0f / w);
    float v = h == 0 ? 0 : (1.0f / h);

    if (s->nverts + 6 > IMGS_VERTEX_COUNT) imgs__flush(s);

    // Background
    imgs__vertex(s, x + 0, y + 0, u, v, 0x0fffffff);
    imgs__vertex(s, x + w, y + h, u, v, 0x0fffffff);
    imgs__vertex(s, x + w, y + 0, u, v, 0x0fffffff);

    imgs__vertex(s, x + 0, y + 0, u, v, 0x0fffffff);
    imgs__vertex(s, x + 0, y + h, u, v, 0x0fffffff);
    imgs__vertex(s, x + w, y + h, u, v, 0x0fffffff);

    // Texture
    imgs__vertex(s, x + 0, y + 0, 0, 0, 0xffffffff);
    imgs__vertex(s, x + w, y + h, 1, 1, 0xffffffff);
    imgs__vertex(s, x + w, y + 0, 1, 0, 0xffffffff);

    imgs__vertex(s, x + 0, y + 0, 0, 0, 0xffffffff);
    imgs__vertex(s, x + 0, y + h, 0, 1, 0xffffffff);
    imgs__vertex(s, x + w, y + h, 1, 1, 0xffffffff);

    // Atlas nodes
    for (i = 0; i < s->atlas->nnodes; i++) {
        struct IMGSatlasNode* n = &s->atlas->nodes[i];

        if (s->nverts + 6 > IMGS_VERTEX_COUNT) imgs__flush(s);

        imgs__vertex(s, x + n->x + 0, y + n->y + 0, u, v, 0xc00000ff);
        imgs__vertex(s, x + n->x + n->width, y + n->y + 1, u, v, 0xc00000ff);
        imgs__vertex(s, x + n->x + n->width, y + n->y + 0, u, v, 0xc00000ff);

        imgs__vertex(s, x + n->x + 0, y + n->y + 0, u, v, 0xc00000ff);
        imgs__vertex(s, x + n->x + 0, y + n->y + 1, u, v, 0xc00000ff);
        imgs__vertex(s, x + n->x + n->width, y + n->y + 1, u, v, 0xc00000ff);
    }

    imgs__flush(s);
}
#endif // IMAGESTASH_IMPLEMENTATION
