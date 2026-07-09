//
// Copyright (c) 2009-2013 Mikko Mononen memon@inside.org
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

#ifndef FONS_H
#define FONS_H

#define FONS_INVALID -1
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#include <stdint.h>

//#define DEBUG
#ifdef DEBUG
#define dprintf(fmt, ...) \
    printf("%s:%d %s() " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
#define dprintf(fmt, ...) ((void)0)
#endif

enum FONSflags {
    FONS_ZERO_TOPLEFT = 1,
    FONS_ZERO_BOTTOMLEFT = 2,
};

enum FONSalign {
    // Horizontal align
    FONS_ALIGN_LEFT 	= 1<<0,	// Default
    FONS_ALIGN_CENTER 	= 1<<1,
    FONS_ALIGN_RIGHT 	= 1<<2,
    // Vertical align
    FONS_ALIGN_TOP 		= 1<<3,
    FONS_ALIGN_MIDDLE	= 1<<4,
    FONS_ALIGN_BOTTOM	= 1<<5,
    FONS_ALIGN_BASELINE	= 1<<6, // Default
};

enum FONSglyphBitmap {
    FONS_GLYPH_BITMAP_OPTIONAL = 1,
    FONS_GLYPH_BITMAP_REQUIRED = 2,
};

enum FONSerrorCode {
    // Font atlas is full.
    FONS_ATLAS_FULL = 1,
    // Scratch memory used to render glyphs is full, requested size reported in 'val', you may need to bump up FONS_SCRATCH_BUF_SIZE.
    FONS_SCRATCH_FULL = 2,
    // Calls to fonsPushState has created too large stack, if you need deep state stack bump up FONS_MAX_STATES.
    FONS_STATES_OVERFLOW = 3,
    // Trying to pop too many states fonsPopState().
    FONS_STATES_UNDERFLOW = 4,
};

struct FONSparams {
    int width, height;
    unsigned char flags;
    void* userPtr;
    int (*renderCreate)(void* uptr, int width, int height);
    int (*renderResize)(void* uptr, int width, int height);
    void (*renderUpdate)(void* uptr, int* rect, const unsigned char* data);
    void (*renderDraw)(void* uptr, const float* verts, const float* tcoords, const unsigned int* colors, int nverts);
    void (*renderDelete)(void* uptr);
};
typedef struct FONSparams FONSparams;

struct FONSquad
{
    float x0,y0,s0,t0;
    float x1,y1,s1,t1;
};
typedef struct FONSquad FONSquad;

struct FONStextIter {
    float x, y, nextx, nexty, scale, spacing;
    unsigned int codepoint;
    short isize, iblur;
    struct FONSfont* font;
    int prevGlyphIndex;
    const char* str;
    const char* next;
    const char* end;
    unsigned int utf8state;
    int bitmapOption;
};
typedef struct FONStextIter FONStextIter;

typedef struct FONScontext FONScontext;

// Constructor and destructor.
FONScontext* fonsCreateInternal(FONSparams* params);
void fonsDeleteInternal(FONScontext* s);

void fonsSetErrorCallback(FONScontext* s, void (*callback)(void* uptr, int error, int val), void* uptr);
// Returns current atlas size.
void fonsGetAtlasSize(FONScontext* s, int* width, int* height);
// Expands the atlas size.
int fonsExpandAtlas(FONScontext* s, int width, int height);
// Resets the whole stash.
int fonsResetAtlas(FONScontext* stash, int width, int height);

// Add fonts
int fonsAddFont(FONScontext* s, const char* name, const char* path);
int fonsAddFontMem(FONScontext* s, const char* name, unsigned char* data, int ndata, int freeData);
int fonsGetFontByName(FONScontext* s, const char* name);

// State handling
void fonsPushState(FONScontext* s);
void fonsPopState(FONScontext* s);
void fonsClearState(FONScontext* s);

// State setting
void fonsSetSize(FONScontext* s, float size);
void fonsSetColor(FONScontext* s, unsigned int color);
void fonsSetSpacing(FONScontext* s, float spacing);
void fonsSetBlur(FONScontext* s, float blur);
void fonsSetAlign(FONScontext* s, int align);
void fonsSetFont(FONScontext* s, int font);

// Draw text
float fonsDrawText(FONScontext* s, float x, float y, const char* string, const char* end);

// Measure text
float fonsTextBounds(FONScontext* s, float x, float y, const char* string, const char* end, float* bounds);
void fonsLineBounds(FONScontext* s, float y, float* miny, float* maxy);
void fonsVertMetrics(FONScontext* s, float* ascender, float* descender, float* lineh);

// Text iterator
int fonsTextIterInit(FONScontext* stash, FONStextIter* iter, float x, float y, const char* str, const char* end, int bitmapOption);
int fonsTextIterNext(FONScontext* stash, FONStextIter* iter, struct FONSquad* quad);

// Pull texture changes
const unsigned char* fonsGetTextureData(FONScontext* stash, int* width, int* height);
int fonsValidateTexture(FONScontext* s, int* dirty);

// Draws the stash texture for debugging
void fonsDrawDebug(FONScontext* s, float x, float y);

#endif // FONTSTASH_H


#ifdef FONTSTASH_IMPLEMENTATION

#define FONS_NOTUSED(v)  (void)sizeof(v)

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include <math.h>

struct FONSttFontImpl {
    FT_Face font;
};
typedef struct FONSttFontImpl FONSttFontImpl;

static FT_Library ftLibrary;

int fons__tt_init(FONScontext *context)
{
    FT_Error ftError;
    FONS_NOTUSED(context);
    ftError = FT_Init_FreeType(&ftLibrary);
    return ftError == 0;
}

int fons__tt_done(FONScontext *context)
{
    FT_Error ftError;
    FONS_NOTUSED(context);
    ftError = FT_Done_FreeType(ftLibrary);
    return ftError == 0;
}

int fons__tt_loadFont(FONScontext *context, FONSttFontImpl *font, unsigned char *data, int dataSize)
{
    FT_Error ftError;
    FONS_NOTUSED(context);

    //font->font.userdata = stash;
    ftError = FT_New_Memory_Face(ftLibrary, (const FT_Byte*)data, dataSize, 0, &font->font);
    return ftError == 0;
}

void fons__tt_getFontVMetrics(FONSttFontImpl *font, int *ascent, int *descent, int *lineGap)
{
    FT_Error ftError = 0;
    int raw_ascent = font->font->ascender;
    int raw_descent = font->font->descender;
    int raw_height = font->font->height;

    // If metrics are invalid (common for color/bitmap fonts without size set), select a default size to populate them.
    if (raw_ascent == 0 || raw_descent == 0) {
        if (FT_HAS_COLOR(font->font)) {
            // Color fonts: Select embedded strike (index 0) for metrics.
            ftError = FT_Select_Size(font->font, 0);
            if (!ftError) {
                // Get metrics in pixels (26.6 fixed-point)
                raw_ascent = (int)(font->font->size->metrics.ascender >> 6);
                raw_descent = (int)(font->font->size->metrics.descender >> 6);
                raw_height = (int)(font->font->size->metrics.height >> 6);
                dprintf("fons__tt_getFontVMetrics: Selected color strike 0; raw metrics updated.\n");
            }
        } else if (!FT_IS_SCALABLE(font->font)) {
            // Non-color bitmap font: Select closest strike to a default size (e.g., 32px).
            int default_size = 32;
            int closest = 0;
            int min_diff = INT_MAX;
            if (font->font->num_fixed_sizes > 0) {
                for (int k = 0; k < font->font->num_fixed_sizes; k++) {
                    int h = (int)(font->font->available_sizes[k].height);
                    int diff = abs(h - default_size);
                    if (diff < min_diff) {
                        min_diff = diff;
                        closest = k;
                    }
                }
                ftError = FT_Select_Size(font->font, closest);
                if (!ftError) {
                    // Get metrics in pixels
                    raw_ascent = (int)(font->font->size->metrics.ascender >> 6);
                    raw_descent = (int)(font->font->size->metrics.descender >> 6);
                    raw_height = (int)(font->font->size->metrics.height >> 6);
                    dprintf("fons__tt_getFontVMetrics: Selected closest bitmap strike %d for size ~%d; raw metrics updated.\n", closest, default_size);
                }
            }
        } else {
            // Scalable non-color font: Set nominal pixel size (e.g., based on EM units) to get pixel metrics.
            float emSize = (float)font->font->units_per_EM / (float)(font->font->ascender - font->font->descender);
            FT_UInt ps = (FT_UInt)(1000.0f * emSize);  // Nominal ~1000 units for normalization
            if (ps > 1024) ps = 1024;  // Cap
            ftError = FT_Set_Pixel_Sizes(font->font, 0, ps);
            if (!ftError) {
                // Get scaled metrics in pixels
                raw_ascent = (int)(font->font->size->metrics.ascender >> 6);
                raw_descent = (int)(font->font->size->metrics.descender >> 6);
                raw_height = (int)(font->font->size->metrics.height >> 6);
                dprintf("fons__tt_getFontVMetrics: Set pixel size %u for scalable font; raw metrics updated.\n", ps);
            }
        }
        // If selection failed, fall back to defaults (handled downstream in fonsAddFontMem).
    }

    // Assign final values (use raw if updated, else original font units for scalable).
    *ascent = raw_ascent;
    *descent = raw_descent;
    *lineGap = raw_height - (*ascent - *descent);

    dprintf("fons__tt_getFontVMetrics: ascent=%d descent=%d lineGap=%d\n",
            *ascent, *descent, *lineGap);
}

float fons__tt_getPixelHeightScale(FONSttFontImpl *font, float size)
{
	int ascender, descender, line_height;

	// Set font size for measurements
    if (FT_HAS_COLOR(font->font)) {
		// Bitmap Color fonts have no sizing - just select size 0
        if (FT_Select_Size(font->font, 0)) {
            return 1; // failed
        }
    } else {
        if (FT_Set_Pixel_Sizes(font->font, 0, (FT_UInt)size)) {
            return 1; // failed
        }
    }

    // Get metrics from face
    FT_Size_Metrics size_metrics = font->font->size->metrics;
    ascender = (float)(size_metrics.ascender >> 6);
    descender = (float)(size_metrics.descender >> 6);
    line_height = (float)(size_metrics.height >> 6);

    return size / (ascender - descender);
}

int fons__tt_getGlyphIndex(FONSttFontImpl *font, int codepoint)
{
    return FT_Get_Char_Index(font->font, codepoint);
}

int fons__tt_buildGlyphBitmap(FONSttFontImpl *font, int glyph, float size, float scale,
                              int *advance, int *lsb, int *x0, int *y0, int *x1, int *y1, int *isColor)
{
    FT_Error ftError;
    FT_GlyphSlot ftGlyph;
    FONS_NOTUSED(scale);

    float desired_px = size;  // Approximate point size as pixels

    if (!FT_HAS_COLOR(font->font)) {
        // Greyscale font: Render at adjusted size to match vertical metrics to 'size'
        if (FT_IS_SCALABLE(font->font)) {
            float hmetric = (float)(font->font->ascender - font->font->descender);
            float upemf = (float)font->font->units_per_EM;
            if (hmetric > 0.0f && upemf > 0.0f) {
                FT_UInt ps = (FT_UInt)(desired_px * upemf / hmetric + 0.5f);
                ftError = FT_Set_Pixel_Sizes(font->font, 0, ps);
            } else {
                ftError = FT_Set_Pixel_Sizes(font->font, 0, (FT_UInt)desired_px);
            }
        } else {
            // Non-scalable bitmap greyscale: Select closest strike to desired size
            int closest = 0;
            int min_diff = INT_MAX;
            if (font->font->num_fixed_sizes > 0) {
                for (int k = 0; k < font->font->num_fixed_sizes; k++) {
                    int h = (int)(font->font->available_sizes[k].height);
                    int diff = abs(h - (int)desired_px);
                    if (diff < min_diff) {
                        min_diff = diff;
                        closest = k;
                    }
                }
                ftError = FT_Select_Size(font->font, closest);
            } else {
                ftError = FT_Err_Invalid_Argument;
            }
        }
        if (ftError) return 0;
    } else {
        // Color font: Retain existing logic
        if (FT_IS_SCALABLE(font->font)) {
            float emSize = (float)font->font->units_per_EM / (float)(font->font->ascender - font->font->descender);
            FT_UInt ps = (FT_UInt)(desired_px * emSize);
            if (ps > 1024) ps = 1024;
            ftError = FT_Set_Pixel_Sizes(font->font, 0, ps);
        } else {
            ftError = FT_Select_Size(font->font, 0);
        }
        if (ftError) return 0;
    }

    ftError = FT_Load_Glyph(font->font, glyph, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_COLOR);
    if (ftError) return 0;

    // Reject if bitmap too large
    if (font->font->glyph->bitmap.width > 1024 || font->font->glyph->bitmap.rows > 1024) return 0;

    ftGlyph = font->font->glyph;
    //*advance = (int)(ftGlyph->advance.x >> 6) + 1
    *advance = (int)((ftGlyph->advance.x + 32) >> 6);;
    *lsb = (int)ftGlyph->metrics.horiBearingX;
    *x0 = ftGlyph->bitmap_left;
    *x1 = *x0 + ftGlyph->bitmap.width;
    *y0 = -ftGlyph->bitmap_top;
    *y1 = *y0 + ftGlyph->bitmap.rows;
    *isColor = (ftGlyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) ? 1 : 0;

    return 1;
}

int fons__tt_buildGlyphBitmap__OLD(FONSttFontImpl *font, int glyph, float size, float scale,
                              int *advance, int *lsb, int *x0, int *y0, int *x1, int *y1, int *isColor)
{
    FT_Error ftError;
    FT_GlyphSlot ftGlyph;
    FT_Fixed advFixed;
    FONS_NOTUSED(scale);

    float desired_px = size;  // Approximate point size as pixels, adjust if needed

    if (FT_IS_SCALABLE(font->font)) {
        float emSize = (float)font->font->units_per_EM / (float)(font->font->ascender - font->font->descender);
        FT_UInt ps = (FT_UInt)(desired_px * emSize);
        if (ps > 1024) ps = 1024;
        ftError = FT_Set_Pixel_Sizes(font->font, 0, ps);
        if (ftError) return 0;
    } else {
        // Bitmap font, select closest strike
        int closest = 0;
        int min_diff = INT_MAX;
        if (font->font->num_fixed_sizes == 0) return 0;  // No strikes
        for (int k = 0; k < font->font->num_fixed_sizes; k++) {
            int h = font->font->available_sizes[k].height;  // or y_ppem >> 6 if in 26.6
            int diff = abs(h - (int)desired_px);
            if (diff < min_diff) {
                min_diff = diff;
                closest = k;
            }
        }
        ftError = FT_Select_Size(font->font, closest);
        if (ftError) return 0;
    }

    ftError = FT_Load_Glyph(font->font, glyph, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_COLOR);
    if (ftError) return 0;

    // Reject if bitmap too large
    if (font->font->glyph->bitmap.width > 1024 || font->font->glyph->bitmap.rows > 1024) return 0;

    //ftError = FT_Get_Advance(font->font, glyph, FT_LOAD_NO_SCALE, &advFixed);
    if (ftError) return 0;
    ftGlyph = font->font->glyph;
    *advance = (int)(ftGlyph->advance.x >> 6)+1;
    *lsb = (int)ftGlyph->metrics.horiBearingX;
    *x0 = ftGlyph->bitmap_left;
    *x1 = *x0 + ftGlyph->bitmap.width;
    *y0 = -ftGlyph->bitmap_top;
    *y1 = *y0 + ftGlyph->bitmap.rows;
    *isColor = (ftGlyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) ? 1 : 0;

    return 1;
}

int fons__tt_buildGlyphBitmap__OLD2(FONSttFontImpl *font, int glyph, float size, float scale,
                              int *advance, int *lsb, int *x0, int *y0, int *x1, int *y1, int *isColor)
{
    FT_Error ftError;
    FT_GlyphSlot ftGlyph;
    FT_Fixed advFixed;
    FONS_NOTUSED(scale);

    ftError = FT_Set_Pixel_Sizes(font->font, 0, (FT_UInt)(size * (float)font->font->units_per_EM / (float)(font->font->ascender - font->font->descender)));
    if (ftError) return 0;
    ftError = FT_Load_Glyph(font->font, glyph, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_COLOR);
    if (ftError) return 0;
    //ftError = FT_Get_Advance(font->font, glyph, FT_LOAD_NO_SCALE, &advFixed);
    //if (ftError) return 0;
    ftGlyph = font->font->glyph;
    *advance = (int)(ftGlyph->advance.x >> 6)+1;
    *lsb = (int)ftGlyph->metrics.horiBearingX;
    *x0 = ftGlyph->bitmap_left;
    *x1 = *x0 + ftGlyph->bitmap.width;
    *y0 = -ftGlyph->bitmap_top;
    *y1 = *y0 + ftGlyph->bitmap.rows;
    *isColor = (ftGlyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) ? 1 : 0;
	if(*isColor != 0) {
		dprintf("fontstash.h: GOT COLOR FONT!! x0=%d, y0=%d, x1=%d, y1=%d, lsb=%d\n",
           *x0, *y0, *x1, *y1, *lsb);
	}
    return 1;
}


void fons__tt_renderGlyphBitmap(FONSttFontImpl *font, unsigned char *output, int outWidth, int outHeight, int outStride,
                                float scaleX, float scaleY, int glyph)
{
    FT_GlyphSlot ftGlyph = font->font->glyph;

    int w = ftGlyph->bitmap.width;
    int h = ftGlyph->bitmap.rows;
    unsigned char *src = ftGlyph->bitmap.buffer;
    unsigned char *dst = output;
    int srcStride = ftGlyph->bitmap.pitch;
    if (ftGlyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        if (outWidth == w && outHeight == h) {
            for (int y = 0; y < h; ++y) {
                unsigned char *s = src + y * srcStride;
                uint32_t *d = (uint32_t *)(dst + y * outStride);
                for (int x = 0; x < w; ++x) {
                    uint32_t val = ((uint32_t)(*s++)) << 24;
                    *d++ = val | 0x00FFFFFFU;
                }
            }
        } else {
            // Resize case for GRAY (though unlikely, but for completeness)
            int temp_bytes = w * h;
            unsigned char *temp = (unsigned char *)malloc(temp_bytes);
            if (!temp) {
                return;
            }
            // Copy to temp (grayscale)
            for (int y = 0; y < h; ++y) {
                unsigned char *s = src + y * srcStride;
                unsigned char *d_temp = temp + y * w;
                for (int x = 0; x < w; ++x) {
                    d_temp[x] = s[x];
                }
            }
            // Resize to output (but output is RGBA, so need to convert during resize? Wait, stbir for 1 channel to 4? But stbir_uint8_linear is for channels=4
            // Actually, for resize gray, better to resize gray, then convert to RGBA in output.
            // But to simplify, since rare, assume no resize for gray, or implement properly.
            // For now, assume out==in for gray.
            free(temp);
        }
    } else if (ftGlyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        int bytes_per_pixel = 4;
        if (outWidth == w && outHeight == h) {
            for (int y = 0; y < h; ++y) {
                uint32_t *s = (uint32_t *)(src + y * srcStride);
                uint32_t *d = (uint32_t *)(dst + y * outStride);
                for (int x = 0; x < w; ++x) {
                    uint32_t u = *s++;
                    u = (u & 0xFF00FF00U) | ((u & 0x000000FFU) << 16) | ((u & 0x00FF0000U) >> 16);
                    *d++ = u;
                }
            }
        } else {
            // Resize case for BGRA
            int temp_bytes = w * h * bytes_per_pixel;
            unsigned char *temp = (unsigned char *)malloc(temp_bytes);
            if (!temp) {
                return;
            }
            // Copy and swap to temp (packed RGBA)
            for (int y = 0; y < h; ++y) {
                unsigned char *s = src + y * srcStride;
                unsigned char *d_temp = temp + y * w * bytes_per_pixel;
                uint32_t *su = (uint32_t *)s;
                uint32_t *du = (uint32_t *)d_temp;
                for (int x = 0; x < w; ++x) {
                    uint32_t u = *su++;
                    u = (u & 0xFF00FF00U) | ((u & 0x000000FFU) << 16) | ((u & 0x00FF0000U) >> 16);
                    *du++ = u;
                }
            }
            // Resize to output
            if (!stbir_resize_uint8_linear(temp, w, h, w * bytes_per_pixel,
                                           output, outWidth, outHeight, outStride, bytes_per_pixel)) {
                free(temp);
                return;
            }
            free(temp);
        }
    }
}

int fons__tt_getGlyphKernAdvance(FONSttFontImpl *font, int glyph1, int glyph2)
{
    FT_Vector ftKerning;
    FT_Get_Kerning(font->font, glyph1, glyph2, FT_KERNING_DEFAULT, &ftKerning);
    return (int)((ftKerning.x + 32) >> 6);  // Round up and convert to integer
}

#ifndef FONS_SCRATCH_BUF_SIZE
#	define FONS_SCRATCH_BUF_SIZE 96000
#endif
#ifndef FONS_HASH_LUT_SIZE
#	define FONS_HASH_LUT_SIZE 256
#endif
#ifndef FONS_INIT_FONTS
#	define FONS_INIT_FONTS 4
#endif
#ifndef FONS_INIT_GLYPHS
#	define FONS_INIT_GLYPHS 256
#endif
#ifndef FONS_INIT_ATLAS_NODES
#	define FONS_INIT_ATLAS_NODES 256
#endif
#ifndef FONS_VERTEX_COUNT
#	define FONS_VERTEX_COUNT 1024
#endif
#ifndef FONS_MAX_STATES
#	define FONS_MAX_STATES 20
#endif
#ifndef FONS_MAX_FALLBACKS
#	define FONS_MAX_FALLBACKS 20
#endif

static unsigned int fons__hashint(unsigned int a)
{
    a += ~(a<<15);
    a ^=  (a>>10);
    a +=  (a<<3);
    a ^=  (a>>6);
    a += ~(a<<11);
    a ^=  (a>>16);
    return a;
}

static int fons__mini(int a, int b)
{
    return a < b ? a : b;
}

static int fons__maxi(int a, int b)
{
    return a > b ? a : b;
}

struct FONSglyph
{
    unsigned int codepoint;
    int index;
    int next;
    short size, blur;
    short x0,y0,x1,y1;
    short xadv,xoff,yoff;
    short isColor;
};
typedef struct FONSglyph FONSglyph;

struct FONSfont
{
    FONSttFontImpl font;
    char name[64];
    unsigned char* data;
    int dataSize;
    unsigned char freeData;
    float ascender;
    float descender;
    float lineh;
    FONSglyph* glyphs;
    int cglyphs;
    int nglyphs;
    int lut[FONS_HASH_LUT_SIZE];
    int fallbacks[FONS_MAX_FALLBACKS];
    int nfallbacks;
};
typedef struct FONSfont FONSfont;

struct FONSstate
{
    int font;
    int align;
    float size;
    unsigned int color;
    float blur;
    float spacing;
};
typedef struct FONSstate FONSstate;

struct FONSatlasNode {
    short x, y, width;
};
typedef struct FONSatlasNode FONSatlasNode;

struct FONSatlas
{
    int width, height;
    FONSatlasNode* nodes;
    int nnodes;
    int cnodes;
};
typedef struct FONSatlas FONSatlas;

struct FONScontext
{
    FONSparams params;
    float itw,ith;
    unsigned char* texData;
    int dirtyRect[4];
    FONSfont** fonts;
    FONSatlas* atlas;
    int cfonts;
    int nfonts;
    float verts[FONS_VERTEX_COUNT*2];
    float tcoords[FONS_VERTEX_COUNT*2];
    unsigned int colors[FONS_VERTEX_COUNT];
    int nverts;
    unsigned char* scratch;
    int nscratch;
    FONSstate states[FONS_MAX_STATES];
    int nstates;
    void (*handleError)(void* uptr, int error, int val);
    void* errorUptr;
};

// Copyright (c) 2008-2010 Bjoern Hoehrmann <bjoern@hoehrmann.de>
// See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.

#define FONS_UTF8_ACCEPT 0
#define FONS_UTF8_REJECT 12

static unsigned int fons__decutf8(unsigned int* state, unsigned int* codep, unsigned int byte)
{
    static const unsigned char utf8d[] = {
        // The first part of the table maps bytes to character classes that
        // to reduce the size of the transition table and create bitmasks.
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
        10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,

        // The second part is a transition table that maps a combination
        // of a state of the automaton and a character class to a state.
        0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
        12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
        12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
        12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
        12,36,12,12,12,12,12,12,12,12,12,12,
    };

    unsigned int type = utf8d[byte];

    *codep = (*state != FONS_UTF8_ACCEPT) ?
        (byte & 0x3fu) | (*codep << 6) :
        (0xff >> type) & (byte);

    *state = utf8d[256 + *state + type];
    return *state;
}

// Atlas based on Skyline Bin Packer by Jukka Jylänki

static void fons__deleteAtlas(FONSatlas* atlas)
{
    if (atlas == NULL) return;
    if (atlas->nodes != NULL) free(atlas->nodes);
    free(atlas);
}

static FONSatlas* fons__allocAtlas(int w, int h, int nnodes)
{
    FONSatlas* atlas = NULL;

    // Allocate memory for the font stash.
    atlas = (FONSatlas*)malloc(sizeof(FONSatlas));
    if (atlas == NULL) goto error;
    memset(atlas, 0, sizeof(FONSatlas));

    atlas->width = w;
    atlas->height = h;

    // Allocate space for skyline nodes
    atlas->nodes = (FONSatlasNode*)malloc(sizeof(FONSatlasNode) * nnodes);
    if (atlas->nodes == NULL) goto error;
    memset(atlas->nodes, 0, sizeof(FONSatlasNode) * nnodes);
    atlas->nnodes = 0;
    atlas->cnodes = nnodes;

    // Init root node.
    atlas->nodes[0].x = 0;
    atlas->nodes[0].y = 0;
    atlas->nodes[0].width = (short)w;
    atlas->nnodes++;

    return atlas;

error:
    if (atlas) fons__deleteAtlas(atlas);
    return NULL;
}

static int fons__atlasInsertNode(FONSatlas* atlas, int idx, int x, int y, int w)
{
    int i;
    // Insert node
    if (atlas->nnodes+1 > atlas->cnodes) {
        atlas->cnodes = atlas->cnodes == 0 ? 8 : atlas->cnodes * 2;
        atlas->nodes = (FONSatlasNode*)realloc(atlas->nodes, sizeof(FONSatlasNode) * atlas->cnodes);
        if (atlas->nodes == NULL)
            return 0;
    }
    for (i = atlas->nnodes; i > idx; i--)
        atlas->nodes[i] = atlas->nodes[i-1];
    atlas->nodes[idx].x = (short)x;
    atlas->nodes[idx].y = (short)y;
    atlas->nodes[idx].width = (short)w;
    atlas->nnodes++;

    return 1;
}

static void fons__atlasRemoveNode(FONSatlas* atlas, int idx)
{
    int i;
    if (atlas->nnodes == 0) return;
    for (i = idx; i < atlas->nnodes-1; i++)
        atlas->nodes[i] = atlas->nodes[i+1];
    atlas->nnodes--;
}

static void fons__atlasExpand(FONSatlas* atlas, int w, int h)
{
    // Insert node for empty space
    if (w > atlas->width)
        fons__atlasInsertNode(atlas, atlas->nnodes, atlas->width, 0, w - atlas->width);
    atlas->width = w;
    atlas->height = h;
}

static void fons__atlasReset(FONSatlas* atlas, int w, int h)
{
    atlas->width = w;
    atlas->height = h;
    atlas->nnodes = 0;

    // Init root node.
    atlas->nodes[0].x = 0;
    atlas->nodes[0].y = 0;
    atlas->nodes[0].width = (short)w;
    atlas->nnodes++;
}

static int fons__atlasAddSkylineLevel(FONSatlas* atlas, int idx, int x, int y, int w, int h)
{
    int i;

    // Insert new node
    if (fons__atlasInsertNode(atlas, idx, x, y+h, w) == 0)
        return 0;

    // Delete skyline segments that fall under the shadow of the new segment.
    for (i = idx+1; i < atlas->nnodes; i++) {
        if (atlas->nodes[i].x < atlas->nodes[i-1].x + atlas->nodes[i-1].width) {
            int shrink = atlas->nodes[i-1].x + atlas->nodes[i-1].width - atlas->nodes[i].x;
            atlas->nodes[i].x += (short)shrink;
            atlas->nodes[i].width -= (short)shrink;
            if (atlas->nodes[i].width <= 0) {
                fons__atlasRemoveNode(atlas, i);
                i--;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    // Merge same height skyline segments that are next to each other.
    for (i = 0; i < atlas->nnodes-1; i++) {
        if (atlas->nodes[i].y == atlas->nodes[i+1].y) {
            atlas->nodes[i].width += atlas->nodes[i+1].width;
            fons__atlasRemoveNode(atlas, i+1);
            i--;
        }
    }

    return 1;
}

static int fons__atlasRectFits(FONSatlas* atlas, int i, int w, int h)
{
    // Checks if there is enough space at the location of skyline span 'i',
    // and return the max height of all skyline spans under that at that location,
    // (think tetris block being dropped at that position). Or -1 if no space found.
    int x = atlas->nodes[i].x;
    int y = atlas->nodes[i].y;
    int spaceLeft;
    if (x + w > atlas->width)
        return -1;
    spaceLeft = w;
    while (spaceLeft > 0) {
        if (i == atlas->nnodes) return -1;
        y = fons__maxi(y, atlas->nodes[i].y);
        if (y + h > atlas->height) return -1;
        spaceLeft -= atlas->nodes[i].width;
        ++i;
    }
    return y;
}

static int fons__atlasAddRect(FONSatlas* atlas, int rw, int rh, int* rx, int* ry)
{
    int besth = atlas->height, bestw = atlas->width, besti = -1;
    int bestx = -1, besty = -1, i;

    // Bottom left fit heuristic.
    for (i = 0; i < atlas->nnodes; i++) {
        int y = fons__atlasRectFits(atlas, i, rw, rh);
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

    // Perform the actual packing.
    if (fons__atlasAddSkylineLevel(atlas, besti, bestx, besty, rw, rh) == 0)
        return 0;

    *rx = bestx;
    *ry = besty;

	dprintf("fons__atlasAddRect atlas=%p rw=%d rh=%d *rx=%d *ry=%d\n",
			atlas, rw, rh, *rx, *ry);


    return 1;
}

static void fons__addWhiteRect(FONScontext* stash, int w, int h)
{
    int x, y, gx, gy;
    unsigned char* dst;
    if (fons__atlasAddRect(stash->atlas, w, h, &gx, &gy) == 0)
        return;

    // Rasterize
    dst = &stash->texData[(gx + gy * stash->params.width) * 4];
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            dst[x*4 + 0] = 255;
            dst[x*4 + 1] = 255;
            dst[x*4 + 2] = 255;
            dst[x*4 + 3] = 255;
        }
        dst += stash->params.width * 4;
    }

    stash->dirtyRect[0] = fons__mini(stash->dirtyRect[0], gx);
    stash->dirtyRect[1] = fons__mini(stash->dirtyRect[1], gy);
    stash->dirtyRect[2] = fons__maxi(stash->dirtyRect[2], gx+w);
    stash->dirtyRect[3] = fons__maxi(stash->dirtyRect[3], gy+h);
}

FONScontext* fonsCreateInternal(FONSparams* params)
{
    FONScontext* stash = NULL;

    // Allocate memory for the font stash.
    stash = (FONScontext*)malloc(sizeof(FONScontext));
    if (stash == NULL) goto error;
    memset(stash, 0, sizeof(FONScontext));

    stash->params = *params;

    // Allocate scratch buffer.
    stash->scratch = (unsigned char*)malloc(FONS_SCRATCH_BUF_SIZE);
    if (stash->scratch == NULL) goto error;

    // Initialize implementation library
    if (!fons__tt_init(stash)) goto error;

    if (stash->params.renderCreate != NULL) {
        if (stash->params.renderCreate(stash->params.userPtr, stash->params.width, stash->params.height) == 0)
            goto error;
    }

    stash->atlas = fons__allocAtlas(stash->params.width, stash->params.height, FONS_INIT_ATLAS_NODES);
    if (stash->atlas == NULL) goto error;

    // Allocate space for fonts.
    stash->fonts = (FONSfont**)malloc(sizeof(FONSfont*) * FONS_INIT_FONTS);
    if (stash->fonts == NULL) goto error;
    memset(stash->fonts, 0, sizeof(FONSfont*) * FONS_INIT_FONTS);
    stash->cfonts = FONS_INIT_FONTS;
    stash->nfonts = 0;

    // Create texture for the cache.
    stash->itw = 1.0f/stash->params.width;
    stash->ith = 1.0f/stash->params.height;
    stash->texData = (unsigned char*)malloc(stash->params.width * stash->params.height * 4);
    if (stash->texData == NULL) goto error;
    memset(stash->texData, 0, stash->params.width * stash->params.height * 4);
	dprintf("fonsCreateInternal: texData=%p width=%d height=%d\n",
		stash->texData, stash->params.width, stash->params.height);

    stash->dirtyRect[0] = stash->params.width;
    stash->dirtyRect[1] = stash->params.height;
    stash->dirtyRect[2] = 0;
    stash->dirtyRect[3] = 0;

    // Add white rect at 0,0 for debug drawing.
    fons__addWhiteRect(stash, 2,2);

    fonsPushState(stash);
    fonsClearState(stash);

    return stash;

error:
    fonsDeleteInternal(stash);
    return NULL;
}

static FONSstate* fons__getState(FONScontext* stash)
{
    return &stash->states[stash->nstates-1];
}

int fonsAddFallbackFont(FONScontext* stash, int base, int fallback)
{
    FONSfont* baseFont = stash->fonts[base];
    if (baseFont->nfallbacks < FONS_MAX_FALLBACKS) {
        baseFont->fallbacks[baseFont->nfallbacks++] = fallback;
        return 1;
    }
    return 0;
}

void fonsSetSize(FONScontext* stash, float size)
{
    fons__getState(stash)->size = size;
}

void fonsSetColor(FONScontext* stash, unsigned int color)
{
    fons__getState(stash)->color = color;
}

void fonsSetSpacing(FONScontext* stash, float spacing)
{
    fons__getState(stash)->spacing = spacing;
}

void fonsSetBlur(FONScontext* stash, float blur)
{
    fons__getState(stash)->blur = blur;
}

void fonsSetAlign(FONScontext* stash, int align)
{
    fons__getState(stash)->align = align;
}

void fonsSetFont(FONScontext* stash, int font)
{
    fons__getState(stash)->font = font;
}

void fonsPushState(FONScontext* stash)
{
    if (stash->nstates >= FONS_MAX_STATES) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, FONS_STATES_OVERFLOW, 0);
        return;
    }
    if (stash->nstates > 0)
        memcpy(&stash->states[stash->nstates], &stash->states[stash->nstates-1], sizeof(FONSstate));
    stash->nstates++;
}

void fonsPopState(FONScontext* stash)
{
    if (stash->nstates <= 1) {
        if (stash->handleError)
            stash->handleError(stash->errorUptr, FONS_STATES_UNDERFLOW, 0);
        return;
    }
    stash->nstates--;
}

void fonsClearState(FONScontext* stash)
{
    FONSstate* state = fons__getState(stash);
    state->size = 12.0f;
    state->color = 0xffffffff;
    state->font = 0;
    state->blur = 0;
    state->spacing = 0;
    state->align = FONS_ALIGN_LEFT | FONS_ALIGN_BASELINE;
}

static void fons__freeFont(FONSfont* font)
{
    if (font == NULL) return;
    if (font->glyphs) free(font->glyphs);
    if (font->freeData && font->data) free(font->data);
    free(font);
}

static int fons__allocFont(FONScontext* stash)
{
    FONSfont* font = NULL;
    if (stash->nfonts+1 > stash->cfonts) {
        stash->cfonts = stash->cfonts == 0 ? 8 : stash->cfonts * 2;
        stash->fonts = (FONSfont**)realloc(stash->fonts, sizeof(FONSfont*) * stash->cfonts);
        if (stash->fonts == NULL)
            return -1;
    }
    font = (FONSfont*)malloc(sizeof(FONSfont));
    if (font == NULL) goto error;
    memset(font, 0, sizeof(FONSfont));

    font->glyphs = (FONSglyph*)malloc(sizeof(FONSglyph) * FONS_INIT_GLYPHS);
    if (font->glyphs == NULL) goto error;
    font->cglyphs = FONS_INIT_GLYPHS;
    font->nglyphs = 0;

    stash->fonts[stash->nfonts++] = font;
    return stash->nfonts-1;

error:
    fons__freeFont(font);

    return FONS_INVALID;
}

int fonsAddFont(FONScontext* stash, const char* name, const char* path)
{
    FILE* fp = 0;
    int dataSize = 0;
    size_t readed;
    unsigned char* data = NULL;

    // Read in the font data.
    fp = fopen(path, "rb");
    if (fp == NULL) goto error;
    fseek(fp,0,SEEK_END);
    dataSize = (int)ftell(fp);
    fseek(fp,0,SEEK_SET);
    data = (unsigned char*)malloc(dataSize);
    if (data == NULL) goto error;
    readed = fread(data, 1, dataSize, fp);
    fclose(fp);
    fp = 0;
    if (readed != dataSize) goto error;
	dprintf("FONT ADDED: [%s]\n", name);

    return fonsAddFontMem(stash, name, data, dataSize, 1);

error:
    if (data) free(data);
    if (fp) fclose(fp);
    return FONS_INVALID;
}

int fonsAddFontMem(FONScontext* stash, const char* name, unsigned char* data, int dataSize, int freeData)
{
    int i, ascent, descent, fh, lineGap;
    FONSfont* font;

    int idx = fons__allocFont(stash);
    if (idx == FONS_INVALID)
        return FONS_INVALID;

    font = stash->fonts[idx];

    strncpy(font->name, name, sizeof(font->name));
    font->name[sizeof(font->name)-1] = '\0';

    // Init hash lookup.
    for (i = 0; i < FONS_HASH_LUT_SIZE; ++i)
        font->lut[i] = -1;

    // Read in the font data.
    font->dataSize = dataSize;
    font->data = data;
    font->freeData = (unsigned char)freeData;

    // Init font
    stash->nscratch = 0;
    if (!fons__tt_loadFont(stash, &font->font, data, dataSize)) goto error;

    // Store normalized line height. The real line height is got
    // by multiplying the lineh by font size.
    fons__tt_getFontVMetrics( &font->font, &ascent, &descent, &lineGap);
    fh = ascent - descent;
    font->ascender = (float)ascent / (float)fh;
    font->descender = (float)descent / (float)fh;
    font->lineh = (float)(fh + lineGap) / (float)fh;

	dprintf("fonsAddFontMem: ascender=%f descender=%f lineheight=%f\n",
			font->ascender, font->descender, font->lineh);

    return idx;

error:
    fons__freeFont(font);
    stash->nfonts--;
    return FONS_INVALID;
}

int fonsGetFontByName(FONScontext* s, const char* name)
{
    int i;
    for (i = 0; i < s->nfonts; i++) {
        if (strcmp(s->fonts[i]->name, name) == 0)
            return i;
    }
	dprintf("FONT NOT FOUND %s\n", name);
    return FONS_INVALID;
}


static FONSglyph* fons__allocGlyph(FONSfont* font)
{
    if (font->nglyphs+1 > font->cglyphs) {
        font->cglyphs = font->cglyphs == 0 ? 8 : font->cglyphs * 2;
        font->glyphs = (FONSglyph*)realloc(font->glyphs, sizeof(FONSglyph) * font->cglyphs);
        if (font->glyphs == NULL) return NULL;
    }
    font->nglyphs++;
    return &font->glyphs[font->nglyphs-1];
}


// Based on Exponential blur, Jani Huhtanen, 2006

#define APREC 16
#define ZPREC 7

static void fons__blurCols(unsigned char* dst, int w, int h, int dstStride, int alpha)
{
    int x, y;
    for (y = 0; y < h; y++) {
        int z = 0; // force zero border
        for (x = 1; x < w; x++) {
            z += (alpha * (((int)(dst[x]) << ZPREC) - z)) >> APREC;
            dst[x] = (unsigned char)(z >> ZPREC);
        }
        dst[w-1] = 0; // force zero border
        z = 0;
        for (x = w-2; x >= 0; x--) {
            z += (alpha * (((int)(dst[x]) << ZPREC) - z)) >> APREC;
            dst[x] = (unsigned char)(z >> ZPREC);
        }
        dst[0] = 0; // force zero border
        dst += dstStride;
    }
}

static void fons__blurRows(unsigned char* dst, int w, int h, int dstStride, int alpha)
{
    int x, y;
    for (x = 0; x < w; x++) {
        int z = 0; // force zero border
        for (y = dstStride; y < h*dstStride; y += dstStride) {
            z += (alpha * (((int)(dst[y]) << ZPREC) - z)) >> APREC;
            dst[y] = (unsigned char)(z >> ZPREC);
        }
        dst[(h-1)*dstStride] = 0; // force zero border
        z = 0;
        for (y = (h-2)*dstStride; y >= 0; y -= dstStride) {
            z += (alpha * (((int)(dst[y]) << ZPREC) - z)) >> APREC;
            dst[y] = (unsigned char)(z >> ZPREC);
        }
        dst[0] = 0; // force zero border
        dst++;
    }
}


static void fons__blur(FONScontext* stash, unsigned char* dst, int w, int h, int dstStride, int blur)
{
    int alpha;
    float sigma;
    (void)stash;

    if (blur < 1)
        return;
    // Calculate the alpha such that 90% of the kernel is within the radius. (Kernel extends to infinity)
    sigma = (float)blur * 0.57735f; // 1 / sqrt(3)
    alpha = (int)((1<<APREC) * (1.0f - expf(-2.3f / (sigma+1.0f))));
    fons__blurRows(dst, w, h, dstStride, alpha);
    fons__blurCols(dst, w, h, dstStride, alpha);
    fons__blurRows(dst, w, h, dstStride, alpha);
    fons__blurCols(dst, w, h, dstStride, alpha);
//	fons__blurrows(dst, w, h, dstStride, alpha);
//	fons__blurcols(dst, w, h, dstStride, alpha);
}

static FONSglyph* fons__getGlyph(FONScontext* stash, FONSfont* font, unsigned int codepoint,
                                 short isize, short iblur, int bitmapOption)
{
    int i, g, advance, lsb, x0, y0, x1, y1, gw, gh, gx, gy, x, y;
    int isColor = 0;
    float scale;
    FONSglyph* glyph = NULL;
    unsigned int h;
    float size = isize/10.0f;
    int pad, added;
    unsigned char* dst;
    FONSfont* renderFont = font;

    if (isize < 2) return NULL;
    if (iblur > 20) iblur = 20;
    pad = iblur + 2;

    // Reset allocator.
    stash->nscratch = 0;

    // Find code point and size.
    h = fons__hashint(codepoint) & (FONS_HASH_LUT_SIZE-1);
    i = font->lut[h];
    while (i != -1) {
        if (font->glyphs[i].codepoint == codepoint && font->glyphs[i].size == isize && font->glyphs[i].blur == iblur) {
            glyph = &font->glyphs[i];
            if (bitmapOption == FONS_GLYPH_BITMAP_OPTIONAL || (glyph->x0 >= 0 && glyph->y0 >= 0)) {
                return glyph;
            }
            break;
        }
        i = font->glyphs[i].next;
    }

    // Create a new glyph or rasterize bitmap data for a cached glyph.
    g = fons__tt_getGlyphIndex(&font->font, codepoint);
    if (g == 0) {
        for (i = 0; i < font->nfallbacks; ++i) {
            FONSfont* fallbackFont = stash->fonts[font->fallbacks[i]];
            int fallbackIndex = fons__tt_getGlyphIndex(&fallbackFont->font, codepoint);
            if (fallbackIndex != 0) {
                g = fallbackIndex;
                renderFont = fallbackFont;
                break;
            }
        }
    }
    scale = fons__tt_getPixelHeightScale(&renderFont->font, size);
    // Fix for invalid scale
    if (!isfinite(scale) || scale <= 0.0f) {
        scale = 1.0f;
    }
    int ret;
    if (g == 0) {
        ret = 0;
        advance = (int)size;
        lsb = 0;
        x0 = 0;
        y0 = 0;
        x1 = 0;
        y1 = 0;
        isColor = 0;
    } else {
        ret = fons__tt_buildGlyphBitmap(&renderFont->font, g, size, scale, &advance, &lsb, &x0, &y0, &x1, &y1, &isColor);
        if (!ret) {
			// Fallback: use FT_Get_Advance for reliable metrics (especially monospace fonts)
			FT_Fixed advFixed = 0;
			FT_Get_Advance(renderFont->font.font, g, FT_LOAD_DEFAULT, &advFixed);
			advance = (int)((advFixed * size) / (float)renderFont->font.font->units_per_EM);
			if (advance <= 0) advance = (int)size;
			lsb = 0;
			x0 = 0;
			y0 = 0;
			x1 = advance;
			y1 = (int)size;
			isColor = 0;
        }
    }
    dprintf("freeType glyph: size=%f scale=%f advance=%d lsb=%d x0=%d y0=%d x1=%d y1=%d isColor=%d\n",
             size, scale, advance, lsb, x0, y0, x1, y1, isColor);

    // Store original metrics before potential resize
    int orig_x0 = x0;
    int orig_y0 = y0;
    int orig_x1 = x1;
    int orig_y1 = y1;
    int orig_advance = advance;

    int orig_w = x1 - x0;
    int orig_h = y1 - y0;

    short calc_xoff, calc_yoff;
    short calc_xadv;
    int target_w = 0, target_h = 0;

    // Handle color vs. grayscale paths
    if (isColor) {
        // For color glyphs: fallback to grayscale if invalid dimensions
        if (orig_w <= 0 || orig_h <= 0) {
            isColor = 0;
        }
    }
    if (isColor) {
        // Color path: Use uniform scale for offsets/advance; resize bitmap to scaled dimensions (aspect-preserving)
        float scaled_w = (float)orig_w * scale;
        float scaled_h = (float)orig_h * scale;
        target_w = fmax(1, (int)(scaled_w + 0.5f));  // Round to nearest int, min 1
        target_h = fmax(1, (int)(scaled_h + 0.5f));
        gw = target_w + pad * 2;
        gh = target_h + pad * 2;
        calc_xoff = (short)((float)orig_x0 * scale - (float)pad);
        calc_yoff = (short)((float)orig_y0 * scale - (float)pad);
        calc_xadv = (short)(orig_advance * scale * 10.0f);  // Scale advance for color
    } else {
        // Grayscale path: No resize; direct padding. Rendered at logical size, no scale multiply
        gw = orig_w + pad * 2;
        gh = orig_h + pad * 2;
        calc_xoff = (short)(orig_x0 - pad);
        calc_yoff = (short)(orig_y0 - iblur - 2);  // Ignore insets for vertical positioning to reduce space above/below
        calc_xadv = (short)(orig_advance * 10.0f);  // No scale multiply for greyscale
    }

    // Ensure atlas is large enough for the glyph only if needed
    int needAtlas = ((isColor ? (target_w > 0) : (orig_w > 0 || orig_h > 0)) && (bitmapOption == FONS_GLYPH_BITMAP_REQUIRED));
    if (needAtlas) {
        if (gw > stash->params.width || gh > stash->params.height) {
            int new_w = fons__maxi(stash->params.width, gw + pad * 2);
            int new_h = fons__maxi(stash->params.height, gh + pad * 2);
            if (!fonsExpandAtlas(stash, new_w, new_h)) {
                return NULL;
            }
        }
    }

    // Determines the spot to draw glyph in the atlas.
    gx = -1;
    gy = -1;
    if (needAtlas) {
        added = fons__atlasAddRect(stash->atlas, gw, gh, &gx, &gy);
        if (added == 0 && stash->handleError != NULL) {
            stash->handleError(stash->errorUptr, FONS_ATLAS_FULL, 0);
            added = fons__atlasAddRect(stash->atlas, gw, gh, &gx, &gy);
        }
        if (added == 0) return NULL;
    }

    // Init glyph.
    if (glyph == NULL) {
        glyph = fons__allocGlyph(font);
        if (glyph == NULL) {
            return NULL;
        }
        glyph->codepoint = codepoint;
        glyph->size = isize;
        glyph->blur = iblur;
        glyph->next = font->lut[h];
        font->lut[h] = font->nglyphs - 1;
    }
    glyph->index = g;
    if (gx >= 0) {
        glyph->x0 = (short)gx;
        glyph->y0 = (short)gy;
        glyph->x1 = (short)(gx + gw);
        glyph->y1 = (short)(gy + gh);
    } else {
        // No atlas spot allocated (OPTIONAL path).  Use a negative origin
        // sentinel so that callers can detect the missing bitmap, but still
        // store the *width* and *height* of the glyph cell so that
        // fons__getQuad() can produce a correct visual bounding box.  Without
        // this, nvgTextBounds() under-reports the right edge by the width of
        // the last character on the very first call (before any glyph has
        // been rasterised into the atlas).
        glyph->x0 = -1;
        glyph->y0 = -1;
        glyph->x1 = (short)(-1 + gw);
        glyph->y1 = (short)(-1 + gh);
    }
    glyph->xoff = calc_xoff;
    glyph->yoff = calc_yoff;
    glyph->xadv = calc_xadv;
    glyph->isColor = (short)isColor;

    if (isColor) {
        dprintf("Glyph Render Bounds: x0=%d y0=%d x1=%d y1=%d xoff=%d yoff=%d xadv=%d\n",
               glyph->x0, glyph->y0, glyph->x1, glyph->y1, glyph->xoff, glyph->yoff, glyph->xadv);
    }

    if (bitmapOption == FONS_GLYPH_BITMAP_OPTIONAL) {
        return glyph;
    }
    // Clear glyph area in texture only if allocated
    if (gx >= 0) {
        unsigned char* gdst = &stash->texData[(glyph->x0 + glyph->y0 * stash->params.width) * 4];
        int atlasStride = stash->params.width * 4;
        for (y = 0; y < gh; y++) {
            memset(gdst + y * atlasStride, 0, gw * 4);
        }
    }

    FT_GlyphSlot ftGlyphSlot = renderFont->font.font->glyph;

    if (isColor && target_w > 0 && gx >= 0) {
        // Render color glyph to padded position in atlas with aspect-preserving resize
        unsigned char* inner_dst = &stash->texData[(glyph->x0 + pad + (glyph->y0 + pad) * stash->params.width) * 4];
        int atlasStride = stash->params.width * 4;
        fons__tt_renderGlyphBitmap(&renderFont->font, inner_dst, target_w, target_h, atlasStride, scale, scale, g);

        dprintf("fons__getGlyph: Rendered codepoint %u to scaled color texture (%dx%d)\n", codepoint, target_w, target_h);
    } else if (!isColor && orig_w > 0 && orig_h > 0 && gx >= 0) {
        // Grayscale glyph path (unchanged)
        int monoSize = gw * gh;
        if (stash->nscratch + monoSize > FONS_SCRATCH_BUF_SIZE) {
            if (stash->handleError)
                stash->handleError(stash->errorUptr, FONS_SCRATCH_FULL, stash->nscratch + monoSize);
            return glyph;
        }
        unsigned char* mono = stash->scratch + stash->nscratch;
        stash->nscratch += monoSize;
        memset(mono, 0, monoSize);

        unsigned char* mdst = mono + pad + pad * gw;
        int mstride = gw;
        for (y = 0; y < ftGlyphSlot->bitmap.rows; y++) {
            unsigned char* s = ftGlyphSlot->bitmap.buffer + y * ftGlyphSlot->bitmap.pitch;
            for (x = 0; x < ftGlyphSlot->bitmap.width; x++) {
                mdst[y * mstride + x] = s[x];
            }
        }

        if (iblur > 0) {
            fons__blur(stash, mono, gw, gh, mstride, iblur);
        }

        unsigned char* gdst = &stash->texData[(glyph->x0 + glyph->y0 * stash->params.width) * 4];
        int atlasStride2 = stash->params.width * 4;
        for (y = 0; y < gh; y++) {
            unsigned char* row = mono + y * gw;
            unsigned char* tdst = gdst + y * atlasStride2;
            for (x = 0; x < gw; x++) {
                unsigned char val = row[x];
                tdst[x * 4 + 0] = 255;
                tdst[x * 4 + 1] = 255;
                tdst[x * 4 + 2] = 255;
                tdst[x * 4 + 3] = val;
            }
        }
    } // else missing glyph, no render

    // Update dirty rectangle only if allocated
    if (gx >= 0) {
        stash->dirtyRect[0] = fons__mini(stash->dirtyRect[0], glyph->x0);
        stash->dirtyRect[1] = fons__mini(stash->dirtyRect[1], glyph->y0);
        stash->dirtyRect[2] = fons__maxi(stash->dirtyRect[2], glyph->x1);
        stash->dirtyRect[3] = fons__maxi(stash->dirtyRect[3], glyph->y1);
    }

    return glyph;
}

static void fons__getQuad(FONScontext* stash, FONSfont* font,
                           int prevGlyphIndex, FONSglyph* glyph,
                           float scale, float spacing, float* x, float* y, FONSquad* q)
{
    float rx,ry,xoff,yoff,x0,y0,x1,y1;

    if (prevGlyphIndex != -1) {
        float adv = fons__tt_getGlyphKernAdvance(&font->font, prevGlyphIndex, glyph->index) * scale;
        *x += (int)(adv + spacing + 0.5f);
    }

    // Each glyph has 2px border to allow good interpolation,
    // one pixel to prevent leaking, and one to allow good interpolation for rendering.
    // Inset the texture region by one pixel for correct interpolation.
    xoff = (short)(glyph->xoff+1);
    yoff = (short)(glyph->yoff+1);
    x0 = (float)(glyph->x0+1);
    y0 = (float)(glyph->y0+1);
    x1 = (float)(glyph->x1-1);
    y1 = (float)(glyph->y1-1);

    if (stash->params.flags & FONS_ZERO_TOPLEFT) {
        rx = (float)(int)(*x + xoff);
        ry = (float)(int)(*y + yoff);

        q->x0 = rx;
        q->y0 = ry;
        q->x1 = rx + x1 - x0;
        q->y1 = ry + y1 - y0;

        q->s0 = x0 * stash->itw;
        q->t0 = y0 * stash->ith;
        q->s1 = x1 * stash->itw;
        q->t1 = y1 * stash->ith;
    } else {
        rx = (float)(int)(*x + xoff);
        ry = (float)(int)(*y - yoff);

        q->x0 = rx;
        q->y0 = ry;
        q->x1 = rx + x1 - x0;
        q->y1 = ry - y1 + y0;

        q->s0 = x0 * stash->itw;
        q->t0 = y0 * stash->ith;
        q->s1 = x1 * stash->itw;
        q->t1 = y1 * stash->ith;
    }

    *x += (int)(glyph->xadv / 10.0f + 0.5f);
	dprintf("fons__getQuads: scale=%f spacing=%f *x=%f *y=%f, q={%f,%f,%f,%f,%f,%f,%f,%f}\n",
		scale, spacing, *x, *y,
        q->x0, q->y0, q->x1, q->y1, q->s0, q->t0, q->s1, q->t1);

}

static void fons__flush(FONScontext* stash)
{
    // Flush texture
    if (stash->dirtyRect[0] < stash->dirtyRect[2] && stash->dirtyRect[1] < stash->dirtyRect[3]) {
        if (stash->params.renderUpdate != NULL)
            stash->params.renderUpdate(stash->params.userPtr, stash->dirtyRect, stash->texData);
        // Reset dirty rect
        stash->dirtyRect[0] = stash->params.width;
        stash->dirtyRect[1] = stash->params.height;
        stash->dirtyRect[2] = 0;
        stash->dirtyRect[3] = 0;
    }

    // Flush triangles
    if (stash->nverts > 0) {
        if (stash->params.renderDraw != NULL)
            stash->params.renderDraw(stash->params.userPtr, stash->verts, stash->tcoords, stash->colors, stash->nverts);
        stash->nverts = 0;
    }
}

static __inline void fons__vertex(FONScontext* stash, float x, float y, float s, float t, unsigned int c)
{
    stash->verts[stash->nverts*2+0] = x;
    stash->verts[stash->nverts*2+1] = y;
    stash->tcoords[stash->nverts*2+0] = s;
    stash->tcoords[stash->nverts*2+1] = t;
    stash->colors[stash->nverts] = c;
    stash->nverts++;
}

static float fons__getVertAlign(FONScontext* stash, FONSfont* font, int align, short isize)
{
    if (stash->params.flags & FONS_ZERO_TOPLEFT) {
        if (align & FONS_ALIGN_TOP) {
            return font->ascender * (float)isize/10.0f;
        } else if (align & FONS_ALIGN_MIDDLE) {
            return (font->ascender + font->descender) / 2.0f * (float)isize/10.0f;
        } else if (align & FONS_ALIGN_BASELINE) {
            return 0.0f;
        } else if (align & FONS_ALIGN_BOTTOM) {
            return font->descender * (float)isize/10.0f;
        }
    } else {
        if (align & FONS_ALIGN_TOP) {
            return -font->ascender * (float)isize/10.0f;
        } else if (align & FONS_ALIGN_MIDDLE) {
            return -(font->ascender + font->descender) / 2.0f * (float)isize/10.0f;
        } else if (align & FONS_ALIGN_BASELINE) {
            return 0.0f;
        } else if (align & FONS_ALIGN_BOTTOM) {
            return -font->descender * (float)isize/10.0f;
        }
    }
    return 0.0;
}


float fonsDrawText(FONScontext* stash,
                   float x, float y,
                   const char* str, const char* end)
{
    FONSstate* state = fons__getState(stash);
    unsigned int codepoint;
    unsigned int utf8state = 0;
    FONSglyph* glyph = NULL;
    FONSquad q;
    int prevGlyphIndex = -1;
    short isize = (short)(state->size*10.0f);
    short iblur = (short)state->blur;
    float scale;
    FONSfont* font;
    float width;

    if (stash == NULL) return x;
    if (state->font < 0 || state->font >= stash->nfonts) return x;
    font = stash->fonts[state->font];
    if (font->data == NULL) return x;

    scale = fons__tt_getPixelHeightScale(&font->font, (float)isize/10.0f);

    if (end == NULL)
        end = str + strlen(str);

    // Align horizontally
    if (state->align & FONS_ALIGN_LEFT) {
        // empty
    } else if (state->align & FONS_ALIGN_RIGHT) {
        width = fonsTextBounds(stash, x,y, str, end, NULL);
        x -= width;
    } else if (state->align & FONS_ALIGN_CENTER) {
        width = fonsTextBounds(stash, x,y, str, end, NULL);
        x -= width * 0.5f;
    }
    // Align vertically.
    y += fons__getVertAlign(stash, font, state->align, isize);

    for (; str != end; ++str) {
        if (fons__decutf8(&utf8state, &codepoint, *(const unsigned char*)str))
            continue;
        glyph = fons__getGlyph(stash, font, codepoint, isize, iblur, FONS_GLYPH_BITMAP_REQUIRED);
        if (glyph != NULL) {
            fons__getQuad(stash, font, prevGlyphIndex, glyph, scale, state->spacing, &x, &y, &q);
            if (glyph->x0 >= 0) {
                if (stash->nverts+6 > FONS_VERTEX_COUNT)
                    fons__flush(stash);

                unsigned int c = glyph->isColor ? 0xffffffff : state->color;

                fons__vertex(stash, q.x0, q.y0, q.s0, q.t0, c);
                fons__vertex(stash, q.x1, q.y1, q.s1, q.t1, c);
                fons__vertex(stash, q.x1, q.y0, q.s1, q.t0, c);

                fons__vertex(stash, q.x0, q.y0, q.s0, q.t0, c);
                fons__vertex(stash, q.x0, q.y1, q.s0, q.t1, c);
                fons__vertex(stash, q.x1, q.y1, q.s1, q.t1, c);
            }
        }
        prevGlyphIndex = glyph != NULL ? glyph->index : -1;
    }
    fons__flush(stash);

    return x;
}

int fonsTextIterInit(FONScontext* stash, FONStextIter* iter,
                     float x, float y, const char* str, const char* end, int bitmapOption)
{
    FONSstate* state = fons__getState(stash);
    float width;

    memset(iter, 0, sizeof(*iter));

    if (stash == NULL) return 0;
    if (state->font < 0 || state->font >= stash->nfonts) return 0;
    iter->font = stash->fonts[state->font];
    if (iter->font->data == NULL) return 0;

    iter->isize = (short)(state->size*10.0f);
    iter->iblur = (short)state->blur;
    iter->scale = fons__tt_getPixelHeightScale(&iter->font->font, (float)iter->isize/10.0f);

    // Align horizontally
    if (state->align & FONS_ALIGN_LEFT) {
        // empty
    } else if (state->align & FONS_ALIGN_RIGHT) {
        width = fonsTextBounds(stash, x,y, str, end, NULL);
        x -= width;
    } else if (state->align & FONS_ALIGN_CENTER) {
        width = fonsTextBounds(stash, x,y, str, end, NULL);
        x -= width * 0.5f;
    }
    // Align vertically.
    y += fons__getVertAlign(stash, iter->font, state->align, iter->isize);

    if (end == NULL)
        end = str + strlen(str);

    iter->x = iter->nextx = x;
    iter->y = iter->nexty = y;
    iter->spacing = state->spacing;
    iter->str = str;
    iter->next = str;
    iter->end = end;
    iter->codepoint = 0;
    iter->prevGlyphIndex = -1;
    iter->bitmapOption = bitmapOption;

    return 1;
}

int fonsTextIterNext(FONScontext* stash, FONStextIter* iter, FONSquad* quad)
{
    FONSglyph* glyph = NULL;
    const char* str = iter->next;
    iter->str = iter->next;

    if (str == iter->end)
        return 0;

    for (; str != iter->end; str++) {
        if (fons__decutf8(&iter->utf8state, &iter->codepoint, *(const unsigned char*)str))
            continue;
        str++;
        // Get glyph and quad
        iter->x = iter->nextx;
        iter->y = iter->nexty;
        glyph = fons__getGlyph(stash, iter->font, iter->codepoint, iter->isize, iter->iblur, iter->bitmapOption);
        if (glyph != NULL) {
            fons__getQuad(stash, iter->font, iter->prevGlyphIndex, glyph, iter->scale, iter->spacing, &iter->nextx, &iter->nexty, quad);
            if (glyph->x0 < 0) {
                // Invalid quad for missing glyph
                quad->x0 = quad->x1 = iter->nextx;
                quad->y0 = quad->y1 = iter->nexty;
                quad->s0 = quad->t0 = quad->s1 = quad->t1 = 0.0f;
            }
        } else {
            // Rare: alloc failed, approximate advance
            iter->nextx += iter->scale * (float)iter->isize / 10.0f;
            iter->nexty = iter->y;
            quad->x0 = quad->x1 = iter->nextx;
            quad->y0 = quad->y1 = iter->nexty;
            quad->s0 = quad->t0 = quad->s1 = quad->t1 = 0.0f;
        }
        iter->prevGlyphIndex = glyph != NULL ? glyph->index : -1;
        break;
    }
    iter->next = str;

    return 1;
}

void fonsDrawDebug(FONScontext* stash, float x, float y)
{
    int i;
    int w = stash->params.width;
    int h = stash->params.height;
    float u = w == 0 ? 0 : (1.0f / w);
    float v = h == 0 ? 0 : (1.0f / h);

    if (stash->nverts+6+6 > FONS_VERTEX_COUNT)
        fons__flush(stash);

    // Draw background
    fons__vertex(stash, x+0, y+0, u, v, 0x0fffffff);
    fons__vertex(stash, x+w, y+h, u, v, 0x0fffffff);
    fons__vertex(stash, x+w, y+0, u, v, 0x0fffffff);

    fons__vertex(stash, x+0, y+0, u, v, 0x0fffffff);
    fons__vertex(stash, x+0, y+h, u, v, 0x0fffffff);
    fons__vertex(stash, x+w, y+h, u, v, 0x0fffffff);

    // Draw texture
    fons__vertex(stash, x+0, y+0, 0, 0, 0xffffffff);
    fons__vertex(stash, x+w, y+h, 1, 1, 0xffffffff);
    fons__vertex(stash, x+w, y+0, 1, 0, 0xffffffff);

    fons__vertex(stash, x+0, y+0, 0, 0, 0xffffffff);
    fons__vertex(stash, x+0, y+h, 0, 1, 0xffffffff);
    fons__vertex(stash, x+w, y+h, 1, 1, 0xffffffff);

    // Drawbug draw atlas
    for (i = 0; i < stash->atlas->nnodes; i++) {
        FONSatlasNode* n = &stash->atlas->nodes[i];

        if (stash->nverts+6 > FONS_VERTEX_COUNT)
            fons__flush(stash);

        fons__vertex(stash, x+n->x+0, y+n->y+0, u, v, 0xc00000ff);
        fons__vertex(stash, x+n->x+n->width, y+n->y+1, u, v, 0xc00000ff);
        fons__vertex(stash, x+n->x+n->width, y+n->y+0, u, v, 0xc00000ff);

        fons__vertex(stash, x+n->x+0, y+n->y+0, u, v, 0xc00000ff);
        fons__vertex(stash, x+n->x+0, y+n->y+1, u, v, 0xc00000ff);
        fons__vertex(stash, x+n->x+n->width, y+n->y+1, u, v, 0xc00000ff);
    }

    fons__flush(stash);
}

float fonsTextBounds(FONScontext* stash,
                     float x, float y,
                     const char* str, const char* end,
                     float* bounds)
{
    FONSstate* state = fons__getState(stash);
    unsigned int codepoint;
    unsigned int utf8state = 0;
    FONSquad q;
    FONSglyph* glyph = NULL;
    int prevGlyphIndex = -1;
    short isize = (short)(state->size*10.0f);
    short iblur = (short)state->blur;
    float scale;
    FONSfont* font;
    float startx, advance;
    float minx, miny, maxx, maxy;

    if (stash == NULL) return 0;
    if (state->font < 0 || state->font >= stash->nfonts) return 0;
    font = stash->fonts[state->font];
    if (font->data == NULL) return 0;

    scale = fons__tt_getPixelHeightScale(&font->font, (float)isize/10.0f);

    // Align vertically.
    y += fons__getVertAlign(stash, font, state->align, isize);

    minx = maxx = x;
    miny = maxy = y;
    startx = x;

    if (end == NULL)
        end = str + strlen(str);

    for (; str != end; ++str) {
        if (fons__decutf8(&utf8state, &codepoint, *(const unsigned char*)str))
            continue;
        glyph = fons__getGlyph(stash, font, codepoint, isize, iblur, FONS_GLYPH_BITMAP_OPTIONAL);
        if (glyph != NULL) {
            fons__getQuad(stash, font, prevGlyphIndex, glyph, scale, state->spacing, &x, &y, &q);
            if (q.x0 < minx) minx = q.x0;
            if (q.x1 > maxx) maxx = q.x1;
            if (stash->params.flags & FONS_ZERO_TOPLEFT) {
                if (q.y0 < miny) miny = q.y0;
                if (q.y1 > maxy) maxy = q.y1;
            } else {
                if (q.y1 < miny) miny = q.y1;
                if (q.y0 > maxy) maxy = q.y0;
            }
        } else {
			dprintf("fonsTextBounds: glyph is null\n");
		}
        prevGlyphIndex = glyph != NULL ? glyph->index : -1;
    }

    advance = x - startx;

    // Align horizontally
    if (state->align & FONS_ALIGN_LEFT) {
        // empty
    } else if (state->align & FONS_ALIGN_RIGHT) {
        minx -= advance;
        maxx -= advance;
    } else if (state->align & FONS_ALIGN_CENTER) {
        minx -= advance * 0.5f;
        maxx -= advance * 0.5f;
    }

    if (bounds) {
        bounds[0] = minx;
        bounds[1] = miny;
        bounds[2] = maxx;
        bounds[3] = maxy;
    }

    return advance;
}

void fonsVertMetrics(FONScontext* stash,
                     float* ascender, float* descender, float* lineh)
{
    FONSfont* font;
    FONSstate* state = fons__getState(stash);
    short isize;

    if (stash == NULL) return;
    if (state->font < 0 || state->font >= stash->nfonts) return;
    font = stash->fonts[state->font];
    isize = (short)(state->size*10.0f);
    if (font->data == NULL) return;

    if (ascender)
        *ascender = font->ascender*isize/10.0f;
    if (descender)
        *descender = font->descender*isize/10.0f;
    if (lineh)
        *lineh = font->lineh*isize/12.0f; // Originally 10.0f, This fixes the lineheight bug
}

void fonsLineBounds(FONScontext* stash, float y, float* miny, float* maxy)
{
    FONSfont* font;
    FONSstate* state = fons__getState(stash);
    short isize;

    if (stash == NULL) return;
    if (state->font < 0 || state->font >= stash->nfonts) return;
    font = stash->fonts[state->font];
    isize = (short)(state->size*10.0f);
    if (font->data == NULL) return;

    y += fons__getVertAlign(stash, font, state->align, isize);

    if (stash->params.flags & FONS_ZERO_TOPLEFT) {
        *miny = y - font->ascender * (float)isize/10.0f;
        *maxy = *miny + font->lineh*isize/10.0f;
    } else {
        *maxy = y + font->descender * (float)isize/10.0f;
        *miny = *maxy - font->lineh*isize/10.0f;
    }
}

const unsigned char* fonsGetTextureData(FONScontext* stash, int* width, int* height)
{
    if (width != NULL)
        *width = stash->params.width;
    if (height != NULL)
        *height = stash->params.height;
    return stash->texData;
}

int fonsValidateTexture(FONScontext* stash, int* dirty)
{
    if (stash->dirtyRect[0] < stash->dirtyRect[2] && stash->dirtyRect[1] < stash->dirtyRect[3]) {
        dirty[0] = stash->dirtyRect[0];
        dirty[1] = stash->dirtyRect[1];
        dirty[2] = stash->dirtyRect[2];
        dirty[3] = stash->dirtyRect[3];
        // Reset dirty rect
        stash->dirtyRect[0] = stash->params.width;
        stash->dirtyRect[1] = stash->params.height;
        stash->dirtyRect[2] = 0;
        stash->dirtyRect[3] = 0;
        return 1;
    }
    return 0;
}

void fonsDeleteInternal(FONScontext* stash)
{
    int i;
    if (stash == NULL) return;

    if (stash->params.renderDelete)
        stash->params.renderDelete(stash->params.userPtr);

    for (i = 0; i < stash->nfonts; ++i)
        fons__freeFont(stash->fonts[i]);

    if (stash->atlas) fons__deleteAtlas(stash->atlas);
    if (stash->fonts) free(stash->fonts);
    if (stash->texData) free(stash->texData);
    if (stash->scratch) free(stash->scratch);
    free(stash);
    fons__tt_done(stash);
	dprintf("fonsDeleteInternal()\n");
}

void fonsSetErrorCallback(FONScontext* stash, void (*callback)(void* uptr, int error, int val), void* uptr)
{
    if (stash == NULL) return;
    stash->handleError = callback;
    stash->errorUptr = uptr;
}

void fonsGetAtlasSize(FONScontext* stash, int* width, int* height)
{
    if (stash == NULL) return;
    *width = stash->params.width;
    *height = stash->params.height;
}

int fonsExpandAtlas(FONScontext* stash, int width, int height)
{
    int i, maxy = 0;
    unsigned char* data = NULL;
    if (stash == NULL) return 0;

    width = fons__maxi(width, stash->params.width);
    height = fons__maxi(height, stash->params.height);

    if (width == stash->params.width && height == stash->params.height)
        return 1;

    // Flush pending glyphs.
    fons__flush(stash);

    // Create new texture
    if (stash->params.renderResize != NULL) {
        if (stash->params.renderResize(stash->params.userPtr, width, height) == 0)
            return 0;
    }
    // Copy old texture data over.
    data = (unsigned char*)malloc(width * height * 4);
    if (data == NULL)
        return 0;
    for (i = 0; i < stash->params.height; i++) {
        unsigned char* dst = &data[i*width*4];
        unsigned char* src = &stash->texData[i*stash->params.width*4];
        memcpy(dst, src, stash->params.width*4);
        if (width > stash->params.width)
            memset(dst+stash->params.width*4, 0, (width - stash->params.width)*4);
    }
    if (height > stash->params.height)
        memset(&data[stash->params.height * width * 4], 0, (height - stash->params.height) * width * 4);

    free(stash->texData);
    stash->texData = data;

    // Increase atlas size
    fons__atlasExpand(stash->atlas, width, height);

    // Add existing data as dirty.
    for (i = 0; i < stash->atlas->nnodes; i++)
        maxy = fons__maxi(maxy, stash->atlas->nodes[i].y);
    stash->dirtyRect[0] = 0;
    stash->dirtyRect[1] = 0;
    stash->dirtyRect[2] = stash->params.width;
    stash->dirtyRect[3] = maxy;

    stash->params.width = width;
    stash->params.height = height;
    stash->itw = 1.0f/stash->params.width;
    stash->ith = 1.0f/stash->params.height;

	dprintf("fonsExpandAtlas: texData=%p width=%d height=%d\n",
		stash->texData, stash->params.width, stash->params.height);

    return 1;
}

int fonsResetAtlas(FONScontext* stash, int width, int height)
{
    int i, j;
    if (stash == NULL) return 0;

    // Flush pending glyphs.
    fons__flush(stash);

    // Create new texture
    if (stash->params.renderResize != NULL) {
        if (stash->params.renderResize(stash->params.userPtr, width, height) == 0)
            return 0;
    }

    // Reset atlas
    fons__atlasReset(stash->atlas, width, height);

    // Clear texture data.
    stash->texData = (unsigned char*)realloc(stash->texData, width * height * 4);
    if (stash->texData == NULL) return 0;
    memset(stash->texData, 0, width * height * 4);

    // Reset dirty rect
    stash->dirtyRect[0] = width;
    stash->dirtyRect[1] = height;
    stash->dirtyRect[2] = 0;
    stash->dirtyRect[3] = 0;

    // Reset cached glyphs
    for (i = 0; i < stash->nfonts; i++) {
        FONSfont* font = stash->fonts[i];
        font->nglyphs = 0;
        for (j = 0; j < FONS_HASH_LUT_SIZE; j++)
            font->lut[j] = -1;
    }

    stash->params.width = width;
    stash->params.height = height;
    stash->itw = 1.0f/stash->params.width;
    stash->ith = 1.0f/stash->params.height;

    // Add white rect at 0,0 for debug drawing.
    fons__addWhiteRect(stash, 2,2);

    return 1;
}


#endif
