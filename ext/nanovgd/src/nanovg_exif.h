//
// nanovg_exif.h — JPEG EXIF orientation for the image loaders.
//
// stb_image parses only APP0 (JFIF) and APP14 (Adobe) and skips every other
// APPn, so the Orientation tag in APP1 never reaches the decoded pixels and
// photos from phones come out rotated.  nvgCreateImage*() and nanogui's
// Texture read the tag with the helpers below and transform the decoded
// pixels before upload.
//
// Image bytes are untrusted, so every read here is bounds checked; a malformed
// segment simply yields orientation 1 (no transform).
//
#ifndef NANOVG_EXIF_H
#define NANOVG_EXIF_H

#include <stdlib.h>

static inline unsigned nvg__exifU16(const unsigned char* t, int off, int le)
{
	return le ? (unsigned)t[off] | ((unsigned)t[off + 1] << 8)
	          : (unsigned)t[off + 1] | ((unsigned)t[off] << 8);
}

static inline unsigned nvg__exifU32(const unsigned char* t, int off, int le)
{
	return le ? (unsigned)t[off] | ((unsigned)t[off+1] << 8) |
	            ((unsigned)t[off+2] << 16) | ((unsigned)t[off+3] << 24)
	          : (unsigned)t[off+3] | ((unsigned)t[off+2] << 8) |
	            ((unsigned)t[off+1] << 16) | ((unsigned)t[off] << 24);
}

// Orientation tag value, 1..8 per the TIFF spec. 1 when absent or unreadable.
static inline int nvg__exifOrientation(const unsigned char* d, int n)
{
	int p;
	if (d == NULL || n < 4 || d[0] != 0xFF || d[1] != 0xD8)
		return 1;	// not a JPEG

	for (p = 2; p + 4 <= n; ) {
		unsigned marker;
		int seg, paylen, i, tn, le, ifd, count;
		const unsigned char *pay, *t;

		if (d[p] != 0xFF) return 1;		// out of sync
		while (p < n && d[p] == 0xFF) p++;	// fill bytes
		if (p >= n) return 1;
		marker = d[p++];
		// Standalone markers carry no length field.
		if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
			continue;
		if (marker == 0xD9 || marker == 0xDA) return 1;	// EOI / start of scan
		if (p + 2 > n) return 1;
		seg = (d[p] << 8) | d[p + 1];
		if (seg < 2 || p + seg > n) return 1;
		pay = d + p + 2;
		paylen = seg - 2;
		p += seg;

		// APP1 big enough for "Exif\0\0" plus a TIFF header.
		if (marker != 0xE1 || paylen < 14) continue;
		if (pay[0] != 'E' || pay[1] != 'x' || pay[2] != 'i' ||
		    pay[3] != 'f' || pay[4] != 0 || pay[5] != 0) continue;

		t = pay + 6;		// offsets in EXIF are relative to the TIFF header
		tn = paylen - 6;
		if (t[0] == 'I' && t[1] == 'I') le = 1;
		else if (t[0] == 'M' && t[1] == 'M') le = 0;
		else return 1;

		if (nvg__exifU16(t, 2, le) != 42) return 1;
		ifd = (int)nvg__exifU32(t, 4, le);
		if (ifd < 8 || ifd + 2 > tn) return 1;
		count = (int)nvg__exifU16(t, ifd, le);
		if (count < 0 || ifd + 2 + count * 12 > tn) return 1;
		for (i = 0; i < count; i++) {
			int e = ifd + 2 + i * 12;
			int v;
			if (nvg__exifU16(t, e, le) != 0x0112) continue;		// Orientation
			if (nvg__exifU16(t, e + 2, le) != 3) return 1;		// must be SHORT
			v = (int)nvg__exifU16(t, e + 8, le);			// stored inline
			return (v >= 1 && v <= 8) ? v : 1;
		}
		return 1;	// no Orientation in IFD0
	}
	return 1;
}

// Applies orient to a tightly packed *w * *h * comp buffer (comp bytes per
// pixel, 1..4), swapping *w/*h for the transposing cases. Returns a new
// malloc'd buffer the caller frees, or NULL when there is nothing to do
// (orientation 1) or allocation failed.
static inline unsigned char* nvg__exifApply(const unsigned char* src, int* w, int* h,
											int comp, int orient)
{
	int sw = *w, sh = *h, dw, dh, x, y, c, swap;
	unsigned char* out;

	if (src == NULL || orient <= 1 || orient > 8 || sw <= 0 || sh <= 0 ||
	    comp < 1 || comp > 4)
		return NULL;

	swap = orient >= 5;
	dw = swap ? sh : sw;
	dh = swap ? sw : sh;
	out = (unsigned char*)malloc((size_t)dw * dh * comp);
	if (out == NULL) return NULL;

	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			const unsigned char* s;
			unsigned char* o;
			int sx, sy;
			switch (orient) {
				case 2:  sx = sw - 1 - x; sy = y;          break;	// mirror H
				case 3:  sx = sw - 1 - x; sy = sh - 1 - y; break;	// 180
				case 4:  sx = x;          sy = sh - 1 - y; break;	// mirror V
				case 5:  sx = y;          sy = x;          break;	// transpose
				case 6:  sx = y;          sy = sh - 1 - x; break;	// 90 CW
				case 7:  sx = sw - 1 - y; sy = sh - 1 - x; break;	// anti-transpose
				default: sx = sw - 1 - y; sy = x;          break;	// 8: 270 CW
			}
			s = src + ((size_t)sy * sw + sx) * comp;
			o = out + ((size_t)y * dw + x) * comp;
			for (c = 0; c < comp; c++) o[c] = s[c];
		}
	}
	*w = dw;
	*h = dh;
	return out;
}

#endif // NANOVG_EXIF_H
