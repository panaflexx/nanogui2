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

/* Skyline packs arbitrary rectangles. Photo packs rows: every row stays
   open, a thumbnail goes to the row its height fits best, and a new row
   opens under the others when none is close. The atlas grows by rows. */
#define IMGS_PACK_SKYLINE 0
#define IMGS_PACK_PHOTO   1

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
    int packer; /* IMGS_PACK_SKYLINE or IMGS_PACK_PHOTO */
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

/* What the atlas holds and where its pixels go. Every area is in pixels;
   `packed` = `inner` + `padding`, and for the photo packer the used band
   (width x used_height) = `packed` + `under_rows` + `row_ends`. Free is the
   atlas below the band. */
struct IMGSstats {
    int width, height;        /* atlas */
    int packer;               /* IMGS_PACK_SKYLINE or IMGS_PACK_PHOTO */
    int images;               /* packed images */
    int rows;                 /* photo: rows handed out */
    int used_height;          /* photo: next row's y; skyline: highest span */
    long long inner;          /* image pixels */
    long long padding;        /* IMGS_PAD border around each image */
    long long packed;         /* inner + padding */
    long long under_rows;     /* photo: under images shorter than their row */
    long long row_ends;       /* photo: right of each row's last image
                                 (every row stays open, so can still fill) */
    long long free_area;      /* below the used band */
    long long tex_bytes;      /* width * height * 4 */
    long long cpu_bytes;      /* texture + image table + skyline nodes */
};
typedef struct IMGSstats IMGSstats;
void imgsGetStats(IMGcontext* s, IMGSstats* out);

/* Checks that the stash is well formed: every image inside the atlas and
   inside the band the packer has handed out, no two images overlapping,
   the skyline covering the width with every image under it, photo rows
   tiling the band with each image inside one row, and the name table
   finding each image exactly once. Returns the number of problems; the
   first few are written to `msg` one per line (may be NULL). */
int imgsValidate(IMGcontext* s, char* msg, int msglen);

#endif // IMAGESTASH_H

#ifdef IMAGESTASH_IMPLEMENTATION

#include <stdio.h>
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
#define IMGS_ATLAS_MAX 4096
/* A thumbnail this many pixels taller than its row still shares that row
   when the row is the bottom one and can grow. */
#define IMGS_ROW_SLACK 4
/* A thumbnail joins a row up to 1/IMGS_ROW_FIT of the row's height shorter
   (or IMGS_ROW_SLACK, when that is more); a shorter one opens its own row.
   Once the atlas is at its cap, any row with room takes it. */
#define IMGS_ROW_FIT 6

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

struct IMGSrow {
    int y;       /* top; rows are kept in y order and tile 0..next_y */
    int x;       /* next free x */
    int height;  /* fixed by the row's first thumbnail, or grown by slack */
};

struct IMGSatlas {
    int width, height;
    int pack;    /* IMGS_PACK_SKYLINE or IMGS_PACK_PHOTO */
    int nnodes;
    int cnodes;
    struct IMGSatlasNode* nodes;
    struct IMGSrow* rows;  /* photo packer: every row, in y order */
    int nrows;
    int crows;
    int next_y;            /* y where the next photo row starts */
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

// Atlas functions. Skyline matches fontstash (a rect may span neighbouring
// spans, and adjacent spans of equal height merge). Photo is a list of rows.
static struct IMGSatlas* imgs__allocAtlas(int w, int h, int cnodes) {
    struct IMGSatlas* atlas = (struct IMGSatlas*)malloc(sizeof(struct IMGSatlas));
    if (atlas == NULL) return NULL;
    memset(atlas, 0, sizeof(struct IMGSatlas));
    atlas->width = w;
    atlas->height = h;
    atlas->pack = IMGS_PACK_SKYLINE;
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
        if (atlas->rows != NULL) free(atlas->rows);
        free(atlas);
    }
}

