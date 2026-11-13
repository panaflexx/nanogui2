/*
    examples/imagestash_atlas_check.cpp -- headless checks for the imagestash
    atlas packers, with no window and no GL.

    Packs photo-shaped thumbnails (128 px on the long edge: 4:3, 3:2, 16:9,
    panoramas, squares and portraits, as a camera roll mixes them; then
    crops of any aspect from 1:3 to 3:1) with both packers until the atlas is full at its cap, running imgsValidate after
    every insert. Then it checks a reset leaves a valid empty atlas that
    packs the same again, that the stats add up, and that the validator
    catches broken stashes (overlap, out of bounds, a broken name table).

    Prints the fill and waste for each packer. Exit status 0 when every
    check passes.

    Usage:  imagestash_atlas_check [seed]
*/

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#define IMAGESTASH_IMPLEMENTATION
#include <imagestash.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/* Limits for the photo packer, as fractions: of the used band for the
   waste, of the atlas for the pixels */
constexpr double ENDS_CAMERA = 0.12, ENDS_ANY = 0.30;     // worst of 30: 0.083, 0.253
constexpr double PIXELS_CAMERA = 0.82, PIXELS_ANY = 0.65; // worst of 30: 0.847, 0.677
constexpr double UNDER_MAX = 0.05;                        // worst of 30: 0.034

int g_failed = 0, g_passed = 0;

void check(bool ok, const std::string &what) {
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int below(int n) { return (int) (next() % (uint32_t) n); }
};

bool g_any_aspect = false;   // crops: any aspect from 1:3 to 3:1

/* A thumbnail fitted into 128 x 128, the way ImageStashWidget decodes one */
void photo_size(Rng &rng, int &w, int &h) {
    if (g_any_aspect) {
        const float t = (float) rng.below(10001) / 10000.f;      // 0..1
        const float a = std::pow(3.f, 2.f * t - 1.f);            // 1/3..3
        if (a >= 1.f) {
            w = 128;
            h = std::max(1, (int) (128.f / a + 0.5f));
        } else {
            h = 128;
            w = std::max(1, (int) (128.f * a + 0.5f));
        }
        return;
    }
    static const float aspects[] = { 4.f / 3.f, 4.f / 3.f, 3.f / 2.f, 3.f / 2.f, 16.f / 9.f,
                                     1.f, 3.f, 3.f / 4.f, 3.f / 4.f, 2.f / 3.f, 9.f / 16.f };
    const float a = aspects[rng.below((int) (sizeof aspects / sizeof aspects[0]))];
    if (a >= 1.f) {
        w = 128;
        h = std::max(1, (int) (128.f / a + 0.5f));
    } else {
        h = 128;
        w = std::max(1, (int) (128.f * a + 0.5f));
    }
}

std::string validate(IMGcontext *s) {
    char msg[2048];
    const int n = imgsValidate(s, msg, (int) sizeof msg);
    return n == 0 ? std::string() : std::to_string(n) + " problems: " + msg;
}

void print_stats(const char *label, IMGcontext *s) {
    IMGSstats st;
    imgsGetStats(s, &st);
    const double band = std::max(1.0, (double) st.width * st.used_height);
    const double atlas = (double) st.width * st.height;
    std::printf("%s: %dx%d, %d thumbs, %d rows, band %d/%d\n", label, st.width, st.height,
                st.images, st.rows, st.used_height, st.height);
    std::printf("    of the band: pixels %.1f%%  padding %.1f%%  under shorter thumbs %.1f%%  "
                "row ends %.1f%%\n",
                100.0 * st.inner / band, 100.0 * st.padding / band, 100.0 * st.under_rows / band,
                100.0 * st.row_ends / band);
    std::printf("    of the atlas: pixels %.1f%%  free %.1f%%, %.1f MB texture, %.2f MB tables\n",
                100.0 * st.inner / atlas, 100.0 * st.free_area / atlas, st.tex_bytes / 1048576.0,
                (st.cpu_bytes - st.tex_bytes) / 1048576.0);
}

