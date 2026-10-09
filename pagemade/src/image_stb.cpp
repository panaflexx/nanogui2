/*
 * stb_image for the headless test. The app already gets these symbols from
 * nanovgd (nanovg.c), so this translation unit is only linked into the test.
 */
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