static int imgs__atlasInsertNode(struct IMGSatlas* atlas, int idx, int x, int y, int w) {
    int i;
    if (atlas->nnodes + 1 > atlas->cnodes) {
        int old_c = atlas->cnodes;
        atlas->cnodes = atlas->cnodes == 0 ? 8 : atlas->cnodes * 2;
        atlas->nodes = (struct IMGSatlasNode*)realloc(atlas->nodes, sizeof(struct IMGSatlasNode) * atlas->cnodes);
        if (atlas->nodes == NULL) return 0;
        memset(atlas->nodes + old_c, 0, sizeof(struct IMGSatlasNode) * (atlas->cnodes - old_c));
    }
    for (i = atlas->nnodes; i > idx; i--)
        atlas->nodes[i] = atlas->nodes[i - 1];
    atlas->nodes[idx].x = (short)x;
    atlas->nodes[idx].y = (short)y;
    atlas->nodes[idx].width = (short)w;
    atlas->nnodes++;
    return 1;
}

static void imgs__atlasRemoveNode(struct IMGSatlas* atlas, int idx) {
    int i;
    if (atlas->nnodes == 0) return;
    for (i = idx; i < atlas->nnodes - 1; i++)
        atlas->nodes[i] = atlas->nodes[i + 1];
    atlas->nnodes--;
}

static void imgs__atlasReset(struct IMGSatlas* atlas, int w, int h) {
    int pack = atlas->pack;
    atlas->width = w;
    atlas->height = h;
    atlas->nrows = 0;
    atlas->next_y = 0;
    atlas->nnodes = 0;
    if (pack == IMGS_PACK_PHOTO)
        return;
    atlas->nodes[0].x = 0;
    atlas->nodes[0].y = 0;
    atlas->nodes[0].width = (short)w;
    atlas->nnodes++;
}

static void imgs__atlasExpand(struct IMGSatlas* atlas, int w, int h) {
    /* The new strip on the right is free skyline. Photo rows already walk
       atlas->width, so a wider atlas extends the open rows. */
    if (atlas->pack != IMGS_PACK_PHOTO && w > atlas->width)
        imgs__atlasInsertNode(atlas, atlas->nnodes, atlas->width, 0, w - atlas->width);
    atlas->width = w;
    atlas->height = h;
}