/* Fill an atlas with photos until it is full at the cap, checking each step */
int fill(IMGcontext *s, Rng &rng, std::vector<unsigned char> &pixels, const char *label,
         int start = 0) {
    int added = 0;
    std::string first_problem;
    for (int i = start;; ++i) {
        int w, h;
        photo_size(rng, w, h);
        const std::string name = "img_" + std::to_string(i);
        if (!imgsAddPixels(s, name.c_str(), pixels.data(), w, h, 0))
            break;
        ++added;
        if (first_problem.empty()) {
            first_problem = validate(s);
            if (!first_problem.empty())
                first_problem = "after " + name + " (" + std::to_string(w) + "x" + std::to_string(h) +
                                "): " + first_problem;
        }
        IMGimage *img = imgsGet(s, name.c_str());
        check(img && img->width == w && img->height == h,
              std::string(label) + ": " + name + " reads back at its size");
        imgsDeleteImage(img);
    }
    check(first_problem.empty(), std::string(label) + " stays valid while filling: " + first_problem);
    return added;
}

void check_packer(int packer, const char *label, uint32_t seed) {
    std::vector<unsigned char> pixels(128 * 128 * 4, 200);
    IMGSparams params = { packer == IMGS_PACK_PHOTO ? 2048 : 512,
                          packer == IMGS_PACK_PHOTO ? 256 : 512,
                          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, packer };
    IMGcontext *s = imgsCreateInternal(&params);
    check(s != nullptr, std::string(label) + ": created");
    if (!s)
        return;
    check(validate(s).empty(), std::string(label) + ": empty atlas is valid: " + validate(s));
    {
        IMGSstats st0;
        imgsGetStats(s, &st0);
        check(st0.images == 0 && st0.used_height == 0, std::string(label) + ": a new atlas is empty");
    }

    Rng rng(seed);
    const int added = fill(s, rng, pixels, label);
    print_stats(label, s);

    IMGSstats st;
    imgsGetStats(s, &st);
    if (packer == IMGS_PACK_PHOTO) {
        /* Every row stays open, so what is left at the row ends is about one
           part-filled row per height. Camera shapes come in a few heights;
           crops come in every height, so more rows are part-filled. The
           limits sit under the worst of 30 seeds; the one-open-row-per-shape
           packer this replaced got 61-64% pixels on the camera mix, with
           26-29% of the band left at row ends. */
        const double band = (double) st.width * st.used_height;
        const double ends = g_any_aspect ? ENDS_ANY : ENDS_CAMERA;
        const double pixels = g_any_aspect ? PIXELS_ANY : PIXELS_CAMERA;
        check(st.row_ends < ends * band,
              std::string(label) + ": row ends under " + std::to_string((int) (ends * 100)) +
                  "% of the band (" + std::to_string(100.0 * st.row_ends / band) + "%)");
        check(st.under_rows < UNDER_MAX * band, std::string(label) + ": little under shorter thumbs (" +
                                                    std::to_string(100.0 * st.under_rows / band) + "%)");
        check(st.inner > pixels * (double) st.width * st.height,
              std::string(label) + ": over " + std::to_string((int) (pixels * 100)) +
                  "% of the atlas is pixels (" +
                  std::to_string(100.0 * st.inner / ((double) st.width * st.height)) + "%)");
    }
    imgsGetStats(s, &st);
    check(st.images == added, std::string(label) + ": stats count every image");
    check(st.height == IMGS_ATLAS_MAX, std::string(label) + ": full only at the cap");
    check(st.packed == st.inner + st.padding, std::string(label) + ": packed = pixels + padding");
    if (packer == IMGS_PACK_PHOTO) {
        const long long band = (long long) st.width * st.used_height;
        check(st.packed + st.under_rows + st.row_ends == band,
              std::string(label) + ": the band adds up (" + std::to_string(st.packed + st.under_rows +
              st.row_ends) + " of " + std::to_string(band) + ")");
        check(st.under_rows >= 0 && st.row_ends >= 0, std::string(label) + ": no negative waste");
        check(st.free_area + band == (long long) st.width * st.height,
              std::string(label) + ": band + free = atlas");
    }

    /* The same name again is kept, not packed twice */
    check(imgsAddPixels(s, "img_0", pixels.data(), 10, 10, 0) == 1,
          std::string(label) + ": a repeated name is accepted");
    imgsGetStats(s, &st);
    check(st.images == added, std::string(label) + ": a repeated name is not packed again");

    /* Reset keeps the size and packs as much again */
    imgsResetAtlas(s, st.width, st.height);
    imgsGetStats(s, &st);
    check(st.images == 0 && validate(s).empty(), std::string(label) + ": reset is empty and valid");
    Rng again(seed);
    const int refill = fill(s, again, pixels, label);
    /* Photo rows pack the same either way. Skyline packs about the same
       (within 1%, either way) into an atlas that starts at full size as
       into one that doubled. */
    check(packer == IMGS_PACK_PHOTO ? refill == added : refill * 100 >= added * 99,
          std::string(label) + ": a reset atlas takes the same photos again (" +
                               std::to_string(refill) + " of " + std::to_string(added) + ")");

    /* Half the photos after a reset, then more: a compaction followed by
       scrolling on, as the widget does it */
    imgsResetAtlas(s, st.width, st.height);
    Rng half(seed);
    for (int i = 0; i < added / 2; ++i) {
        int w, h;
        photo_size(half, w, h);
        imgsAddPixels(s, ("img_" + std::to_string(i)).c_str(), pixels.data(), w, h, 0);
    }
    const int more = fill(s, half, pixels, label, added / 2);
    check(more > added / 3, std::string(label) + ": a half-compacted atlas still takes " +
                                std::to_string(more) + " more");

    imgsDeleteInternal(s);
}

/* The validator has to see what it is for */
void check_validator() {
    std::vector<unsigned char> pixels(128 * 128 * 4, 200);
    IMGSparams params = { 2048, 512, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, IMGS_PACK_PHOTO };
    IMGcontext *s = imgsCreateInternal(&params);
    for (int i = 0; i < 40; ++i)
        imgsAddPixels(s, ("img_" + std::to_string(i)).c_str(), pixels.data(), i % 2 ? 128 : 96,
                      i % 2 ? 96 : 128, 0);
    check(validate(s).empty(), "validator: a packed atlas is valid: " + validate(s));

    IMGSimageImpl saved = s->images[3];
    s->images[3].x = s->images[2].x;
    s->images[3].y = s->images[2].y;
    check(!validate(s).empty(), "validator: sees two images overlap");
    s->images[3] = saved;

    const short x5 = s->images[5].x;
    s->images[5].x = (short) (s->params.width - 10);
    check(!validate(s).empty(), "validator: sees an image past the right edge");
    s->images[5].x = x5;
    imgsResetAtlas(s, 2048, 512);
    for (int i = 0; i < 40; ++i)
        imgsAddPixels(s, ("img_" + std::to_string(i)).c_str(), pixels.data(), 128, 96, 0);
    check(validate(s).empty(), "validator: refilled atlas is valid");

    const int bucket = (int) (imgs__hashstr(s->images[7].name) % IMGS_HASH_LUT_SIZE);
    const int head = s->lut[bucket];
    s->lut[bucket] = -1;
    check(!validate(s).empty(), "validator: sees an image the name table lost");
    s->lut[bucket] = head;

    const int next_y = s->atlas->next_y;
    s->atlas->next_y = 50;
    check(!validate(s).empty(), "validator: sees images below the band");
    s->atlas->next_y = next_y;

    check(validate(s).empty(), "validator: restored atlas is valid again");

    s->atlas->rows[1].y += 1;
    check(!validate(s).empty(), "validator: sees rows that do not tile the band");
    s->atlas->rows[1].y -= 1;

    s->atlas->rows[0].x += 10;
    check(!validate(s).empty(), "validator: sees a gap at a row's end");
    s->atlas->rows[0].x -= 10;

    s->atlas->rows[0].height -= 20;
    s->atlas->rows[1].y -= 20;
    check(!validate(s).empty(), "validator: sees an image taller than its row");
    s->atlas->rows[0].height += 20;
    s->atlas->rows[1].y += 20;

    check(validate(s).empty(), "validator: valid once more");
    imgsDeleteInternal(s);
}

} // namespace

int main(int argc, char **argv) {
    const uint32_t seed = argc > 1 ? (uint32_t) std::strtoul(argv[1], nullptr, 10) : 12345u;
    check_packer(IMGS_PACK_PHOTO, "photo", seed);
    check_packer(IMGS_PACK_SKYLINE, "skyline", seed);
    g_any_aspect = true;
    check_packer(IMGS_PACK_PHOTO, "photo, any aspect", seed);
    check_packer(IMGS_PACK_SKYLINE, "skyline, any aspect", seed);
    g_any_aspect = false;
    check_validator();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