static int imgs__atlasAddSkylineLevel(struct IMGSatlas* atlas, int idx, int x, int y, int w, int h) {
    int i;
    if (imgs__atlasInsertNode(atlas, idx, x, y + h, w) == 0)
        return 0;

    /* Drop the skyline spans that sit under the new rectangle. */
    for (i = idx + 1; i < atlas->nnodes; i++) {
        if (atlas->nodes[i].x < atlas->nodes[i - 1].x + atlas->nodes[i - 1].width) {
            int shrink = atlas->nodes[i - 1].x + atlas->nodes[i - 1].width - atlas->nodes[i].x;
            atlas->nodes[i].x += (short)shrink;
            atlas->nodes[i].width -= (short)shrink;
            if (atlas->nodes[i].width <= 0) {
                imgs__atlasRemoveNode(atlas, i);
                i--;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    /* Join neighbours that now share a height, so a later rect can span them. */
    for (i = 0; i < atlas->nnodes - 1; i++) {
        if (atlas->nodes[i].y == atlas->nodes[i + 1].y) {
            atlas->nodes[i].width += atlas->nodes[i + 1].width;
            imgs__atlasRemoveNode(atlas, i + 1);
            i--;
        }
    }
    return 1;
}

static int imgs__atlasRectFits(struct IMGSatlas* atlas, int i, int w, int h) {
    int x = atlas->nodes[i].x;
    int y = atlas->nodes[i].y;
    int spaceLeft;
    if (x + w > atlas->width)
        return -1;
    spaceLeft = w;
    while (spaceLeft > 0) {
        if (i == atlas->nnodes) return -1;
        if (atlas->nodes[i].y > y)
            y = atlas->nodes[i].y;
        if (y + h > atlas->height) return -1;
        spaceLeft -= atlas->nodes[i].width;
        ++i;
    }
    return y;
}

static int imgs__atlasAddSkyline(struct IMGSatlas* atlas, int rw, int rh, int* rx, int* ry) {
    int besth = atlas->height, bestw = atlas->width, besti = -1;
    int bestx = -1, besty = -1, i;

    if (rw <= 0 || rh <= 0)
        return 0;

    for (i = 0; i < atlas->nnodes; i++) {
        int y = imgs__atlasRectFits(atlas, i, rw, rh);
        if (y != -1) {
            if (y + rh < besth || (y + rh == besth && atlas->nodes[i].width < bestw)) {
                besti = i;
                bestw = atlas->nodes[i].width;
                besth = y + rh;
                bestx = atlas->nodes[i].x;
                besty = y;
            }
        }
    }
    if (besti == -1)
        return 0;
    if (imgs__atlasAddSkylineLevel(atlas, besti, bestx, besty, rw, rh) == 0)
        return 0;
    *rx = bestx;
    *ry = besty;
    return 1;
}

/* Photo rows. A thumbnail goes to the open row whose height it fits best:
   the least height left unused under it, within 1/IMGS_ROW_FIT of the row.
   No row stays behind half empty because a different height came next; it
   takes the next thumbnail of its height. A thumbnail no row fits opens a
   new row under the others, at its own height. `loose` (the atlas cannot
   grow) takes any row with room. */
static int imgs__photoOpen(struct IMGSatlas* atlas, int rh) {
    struct IMGSrow* row;
    if (atlas->next_y + rh > atlas->height)
        return -1;
    if (atlas->nrows + 1 > atlas->crows) {
        int c = atlas->crows ? atlas->crows * 2 : 64;
        struct IMGSrow* rows = (struct IMGSrow*)realloc(atlas->rows, sizeof(struct IMGSrow) * (size_t)c);
        if (rows == NULL)
            return -1;
        atlas->rows = rows;
        atlas->crows = c;
    }
    row = &atlas->rows[atlas->nrows];
    row->y = atlas->next_y;
    row->x = 0;
    row->height = rh;
    atlas->next_y += rh;
    return atlas->nrows++;
}

static int imgs__atlasAddPhoto(struct IMGSatlas* atlas, int rw, int rh, int* rx, int* ry, int loose) {
    int i, best = -1, best_waste = INT_MAX;
    struct IMGSrow* row;
    if (rw <= 0 || rh <= 0 || rw > atlas->width)
        return 0;

    for (i = 0; i < atlas->nrows; ++i) {
        const struct IMGSrow* r = &atlas->rows[i];
        int waste = r->height - rh;
        int fit = r->height / IMGS_ROW_FIT;
        if (fit < IMGS_ROW_SLACK) fit = IMGS_ROW_SLACK;
        if (waste < 0 || r->x + rw > atlas->width || (!loose && waste > fit))
            continue;
        if (waste < best_waste) {
            best = i;
            best_waste = waste;
            if (waste == 0)
                break;
        }
    }

    /* A few pixels taller still fits the bottom row, which can grow */
    if (best < 0 && atlas->nrows > 0) {
        struct IMGSrow* last = &atlas->rows[atlas->nrows - 1];
        int extra = rh - last->height;
        if (extra > 0 && extra <= IMGS_ROW_SLACK && last->x + rw <= atlas->width &&
            atlas->next_y + extra <= atlas->height) {
            last->height += extra;
            atlas->next_y += extra;
            best = atlas->nrows - 1;
        }
    }

    if (best < 0)
        best = imgs__photoOpen(atlas, rh);
    if (best < 0)
        return 0; /* the atlas must grow */

    row = &atlas->rows[best];
    *rx = row->x;
    *ry = row->y;
    row->x += rw;
    return 1;
}

static int imgs__atlasAddRect(struct IMGSatlas* atlas, int rw, int rh, int* rx, int* ry) {
    if (atlas->pack == IMGS_PACK_PHOTO)
        return imgs__atlasAddPhoto(atlas, rw, rh, rx, ry, 0);
    return imgs__atlasAddSkyline(atlas, rw, rh, rx, ry);
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

/* Skyline doubles both sides up to the cap. Photo keeps its width and
   grows tall enough for one more row. */
static int imgs__growAtlas(IMGcontext* stash, int gw, int gh) {
    int nw, nh;
    if (stash->atlas->pack == IMGS_PACK_PHOTO) {
        nw = stash->params.width;
        nh = stash->params.height;
        if (gw > nw)
            nw = gw;
        if (stash->atlas->next_y + gh > nh)
            nh = stash->atlas->next_y + gh;
        /* A chunk of rows shares one texture. Growing by a single row would
           throw the GPU copy away on every thumbnail. */
        nh = (nh + 511) & ~511;
        if (nh > IMGS_ATLAS_MAX)
            nh = IMGS_ATLAS_MAX;
        if (nw > IMGS_ATLAS_MAX || nh > IMGS_ATLAS_MAX)
            return 0;
        if (nw == stash->params.width && nh == stash->params.height)
            return 0;
    } else {
        nw = stash->params.width * 2;
        nh = stash->params.height * 2;
        if (nw > IMGS_ATLAS_MAX) nw = IMGS_ATLAS_MAX;
        if (nh > IMGS_ATLAS_MAX) nh = IMGS_ATLAS_MAX;
        if (nw == stash->params.width && nh == stash->params.height)
            return 0;
    }
    return imgsExpandAtlas(stash, nw, nh);
}

static int imgs__addImage(IMGcontext* stash, const char* name, int width, int height, unsigned char* data, int freeData) {
    int idx;
    int gw, gh, gx = 0, gy = 0;
    unsigned int hash;
    struct IMGSimageImpl* img;

    /* The same name is already packed. Drop the new pixels when we own them
       and keep the atlas copy. */
    if (name && imgs__getImageIndex(stash, name) != -1) {
        if (freeData && data)
            free(data);
        return 1;
    }

    gw = width + 2 * IMGS_PAD;
    gh = height + 2 * IMGS_PAD;
    while (imgs__atlasAddRect(stash->atlas, gw, gh, &gx, &gy) == 0) {
        /* At the cap, a photo row that fits less well is still room */
        if (stash->atlas->pack == IMGS_PACK_PHOTO &&
            (stash->params.height >= IMGS_ATLAS_MAX ||
             stash->atlas->next_y + gh > IMGS_ATLAS_MAX) &&
            imgs__atlasAddPhoto(stash->atlas, gw, gh, &gx, &gy, 1))
            break;
        if (!imgs__growAtlas(stash, gw, gh)) {
            if (freeData) free(data);
            if (stash->handleError)
                stash->handleError(stash->errorUptr, IMGS_ATLAS_FULL, 0);
            return 0;
        }
    }

    idx = imgs__allocImage(stash);
    if (idx == -1) {
        if (freeData) free(data);
        return 0;
    }

    img = &stash->images[idx];
    strncpy(img->name, name, IMGS_MAX_NAME_LEN);
    img->name[IMGS_MAX_NAME_LEN - 1] = '\0';
    img->width = (short)width;
    img->height = (short)height;
    img->x = (short)gx;
    img->y = (short)gy;
    img->next = -1;

    hash = imgs__hashstr(name) % IMGS_HASH_LUT_SIZE;
    img->next = stash->lut[hash];
    stash->lut[hash] = idx;

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
        if (!stbir_resize_uint8_linear(data, w, h, 0, resized, nw, nh, 0, (stbir_pixel_layout)bytes_per_pixel)) {
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

    if (!stbir_resize_uint8_linear(img->pixels, img->width, img->height, 0, newPixels, newWidth, newHeight, 0, (stbir_pixel_layout)bytes_per_pixel)) {
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
            if (!imgs__growAtlas(s, gw, gh)) {
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
    if (params->packer == IMGS_PACK_PHOTO) {
        stash->atlas->pack = IMGS_PACK_PHOTO;
        stash->atlas->nnodes = 0; /* photo rows, not a skyline */
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

    if (stash->atlas->pack == IMGS_PACK_PHOTO)
        maxy = stash->atlas->next_y;
    else {
        for (i = 0; i < stash->atlas->nnodes; i++)
            maxy = (int)fmax(maxy, stash->atlas->nodes[i].y);
    }
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

/* ---- Stats and validation ---------------------------------------------- */

struct IMGSrect { int x, y, w, h, idx; };

static int imgs__cmpRectY(const void* a, const void* b) {
    const struct IMGSrect* ra = (const struct IMGSrect*)a;
    const struct IMGSrect* rb = (const struct IMGSrect*)b;
    if (ra->y != rb->y) return ra->y < rb->y ? -1 : 1;
    if (ra->x != rb->x) return ra->x < rb->x ? -1 : 1;
    return 0;
}

/* Every packed rect (padding included), sorted by y, then x. Caller frees;
   NULL for an empty stash. */
static struct IMGSrect* imgs__rects(IMGcontext* s, int* count) {
    int i, n = s->nimages;
    struct IMGSrect* r;
    *count = 0;
    if (n <= 0) return NULL;
    r = (struct IMGSrect*)malloc(sizeof(struct IMGSrect) * (size_t)n);
    if (r == NULL) return NULL;
    for (i = 0; i < n; ++i) {
        r[i].x = s->images[i].x;
        r[i].y = s->images[i].y;
        r[i].w = s->images[i].width + 2 * IMGS_PAD;
        r[i].h = s->images[i].height + 2 * IMGS_PAD;
        r[i].idx = i;
    }
    qsort(r, (size_t)n, sizeof(struct IMGSrect), imgs__cmpRectY);
    *count = n;
    return r;
}

static int imgs__usedHeight(IMGcontext* s) {
    int i, maxy = 0;
    if (s->atlas->pack == IMGS_PACK_PHOTO)
        return s->atlas->next_y;
    for (i = 0; i < s->atlas->nnodes; ++i)
        if (s->atlas->nodes[i].y > maxy) maxy = s->atlas->nodes[i].y;
    return maxy;
}

/* The photo row starting at `y`, or -1. Rows are in y order. */
static int imgs__rowAt(const struct IMGSatlas* a, int y) {
    int lo = 0, hi = a->nrows - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a->rows[mid].y == y) return mid;
        if (a->rows[mid].y < y) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

void imgsGetStats(IMGcontext* s, IMGSstats* out) {
    int i;
    const struct IMGSatlas* a;
    if (s == NULL || out == NULL) return;
    a = s->atlas;
    memset(out, 0, sizeof(*out));
    out->width = s->params.width;
    out->height = s->params.height;
    out->packer = a->pack;
    out->images = s->nimages;
    out->used_height = imgs__usedHeight(s);
    for (i = 0; i < s->nimages; ++i) {
        long long w = s->images[i].width, h = s->images[i].height;
        out->inner += w * h;
        out->packed += (w + 2 * IMGS_PAD) * (h + 2 * IMGS_PAD);
    }
    out->padding = out->packed - out->inner;
    out->free_area = (long long)out->width * (out->height - out->used_height);
    out->tex_bytes = (long long)out->width * out->height * 4;
    out->cpu_bytes = out->tex_bytes +
                     (long long)s->cimages * (long long)sizeof(struct IMGSimageImpl) +
                     (long long)a->cnodes * (long long)sizeof(struct IMGSatlasNode) +
                     (long long)a->crows * (long long)sizeof(struct IMGSrow);
    if (a->pack != IMGS_PACK_PHOTO)
        return;

    /* A row's images run from x 0 to its free x without gaps, so what is
       not image up to there lies under images shorter than the row */
    out->rows = a->nrows;
    {
        long long filled = 0;
        for (i = 0; i < a->nrows; ++i) {
            filled += (long long)a->rows[i].x * a->rows[i].height;
            out->row_ends += (long long)(out->width - a->rows[i].x) * a->rows[i].height;
        }
        out->under_rows = filled - out->packed;
    }
}

static void imgs__problem(int* count, char* msg, int msglen, const char* fmt, int a, int b, int c, int d) {
    char line[160];
    size_t used;
    ++*count;
    if (msg == NULL || msglen <= 0 || *count > 8) return;
    snprintf(line, sizeof(line), fmt, a, b, c, d);
    used = strlen(msg);
    snprintf(msg + used, (size_t)msglen - used, "%s\n", line);
}

int imgsValidate(IMGcontext* s, char* msg, int msglen) {
    int problems = 0, i, n = 0, band;
    struct IMGSatlas* a;
    struct IMGSrect* r;
    if (msg && msglen > 0) msg[0] = '\0';
    if (s == NULL || s->atlas == NULL || s->texData == NULL) {
        imgs__problem(&problems, msg, msglen, "no atlas%.0d%.0d%.0d%.0d", 0, 0, 0, 0);
        return problems;
    }
    a = s->atlas;
    if (a->width != s->params.width || a->height != s->params.height)
        imgs__problem(&problems, msg, msglen, "packer is %dx%d, texture %dx%d",
                      a->width, a->height, s->params.width, s->params.height);
    if (s->params.width > IMGS_ATLAS_MAX || s->params.height > IMGS_ATLAS_MAX)
        imgs__problem(&problems, msg, msglen, "atlas %dx%d is past the cap %d%.0d",
                      s->params.width, s->params.height, IMGS_ATLAS_MAX, 0);
    if (s->nimages < 0 || s->nimages > s->cimages)
        imgs__problem(&problems, msg, msglen, "%d images in a table of %d%.0d%.0d",
                      s->nimages, s->cimages, 0, 0);
    band = imgs__usedHeight(s);
    if (band < 0 || band > s->params.height)
        imgs__problem(&problems, msg, msglen, "used band %d is outside 0..%d%.0d%.0d",
                      band, s->params.height, 0, 0);

    /* Bounds */
    for (i = 0; i < s->nimages; ++i) {
        const struct IMGSimageImpl* im = &s->images[i];
        int gw = im->width + 2 * IMGS_PAD, gh = im->height + 2 * IMGS_PAD;
        if (im->width <= 0 || im->height <= 0)
            imgs__problem(&problems, msg, msglen, "image %d is %dx%d%.0d", i, im->width, im->height, 0);
        if (im->x < 0 || im->y < 0 || im->x + gw > s->params.width || im->y + gh > s->params.height)
            imgs__problem(&problems, msg, msglen, "image %d at %d,%d runs out of the atlas (height %d)",
                          i, im->x, im->y, s->params.height);
        if (a->pack == IMGS_PACK_PHOTO && im->y + gh > band)
            imgs__problem(&problems, msg, msglen, "image %d ends at y %d, below the band %d%.0d",
                          i, im->y + gh, band, 0);
    }

    /* Overlap: sorted by y, so a rect can only meet the ones after it that
       start above its bottom */
    r = imgs__rects(s, &n);
    if (r == NULL && s->nimages > 0) {
        imgs__problem(&problems, msg, msglen, "out of memory%.0d%.0d%.0d%.0d", 0, 0, 0, 0);
        return problems;
    }
    for (i = 0; i < n; ++i)
        for (int j = i + 1; j < n && r[j].y < r[i].y + r[i].h; ++j)
            if (r[j].x < r[i].x + r[i].w && r[i].x < r[j].x + r[j].w)
                imgs__problem(&problems, msg, msglen, "images %d and %d overlap at %d,%d",
                              r[i].idx, r[j].idx, r[j].x, r[j].y);

    if (a->pack == IMGS_PACK_PHOTO) {
        /* Rows tile the band from the top, each inside the width */
        int y = 0;
        long long filled = 0;
        if (a->nrows < 0 || a->nrows > a->crows)
            imgs__problem(&problems, msg, msglen, "%d rows in a table of %d%.0d%.0d", a->nrows, a->crows, 0, 0);
        for (i = 0; i < a->nrows; ++i) {
            const struct IMGSrow* row = &a->rows[i];
            if (row->y != y)
                imgs__problem(&problems, msg, msglen, "row %d starts at y %d, the row above ends at %d%.0d",
                              i, row->y, y, 0);
            if (row->height <= 0 || row->x < 0 || row->x > a->width)
                imgs__problem(&problems, msg, msglen, "row %d is %d tall with free x %d of %d",
                              i, row->height, row->x, a->width);
            y = row->y + row->height;
            filled += (long long)row->x * row->height;
        }
        if (y != band)
            imgs__problem(&problems, msg, msglen, "rows end at y %d, the band at %d%.0d%.0d", y, band, 0, 0);

        /* Each image sits at the top of one row, inside its height and left
           of its free x, and each row's images run from 0 to its free x
           without a gap: so the row widths add up to the images in them */
        {
            long long packed = 0;
            int* reach = (int*)calloc((size_t)(a->nrows > 0 ? a->nrows : 1), sizeof(int));
            for (i = 0; i < n; ++i) {
                int k = imgs__rowAt(a, r[i].y);
                packed += (long long)r[i].w * r[i].h;
                if (k < 0) {
                    imgs__problem(&problems, msg, msglen, "image %d at y %d is not at the top of a row%.0d%.0d",
                                  r[i].idx, r[i].y, 0, 0);
                    continue;
                }
                if (r[i].h > a->rows[k].height || r[i].x + r[i].w > a->rows[k].x)
                    imgs__problem(&problems, msg, msglen, "image %d runs out of row %d (%d tall, free x %d)",
                                  r[i].idx, k, a->rows[k].height, a->rows[k].x);
                if (reach && r[i].x == reach[k])
                    reach[k] = r[i].x + r[i].w;
            }
            for (i = 0; reach && i < a->nrows; ++i)
                if (reach[i] != a->rows[i].x)
                    imgs__problem(&problems, msg, msglen, "row %d has a gap: images reach x %d, free x is %d%.0d",
                                  i, reach[i], a->rows[i].x, 0);
            free(reach);
            if (packed > filled)
                imgs__problem(&problems, msg, msglen, "rows hold %d px, their images %d px%.0d%.0d",
                              (int)filled, (int)packed, 0, 0);
        }
    } else {
        /* Skyline spans run left to right, end to end, across the width,
           and every image sits on or under them */
        int x = 0;
        for (i = 0; i < a->nnodes; ++i) {
            if (a->nodes[i].x != x || a->nodes[i].width <= 0)
                imgs__problem(&problems, msg, msglen, "skyline span %d is at x %d width %d, expected x %d",
                              i, a->nodes[i].x, a->nodes[i].width, x);
            if (a->nodes[i].y > a->height)
                imgs__problem(&problems, msg, msglen, "skyline span %d is at y %d, past %d%.0d",
                              i, a->nodes[i].y, a->height, 0);
            x = a->nodes[i].x + a->nodes[i].width;
        }
        if (x != a->width)
            imgs__problem(&problems, msg, msglen, "skyline ends at x %d of %d%.0d%.0d", x, a->width, 0, 0);
        for (i = 0; i < n; ++i)
            for (int k = 0; k < a->nnodes; ++k) {
                const struct IMGSatlasNode* nd = &a->nodes[k];
                if (nd->x < r[i].x + r[i].w && r[i].x < nd->x + nd->width && r[i].y + r[i].h > nd->y)
                    imgs__problem(&problems, msg, msglen, "image %d pokes through skyline span %d (y %d > %d)",
                                  r[i].idx, k, r[i].y + r[i].h, nd->y);
            }
    }
    free(r);

    /* Name table: each chain stays in the table and in its own bucket, and
       together they reach every image once */
    {
        int reached = 0;
        for (i = 0; i < IMGS_HASH_LUT_SIZE; ++i) {
            int steps = 0;
            for (int k = s->lut[i]; k != -1; k = s->images[k].next) {
                if (k < 0 || k >= s->nimages) {
                    imgs__problem(&problems, msg, msglen, "bucket %d links to image %d of %d%.0d",
                                  i, k, s->nimages, 0);
                    break;
                }
                if ((int)(imgs__hashstr(s->images[k].name) % IMGS_HASH_LUT_SIZE) != i)
                    imgs__problem(&problems, msg, msglen, "image %d is in bucket %d, not its own%.0d%.0d",
                                  k, i, 0, 0);
                if (++steps > s->nimages) {
                    imgs__problem(&problems, msg, msglen, "bucket %d loops%.0d%.0d%.0d", i, 0, 0, 0);
                    break;
                }
                ++reached;
            }
        }
        if (reached != s->nimages)
            imgs__problem(&problems, msg, msglen, "the name table reaches %d of %d images%.0d%.0d",
                          reached, s->nimages, 0, 0);
        for (i = 0; i < s->nimages; ++i)
            if (imgs__getImageIndex(s, s->images[i].name) != i)
                imgs__problem(&problems, msg, msglen, "image %d's name finds image %d%.0d%.0d",
                              i, imgs__getImageIndex(s, s->images[i].name), 0, 0);
    }
    return problems;
}

#endif // IMAGESTASH_IMPLEMENTATION
