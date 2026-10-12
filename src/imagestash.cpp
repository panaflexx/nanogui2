/*
    src/imagestash.cpp -- Scrollable image asset library widget backed by
    the NanoVG imagestash texture atlas.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/imagestash.h>
#include <nanogui/screen.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/opengl.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>

extern "C" {
#include <imagestash.h>
#include <nanovg_exif.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
}

/// Minimum interval between view updates while bulk-loading thumbnails.
#define IMGSW_REFRESH_INTERVAL std::chrono::milliseconds(500)

namespace {

/// Load an image file, following the nvgCreateImage() pattern: read the
/// file up front so the EXIF scan and the decoder see the same bytes.
/// Returns the decoded RGBA pixels (stb-allocated) plus the EXIF
/// orientation (1 = none); the caller decides when to apply the transform.
unsigned char *imgsw__load(const std::string &path, int *w, int *h,
                           int *orient, uint64_t *file_size) {
    *orient = 1;
    *file_size = 0;

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return nullptr;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) { fclose(fp); return nullptr; }
    unsigned char *blob = (unsigned char *) malloc((size_t) len);
    if (!blob) { fclose(fp); return nullptr; }
    bool ok = fread(blob, 1, (size_t) len, fp) == (size_t) len;
    fclose(fp);
    if (!ok) { free(blob); return nullptr; }

    int n;
    unsigned char *pixels = stbi_load_from_memory(blob, (int) len, w, h, &n, 4);
    if (pixels) {
        *orient = nvg__exifOrientation(blob, (int) len);
        *file_size = (uint64_t) len;
    }
    free(blob);
    return pixels;
}

/// Apply EXIF orientation in place (frees and replaces the buffer when a
/// transform is needed).  Safe no-op for orientation 1.
unsigned char *imgsw__apply_orient(unsigned char *pixels, int *w, int *h, int orient) {
    if (orient <= 1) return pixels;
    unsigned char *rot = nvg__exifApply(pixels, w, h, 4, orient);
    if (rot) { stbi_image_free(pixels); return rot; }
    return pixels;
}

/// Downscale (aspect preserving) to fit within max_w x max_h.  Frees and
/// replaces the buffer when resized; returns it unchanged if it already
/// fits or the resize fails.
unsigned char *imgsw__fit(unsigned char *pixels, int *w, int *h, int max_w, int max_h) {
    if (max_w <= 0 || max_h <= 0 || (*w <= max_w && *h <= max_h))
        return pixels;
    float scale = std::min((float) max_w / *w, (float) max_h / *h);
    int nw = std::max(1, (int) (*w * scale));
    int nh = std::max(1, (int) (*h * scale));
    unsigned char *resized = (unsigned char *) malloc((size_t) nw * nh * 4);
    if (!resized ||
        !stbir_resize_uint8_linear(pixels, *w, *h, 0, resized, nw, nh, 0, STBIR_RGBA)) {
        free(resized);
        return pixels;
    }
    stbi_image_free(pixels);
    *w = nw;
    *h = nh;
    return resized;
}

/// Colors taken from the screen theme. A null theme keeps the original dark sheet.
struct ImgswColors {
    nanogui::Color sheet;
    nanogui::Color scrim;
    nanogui::Color caption;
    nanogui::Color heading;
    nanogui::Color rule;
    nanogui::Color label;
    nanogui::Color accent;
    nanogui::Color failed;
    nanogui::Color placeholder;
};

ImgswColors imgsw_colors(const nanogui::Theme *theme) {
    using nanogui::Color;
    ImgswColors c;
    if (theme) {
        c.sheet = theme->m_screen_background;
        c.scrim = c.sheet;
        c.caption = theme->m_text_muted;
        c.heading = theme->m_text_heading;
        /* Hairline keeps the old rule's strength, in the theme's text color. */
        c.rule = theme->m_text_color;
        c.rule.a() *= 70.f / 255.f;
        c.label = theme->m_text_color;
        c.accent = theme->m_accent_color;
        c.failed = theme->m_danger_color;
        c.placeholder = theme->m_window_fill_unfocused;
    } else {
        c.sheet = Color(28, 28, 32, 255);
        c.scrim = Color(0, 0, 0, 255);
        c.caption = Color(255, 255, 255, 128);
        c.heading = Color(255, 255, 255, 230);
        c.rule = Color(255, 255, 255, 70);
        c.label = Color(255, 255, 255, 200);
        c.accent = Color(70, 130, 255, 255);
        c.failed = Color(120, 40, 40, 255);
        c.placeholder = Color(60, 60, 70, 160);
    }
    return c;
}

nanogui::Color imgsw_alpha(nanogui::Color c, float a) {
    c.a() *= a;
    return c;
}

} // anonymous namespace

NAMESPACE_BEGIN(nanogui)

ImageStashWidget::ImageStashWidget(Widget *parent, int thumb_size)
    : Widget(parent), m_thumb_size(thumb_size) {
    m_nvg_context = screen()->nvg_context();
    if (!m_nvg_context)
        throw std::runtime_error("ImageStashWidget: NanoVG context not initialized");

    IMGSparams params = { 512, 512, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    m_stash = imgsCreateInternal(&params);
    if (!m_stash)
        throw std::runtime_error("ImageStashWidget: failed to create imagestash");

    imgsGetAtlasSize(m_stash, &m_atlas_width, &m_atlas_height);
    m_atlas_image = nvgCreateImageRGBA(m_nvg_context, m_atlas_width, m_atlas_height, 0,
                                       imgsGetTextureData(m_stash, nullptr, nullptr));
    if (m_atlas_image == 0) {
        imgsDeleteInternal(m_stash);
        throw std::runtime_error("ImageStashWidget: failed to create NanoVG atlas image");
    }

    pthread_mutex_init(&m_mutex, nullptr);
    pthread_cond_init(&m_cond, nullptr);
    pthread_mutex_init(&m_highres_mutex, nullptr);
    pthread_cond_init(&m_highres_cond, nullptr);

    pthread_create(&m_loader_thread, nullptr, load_thumbnails, this);
    pthread_create(&m_highres_thread, nullptr, load_high_res, this);
}

ImageStashWidget::~ImageStashWidget() {
    /* Stop the thumbnail loader */
    pthread_mutex_lock(&m_mutex);
    m_stop = true;
    pthread_cond_broadcast(&m_cond);
    pthread_mutex_unlock(&m_mutex);
    pthread_join(m_loader_thread, nullptr);

    for (auto &p : m_pending)
        stbi_image_free(p.pixels);
    m_pending.clear();

    for (auto &kv : m_thumb_cache)
        stbi_image_free(kv.second.pixels);
    m_thumb_cache.clear();

    /* Stop the high-res loader (finishes any decode in flight first) */
    pthread_mutex_lock(&m_highres_mutex);
    m_highres_stop = true;
    pthread_cond_broadcast(&m_highres_cond);
    pthread_mutex_unlock(&m_highres_mutex);
    pthread_join(m_highres_thread, nullptr);

    release_high_res();

    pthread_cond_destroy(&m_highres_cond);
    pthread_mutex_destroy(&m_highres_mutex);
    pthread_cond_destroy(&m_cond);
    pthread_mutex_destroy(&m_mutex);

    if (m_stash)
        imgsDeleteInternal(m_stash);
    if (m_atlas_image != 0)
        nvgDeleteImage(m_nvg_context, m_atlas_image);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void ImageStashWidget::add_image(const std::string &path) {
    add_images({ path });
}

void ImageStashWidget::add_entry(const Entry &entry) {
    PendingItem pi;
    pi.photo = entry;
    if (pi.photo.name.empty() && !pi.photo.path.empty())
        pi.photo.name = std::filesystem::path(pi.photo.path).filename().string();
    pthread_mutex_lock(&m_mutex);
    m_pending_items.push_back(std::move(pi));
    m_layout_dirty = true;
    pthread_mutex_unlock(&m_mutex);
    set_needs_redraw();
}

void ImageStashWidget::set_loader(const Loader &loader) {
    pthread_mutex_lock(&m_mutex);
    m_loader = loader;
    pthread_mutex_unlock(&m_mutex);
}

void ImageStashWidget::set_badge_callback(const BadgeCallback &cb) {
    m_badge = cb;
    set_needs_redraw();
}

void ImageStashWidget::add_images(const std::vector<std::string> &paths) {
    /* Entries are queued here and appended to m_entries by the UI thread in
       rebuild_layout(), so draw() never sees the vector reallocate.
       NB: file_size is intentionally NOT stat'ed here -- on slow (network)
       filesystems the extra round trip per file would stall the caller.
       The loader thread fills it in instead. */
    std::vector<PendingItem> items;
    items.reserve(paths.size());
    for (const auto &path : paths) {
        if (path.empty())
            continue;            /* an empty path marks a section */
        PendingItem pi;
        pi.photo.path = path;
        pi.photo.name = std::filesystem::path(path).filename().string();
        items.push_back(std::move(pi));
    }

    pthread_mutex_lock(&m_mutex);
    for (auto &pi : items)
        m_pending_items.push_back(std::move(pi));
    m_layout_dirty = true;
    pthread_mutex_unlock(&m_mutex);
    set_needs_redraw();
}

void ImageStashWidget::add_section(const std::string &text, SectionAlign align, bool rule) {
    PendingItem pi;
    pi.section = true;
    pi.text  = text;
    pi.align = (int) align;
    pi.rule  = rule;
    pthread_mutex_lock(&m_mutex);
    m_pending_items.push_back(std::move(pi));
    m_layout_dirty = true;
    pthread_mutex_unlock(&m_mutex);
    set_needs_redraw();
}

void ImageStashWidget::insert_section(int item_index, const std::string &text,
                                      SectionAlign align, bool rule) {
    PendingItem pi;
    pi.section = true;
    pi.text  = text;
    pi.align = (int) align;
    pi.rule  = rule;
    pi.insert_pos = item_index;
    pthread_mutex_lock(&m_mutex);
    m_pending_items.push_back(std::move(pi));
    m_layout_dirty = true;
    pthread_mutex_unlock(&m_mutex);
    set_needs_redraw();
}

void ImageStashWidget::clear() {
    pthread_mutex_lock(&m_mutex);
    m_queue.clear();
    m_pending_items.clear();
    for (auto &p : m_pending)
        stbi_image_free(p.pixels);
    m_pending.clear();
    m_entries.clear();
    m_slots.clear();
    m_generation++;          /* loads in flight belong to the old entries */
    pthread_mutex_unlock(&m_mutex);

    m_items.clear();
    m_sections.clear();
    m_item_pos.clear();
    m_item_of_entry.clear();
    m_total_height = 0.0f;
    m_layout_dirty = true;
    m_laid_out_height = -1;

    for (auto &kv : m_thumb_cache)
        stbi_image_free(kv.second.pixels);
    m_thumb_cache.clear();
    m_cache_lru.clear();

    release_high_res();
    m_fullscreen = -1;
    m_press_entry = m_last_click_entry = -1;
    set_selected(-1);
    imgsResetAtlas(m_stash, m_atlas_width, m_atlas_height);
    m_redraw = true;
    set_needs_redraw();
}

/* ------------------------------------------------------------------ */
/*  View options                                                       */
/* ------------------------------------------------------------------ */

void ImageStashWidget::set_animation_time(float seconds) {
    m_anim_ms = (int) (std::min(std::max(seconds, 0.0f), 2.0f) * 1000.0f);
}

void ImageStashWidget::set_show_labels(bool show) {
    if (m_show_labels == show)
        return;
    m_show_labels = show;
    m_layout_dirty = true;   /* label height participates in row stride */
    set_needs_redraw();
}

void ImageStashWidget::set_spacing(int x, int y) {
    m_gap_x = std::max(0, x);
    m_gap_y = std::max(0, y);
    m_layout_dirty = true;
    set_needs_redraw();
}

void ImageStashWidget::set_crop(bool crop) {
    if (m_crop == crop)
        return;
    m_crop = crop;           /* draw-time crop; no relayout or reload needed */
    set_needs_redraw();
}

/* ------------------------------------------------------------------ */
/*  Layout: photos on a fixed-cell centered grid, sections full width  */
/* ------------------------------------------------------------------ */

float ImageStashWidget::cell_stride_x() const {
    return (float) (m_thumb_size + 2 * m_border + m_gap_x);
}

float ImageStashWidget::cell_stride_y() const {
    return (float) (m_thumb_size + 2 * m_border + (m_show_labels ? m_label_h : 0) + m_gap_y);
}

float ImageStashWidget::content_width() const {
    return (float) (width() > 0 ? width()
                                : (parent() ? parent()->width() - 25 : 800));
}

int ImageStashWidget::columns() const {
    return std::max(1, (int) ((content_width() + m_gap_x) / cell_stride_x()));
}

float ImageStashWidget::grid_offset_x() const {
    float grid_w = columns() * cell_stride_x() - m_gap_x;
    return std::max(0.0f, (content_width() - grid_w) * 0.5f);
}

void ImageStashWidget::ensure_layout() const {
    /* Keyed on width, not columns: the centering offset changes with it */
    if (m_layout_dirty || m_layout_width != (int) content_width())
        const_cast<ImageStashWidget *>(this)->rebuild_layout();
}

void ImageStashWidget::rebuild_layout() {
    /* Drain cross-thread input into the UI-side item list.  Clear the dirty
       flag first so an add_* racing with this rebuild re-marks it. */
    m_layout_dirty = false;
    pthread_mutex_lock(&m_mutex);
    for (auto &pi : m_pending_items) {
        Item it;
        if (pi.section) {
            m_sections.push_back({ pi.text, pi.align, pi.rule });
            it.section = (int) m_sections.size() - 1;
        } else {
            it.entry = (int) m_entries.size();
            m_entries.push_back(std::move(pi.photo));
            m_slots.push_back(Slot());
        }
        if (pi.insert_pos >= 0 && pi.insert_pos < (int) m_items.size())
            m_items.insert(m_items.begin() + pi.insert_pos, it);
        else
            m_items.push_back(it);
    }
    m_pending_items.clear();
    pthread_mutex_unlock(&m_mutex);

    /* Compute a position for every item (O(n), only when dirty) */
    int cols = columns();
    float x0 = grid_offset_x();
    float y = header_height();
    int col = 0;

    m_item_pos.resize(m_items.size());
    m_item_of_entry.assign(m_entries.size(), -1);

    for (size_t i = 0; i < m_items.size(); ++i) {
        const Item &it = m_items[i];
        if (it.section >= 0) {
            if (col > 0) {                 /* sections force a row break */
                y += cell_stride_y();
                col = 0;
            }
            m_item_pos[i] = Vector2f(0, y);
            y += section_height();
        } else {
            if (col >= cols) {
                col = 0;
                y += cell_stride_y();
            }
            m_item_pos[i] = Vector2f(x0 + col * cell_stride_x(), y);
            m_item_of_entry[it.entry] = (int) i;
            col++;
        }
    }
    if (col > 0)
        y += cell_stride_y();
    m_total_height = y + m_gap_y;

    m_layout_width = (int) content_width();
}

void ImageStashWidget::view_rect(float &x, float &y, float &w, float &h) const {
    if (const ScrollPanel *panel = dynamic_cast<const ScrollPanel *>(parent())) {
        x = (float) -m_pos.x();
        y = (float) -m_pos.y();
        w = (float) panel->width();
        h = (float) panel->height();
    } else {
        x = y = 0.0f;
        w = (float) width();
        h = (float) height();
    }
}

/* First/last item whose row intersects the viewport (binary search on the
   y-monotonic position cache). */
void ImageStashWidget::visible_items(int &first, int &last, float margin_px) const {
    first = 1; last = 0;
    int n = (int) m_item_pos.size();
    if (n == 0)
        return;

    float vx, vy, vw, vh;
    view_rect(vx, vy, vw, vh);
    float top = vy - margin_px;
    float bottom = vy + vh + margin_px;
    float max_h = std::max(cell_stride_y(), section_height());

    /* first index with pos.y > top */
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (m_item_pos[mid].y() > top) hi = mid; else lo = mid + 1;
    }
    first = lo;
    while (first > 0 && m_item_pos[first - 1].y() + max_h > top)
        first--;
    last = first - 1;
    while (last + 1 < n && m_item_pos[last + 1].y() < bottom)
        last++;
}

int ImageStashWidget::item_at(const Vector2f &content_p) const {
    int n = (int) m_item_pos.size();
    if (n == 0)
        return -1;
    /* first index with pos.y > p.y */
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (m_item_pos[mid].y() > content_p.y()) hi = mid; else lo = mid + 1;
    }
    float box = (float) (m_thumb_size + 2 * m_border);
    float max_h = std::max(cell_stride_y(), section_height());
    for (int i = lo - 1; i >= 0 && m_item_pos[i].y() + max_h > content_p.y(); --i) {
        const Item &it = m_items[i];
        if (it.entry < 0)
            continue;   /* sections are not clickable */
        Vector2f pos = m_item_pos[i];
        /* Only the thumbnail box itself is clickable (not the label/gap) */
        if (content_p.y() - pos.y() < box &&
            content_p.x() >= pos.x() && content_p.x() - pos.x() < box)
            return i;
    }
    return -1;
}

Vector2i ImageStashWidget::preferred_size(NVGcontext *ctx) const {
    ensure_layout();
    return Vector2i((int) content_width(), (int) m_total_height);
}

/* ------------------------------------------------------------------ */
/*  Thumbnail loading pipeline                                         */
/* ------------------------------------------------------------------ */

void *ImageStashWidget::load_thumbnails(void *arg) {
    ImageStashWidget *self = (ImageStashWidget *) arg;
    auto last_post = std::chrono::steady_clock::now() - IMGSW_REFRESH_INTERVAL;

    for (;;) {
        pthread_mutex_lock(&self->m_mutex);
        while (self->m_queue.empty() && !self->m_stop)
            pthread_cond_wait(&self->m_cond, &self->m_mutex);
        if (self->m_stop) {
            pthread_mutex_unlock(&self->m_mutex);
            break;
        }
        int index = self->m_queue.front();
        self->m_queue.pop_front();
        bool last_item = self->m_queue.empty();
        Entry entry = self->m_entries[index];
        Loader loader = self->m_loader;
        int generation = self->m_generation;
        pthread_mutex_unlock(&self->m_mutex);

        /* Decode, downscale and orient without the lock: the UI thread takes
           it every frame, so holding it here would stall drawing. A loader
           result is already upright; null falls back to the file. */
        int w = 0, h = 0, orient = 1;
        uint64_t fsize = entry.file_size;
        bool from_loader = false;
        unsigned char *data = nullptr;
        if (loader)
            data = loader(entry, self->m_thumb_size, &w, &h);
        if (data) {
            from_loader = true;
        } else if (!entry.path.empty()) {
            data = imgsw__load(entry.path, &w, &h, &orient, &fsize);
        }
        int tw = w, th = h;
        if (!data) {
            const std::string &what = entry.path.empty() ? entry.name : entry.path;
            if (!what.empty())
                std::cerr << "ImageStashWidget: failed to load " << what << std::endl;
        } else {
            data = imgsw__fit(data, &tw, &th, self->m_thumb_size, self->m_thumb_size);
            if (!from_loader)
                data = imgsw__apply_orient(data, &tw, &th, orient);
        }

        pthread_mutex_lock(&self->m_mutex);
        if (self->m_stop) {
            pthread_mutex_unlock(&self->m_mutex);
            if (data) stbi_image_free(data);
            break;
        }
        if (generation != self->m_generation) {
            if (data) stbi_image_free(data);    /* entries were cleared */
        } else if (!data) {
            /* Could not decode: reported so it is not retried forever */
            self->m_pending.push_back({ index, nullptr, 0, 0, 0, 0, 0 });
        } else {
            /* Report the displayed (post-rotation) original size */
            int ow = orient >= 5 ? h : w;
            int oh = orient >= 5 ? w : h;
            self->m_pending.push_back({ index, data, ow, oh, tw, th, fsize });
        }

        /* Throttle view updates to twice a second during bulk loading;
           post immediately when the queue drains (interactive requests). */
        auto now = std::chrono::steady_clock::now();
        if (last_item || now - last_post >= IMGSW_REFRESH_INTERVAL) {
            self->set_needs_redraw();
            last_post = now;
        }
        pthread_mutex_unlock(&self->m_mutex);
    }
    return nullptr;
}

bool ImageStashWidget::sync_atlas_size() {
    /* imgsAddPixels may expand the atlas internally; keep the NVG texture
       in sync with the actual atlas size. */
    int cur_w, cur_h;
    imgsGetAtlasSize(m_stash, &cur_w, &cur_h);
    if (cur_w == m_atlas_width && cur_h == m_atlas_height)
        return false;
    nvgDeleteImage(m_nvg_context, m_atlas_image);
    m_atlas_image = nvgCreateImageRGBA(m_nvg_context, cur_w, cur_h, 0,
                                       imgsGetTextureData(m_stash, nullptr, nullptr));
    m_atlas_width = cur_w;
    m_atlas_height = cur_h;
    return true;
}

bool ImageStashWidget::add_to_atlas(int index, unsigned char *pixels, int w, int h) {
    std::string key = "img_" + std::to_string(index);
    if (imgsAddPixels(m_stash, key.c_str(), pixels, w, h, 0)) {
        sync_atlas_size();
        return true;
    }
    return false;
}

void ImageStashWidget::cache_put(int index, unsigned char *pixels, int w, int h) {
    /* Takes ownership of pixels (stbi_image_free'd on eviction/destruction). */
    auto it = m_thumb_cache.find(index);
    if (it != m_thumb_cache.end()) {
        stbi_image_free(it->second.pixels);
        it->second = { pixels, w, h };
        m_cache_lru.erase(std::find(m_cache_lru.begin(), m_cache_lru.end(), index));
    } else {
        m_thumb_cache[index] = { pixels, w, h };
    }
    m_cache_lru.push_front(index);

    while (m_thumb_cache.size() > kCacheEntries) {
        int victim = m_cache_lru.back();
        m_cache_lru.pop_back();
        auto vit = m_thumb_cache.find(victim);
        if (vit != m_thumb_cache.end()) {
            stbi_image_free(vit->second.pixels);
            m_thumb_cache.erase(vit);
        }
    }
}

void ImageStashWidget::reset_atlas() {
    /* Atlas is full at its cap: evict everything, then immediately
       re-materialize the visible window from the CPU thumbnail cache so
       the current view does not flash placeholders or re-read disks. */
    int first, last;
    visible_items(first, last, kMarginRows * cell_stride_y());

    imgsResetAtlas(m_stash, m_atlas_width, m_atlas_height);
    sync_atlas_size();

    pthread_mutex_lock(&m_mutex);
    m_queue.clear();
    for (auto &s : m_slots) {
        s.atlas_x = s.atlas_y = -1;
        s.requested = false;
    }
    pthread_mutex_unlock(&m_mutex);

    for (int i = first; i <= last; ++i) {
        int e = m_items[i].entry;
        if (e < 0)
            continue;
        auto it = m_thumb_cache.find(e);
        if (it == m_thumb_cache.end())
            continue;
        CachedThumb &ct = it->second;
        std::string key = "img_" + std::to_string(e);
        if (add_to_atlas(e, ct.pixels, ct.w, ct.h)) {
            IMGimage *tmp = imgsGet(m_stash, key.c_str());
            if (tmp) {
                m_slots[e].atlas_x = tmp->atlasX;
                m_slots[e].atlas_y = tmp->atlasY;
                imgsDeleteImage(tmp);
            }
        }
    }

    /* Upload the rebuilt atlas now so no stale texture is ever shown */
    nvgUpdateImage(m_nvg_context, m_atlas_image,
                   imgsGetTextureData(m_stash, nullptr, nullptr));
    m_redraw = true;
}

void ImageStashWidget::flush_pending(NVGcontext *ctx) {
    std::vector<PendingThumb> batch;
    pthread_mutex_lock(&m_mutex);
    batch.swap(m_pending);
    pthread_mutex_unlock(&m_mutex);
    if (batch.empty())
        return;

    for (auto &p : batch) {
        if (!p.pixels) {
            m_slots[p.index].failed = true;
            m_redraw = true;
            continue;
        }
        if (!add_to_atlas(p.index, p.pixels, p.thumb_w, p.thumb_h)) {
            /* Atlas is at its cap and full: evict everything and retry.
               The visible window will be re-materialized on demand. */
            reset_atlas();
            if (!add_to_atlas(p.index, p.pixels, p.thumb_w, p.thumb_h)) {
                std::cerr << "ImageStashWidget: cannot fit thumbnail "
                          << m_entries[p.index].name << std::endl;
                stbi_image_free(p.pixels);
                continue;
            }
        }

        std::string key = "img_" + std::to_string(p.index);
        IMGimage *tmp = imgsGet(m_stash, key.c_str());
        Slot &slot = m_slots[p.index];
        if (tmp) {
            slot.atlas_x = tmp->atlasX;
            slot.atlas_y = tmp->atlasY;
            imgsDeleteImage(tmp);
        }
        slot.requested = false;

        Entry &e = m_entries[p.index];
        e.orig_width  = p.orig_w;
        e.orig_height = p.orig_h;
        e.thumb_width  = p.thumb_w;
        e.thumb_height = p.thumb_h;
        if (p.file_size)
            e.file_size = p.file_size;

        /* Keep the pixels in the CPU cache (takes ownership) so a later
           atlas reset can restore visible thumbnails without disk reads */
        cache_put(p.index, p.pixels, p.thumb_w, p.thumb_h);
        m_redraw = true;
    }
}

void ImageStashWidget::ensure_items_visible(int first, int last) {
    /* Request thumbnails for items in [first, last], first visible first. */
    pthread_mutex_lock(&m_mutex);

    /* Drop requests that scrolled out of the window, so fast scrolling does
       not leave a backlog of off-screen decodes ahead of the visible ones */
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        int item = m_item_of_entry[*it];
        if (item < first || item > last) {
            m_slots[*it].requested = false;
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }

    bool any = false;
    for (int i = last; i >= first; --i) {
        int e = m_items[i].entry;
        if (e < 0)
            continue;
        Slot &s = m_slots[e];
        if (s.atlas_x < 0 && !s.requested && !s.failed) {
            s.requested = true;
            m_queue.push_front(e);
            any = true;
        }
    }
    pthread_mutex_unlock(&m_mutex);
    if (any)
        pthread_cond_signal(&m_cond);
}

/* ------------------------------------------------------------------ */
/*  Fullscreen preview (high resolution, loaded in the background)     */
/* ------------------------------------------------------------------ */

void *ImageStashWidget::load_high_res(void *arg) {
    ImageStashWidget *self = (ImageStashWidget *) arg;
    pthread_mutex_lock(&self->m_highres_mutex);
    for (;;) {
        while (!self->m_highres_pending && !self->m_highres_stop)
            pthread_cond_wait(&self->m_highres_cond, &self->m_highres_mutex);
        if (self->m_highres_stop)
            break;
        Entry entry = self->m_highres_entry;
        self->m_highres_pending = false;
        int req = self->m_highres_req;
        int max_w = self->m_highres_max_w, max_h = self->m_highres_max_h;
        pthread_mutex_unlock(&self->m_highres_mutex);

        pthread_mutex_lock(&self->m_mutex);
        Loader loader = self->m_loader;
        pthread_mutex_unlock(&self->m_mutex);

        int w = 0, h = 0, orient = 1;
        uint64_t fsize = 0;
        bool from_loader = false;
        int edge = std::max(max_w, max_h);
        unsigned char *pixels = nullptr;
        if (loader)
            pixels = loader(entry, edge, &w, &h);
        if (pixels) {
            from_loader = true;
        } else if (!entry.path.empty()) {
            pixels = imgsw__load(entry.path, &w, &h, &orient, &fsize);
        }
        if (pixels) {
            /* Shrink to what the preview can show: a smaller texture upload
               on the UI thread, and no aliasing when minified (no mipmaps).
               The limits apply after rotation, so swap them for 90/270. */
            int box_w = max_w, box_h = max_h;
            if (!from_loader && orient >= 5)
                std::swap(box_w, box_h);
            pixels = imgsw__fit(pixels, &w, &h, box_w, box_h);
            if (!from_loader)
                pixels = imgsw__apply_orient(pixels, &w, &h, orient);
        } else if (!entry.path.empty() || !entry.name.empty()) {
            const std::string &what = entry.path.empty() ? entry.name : entry.path;
            std::cerr << "ImageStashWidget: failed to load high-res " << what << std::endl;
        }

        pthread_mutex_lock(&self->m_highres_mutex);
        if (pixels && req == self->m_highres_req && !self->m_highres_stop) {
            self->m_pending_highres   = pixels;  /* cleared by each new request */
            self->m_pending_highres_w = w;
            self->m_pending_highres_h = h;
            self->set_needs_redraw();
        } else if (pixels) {
            stbi_image_free(pixels);             /* superseded while loading */
        }
    }
    pthread_mutex_unlock(&self->m_highres_mutex);
    return nullptr;
}

void ImageStashWidget::request_high_res(int entry) {
    float ratio = screen() ? screen()->pixel_ratio() : 1.0f;
    float vx, vy, vw, vh;
    view_rect(vx, vy, vw, vh);
    pthread_mutex_lock(&m_highres_mutex);
    m_highres_entry = m_entries[entry];
    m_highres_pending = true;
    m_highres_max_w = (int) (vw * ratio);
    m_highres_max_h = (int) (vh * ratio);
    m_highres_req++;
    if (m_pending_highres) {
        stbi_image_free(m_pending_highres);
        m_pending_highres = nullptr;
    }
    pthread_cond_signal(&m_highres_cond);
    pthread_mutex_unlock(&m_highres_mutex);
}

void ImageStashWidget::release_high_res() {
    pthread_mutex_lock(&m_highres_mutex);
    m_highres_pending = false;
    m_highres_entry = Entry();
    m_highres_req++;                 /* discard a load still in flight */
    if (m_pending_highres) {
        stbi_image_free(m_pending_highres);
        m_pending_highres = nullptr;
    }
    pthread_mutex_unlock(&m_highres_mutex);

    if (m_highres_image != 0) {
        nvgDeleteImage(m_nvg_context, m_highres_image);
        m_highres_image = 0;
    }
    if (m_slide_image != 0) {
        nvgDeleteImage(m_nvg_context, m_slide_image);
        m_slide_image = 0;
    }
    m_slide_from = -1;
}

void ImageStashWidget::open_fullscreen(int entry) {
    m_fullscreen = entry;
    m_anim_start = std::chrono::steady_clock::now();
    m_anim_end = m_anim_start + std::chrono::milliseconds(m_anim_ms);
    m_entering = true;
    request_high_res(entry);
    request_focus();                 /* receive arrow keys and Escape */
    m_redraw = true;
    set_needs_redraw();
}

void ImageStashWidget::close_fullscreen() {
    /* Animate back to the grid; teardown happens in draw() once the exit
       animation completes.  A slide in progress ends immediately. */
    if (m_slide_image != 0) {
        nvgDeleteImage(m_nvg_context, m_slide_image);
        m_slide_image = 0;
    }
    m_slide_from = -1;
    m_anim_start = std::chrono::steady_clock::now();
    m_anim_end = m_anim_start + std::chrono::milliseconds(m_anim_ms);
    m_entering = false;
    m_redraw = true;
    set_needs_redraw();
}

void ImageStashWidget::show_neighbor(int dir) {
    if (m_fullscreen >= (int) m_item_of_entry.size())
        return;
    int item = neighbor_item(m_item_of_entry[m_fullscreen], dir, 0);
    if (item < 0)
        return;                      /* already at the first/last photo */

    /* The outgoing photo slides off with its high-res texture (if loaded) */
    if (m_slide_image != 0)
        nvgDeleteImage(m_nvg_context, m_slide_image);
    m_slide_image = m_highres_image;
    m_slide_w = m_highres_w;
    m_slide_h = m_highres_h;
    m_highres_image = 0;
    m_slide_from = m_fullscreen;
    m_slide_dir = dir;
    m_slide_start = std::chrono::steady_clock::now();

    m_fullscreen = m_items[item].entry;
    request_high_res(m_fullscreen);
    ensure_items_visible(item, item);   /* thumbnail stands in until then */
    set_selected(m_fullscreen);         /* scrolls, so the exit zoom lands on screen */
    m_redraw = true;
    set_needs_redraw();
}

void ImageStashWidget::scroll_to_item(int item) {
    ScrollPanel *panel = dynamic_cast<ScrollPanel *>(parent());
    if (!panel)
        return;
    float top, view_h, vx, vw;
    view_rect(vx, top, vw, view_h);
    float range = m_total_height - view_h;
    float y = m_item_pos[item].y();
    if (range <= 0.0f || (y >= top && y + cell_stride_y() <= top + view_h))
        return;
    float target = y - (view_h - cell_stride_y()) * 0.5f;   /* center the row */
    panel->set_scroll(std::min(std::max(target / range, 0.0f), 1.0f));
}

/* ------------------------------------------------------------------ */
/*  Events                                                             */
/* ------------------------------------------------------------------ */

void ImageStashWidget::set_selected(int entry) {
    if (entry >= (int) m_entries.size())
        entry = -1;
    if (entry >= 0 && entry < (int) m_item_of_entry.size() && m_item_of_entry[entry] >= 0)
        scroll_to_item(m_item_of_entry[entry]);
    if (entry == m_selected)
        return;
    m_selected = entry;
    if (m_selection_callback)
        m_selection_callback(entry);
    set_needs_redraw();
}

int ImageStashWidget::neighbor_item(int item, int dx, int dy) const {
    int n = (int) m_items.size();
    if (dx != 0) {
        do {
            item += dx;
        } while (item >= 0 && item < n && m_items[item].entry < 0);   /* skip sections */
        return item >= 0 && item < n ? item : -1;
    }

    /* First photo of the adjacent row, then the one nearest in x along it
       (rows are contiguous runs of photos sharing a y) */
    float x0 = m_item_pos[item].x(), y0 = m_item_pos[item].y();
    int i = item + dy;
    while (i >= 0 && i < n && (m_items[i].entry < 0 || m_item_pos[i].y() == y0))
        i += dy;
    if (i < 0 || i >= n)
        return -1;
    int best = i;
    float row_y = m_item_pos[i].y();
    for (int j = i; j >= 0 && j < n && m_items[j].entry >= 0 &&
                    m_item_pos[j].y() == row_y; j += dy)
        if (std::fabs(m_item_pos[j].x() - x0) < std::fabs(m_item_pos[best].x() - x0))
            best = j;
    return best;
}

bool ImageStashWidget::mouse_button_event(const Vector2i &p, int button, bool down,
                                          int modifiers) {
    if (button != GLFW_MOUSE_BUTTON_LEFT)
        return Widget::mouse_button_event(p, button, down, modifiers);
    if (!down) {
        m_press_entry = -1;
        return true;
    }
    if (m_fullscreen != -1) {
        close_fullscreen();
        return true;
    }

    /* p is in parent coordinates; convert to content coordinates */
    request_focus();                    /* arrow keys move the selection */
    m_press_pos = p - m_pos;
    int item = item_at(Vector2f((float) m_press_pos.x(), (float) m_press_pos.y()));
    int entry = item >= 0 ? m_items[item].entry : -1;
    m_press_entry = entry;
    set_selected(entry);
    if (entry < 0)
        return true;

    if (m_callback)
        m_callback(entry);

    double now = glfwGetTime();
    if (entry == m_last_click_entry && now - m_last_click_time < 0.4) {
        m_last_click_entry = -1;        /* double-click: open the preview */
        m_press_entry = -1;
        open_fullscreen(entry);
    } else {
        m_last_click_entry = entry;
        m_last_click_time = now;
    }
    return true;
}

bool ImageStashWidget::mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button,
                                        int modifiers) {
    if (m_press_entry < 0 || m_fullscreen != -1 || m_drag_actions == DropNone ||
        !(button & (1 << GLFW_MOUSE_BUTTON_LEFT)))
        return Widget::mouse_drag_event(p, rel, button, modifiers);
    /* A few pixels of travel turn the press into a drag */
    Vector2i d = (p - m_pos) - m_press_pos;
    if (d.x() * d.x() + d.y() * d.y() >= 16) {
        int entry = m_press_entry;
        m_press_entry = -1;
        m_last_click_entry = -1;
        begin_drag(entry, m_press_pos);
    }
    return true;
}

/* Drag `entry` out: its file, plus whatever the drag data callback adds, with
   the cached thumbnail as the preview, held where the press grabbed it. */
void ImageStashWidget::begin_drag(int entry, const Vector2i &content_p) {
    if (!screen())
        return;
    const Entry &e = m_entries[entry];
    DragData data;
    if (!e.path.empty()) {
        data.set("text/uri-list", DragData::uri_list({ e.path }));
        data.set("text/plain", e.path);
    }
    if (m_drag_data_callback)
        m_drag_data_callback(entry, data);

    DragPreview preview;
    preview.label = e.label.empty() ? e.name : e.label;
    auto it = m_thumb_cache.find(entry);
    int item = entry < (int) m_item_of_entry.size() ? m_item_of_entry[entry] : -1;
    if (it != m_thumb_cache.end() && item >= 0) {
        const CachedThumb &ct = it->second;
        preview.w = ct.w;
        preview.h = ct.h;
        preview.rgba.assign(ct.pixels, ct.pixels + (size_t) ct.w * ct.h * 4);
        float sx0, sy0, sw, sh, x, y, w, h;
        thumb_source(entry, 0.0f, sx0, sy0, sw, sh);
        cell_rect(item, sw, sh, x, y, w, h);
        float k = ct.w / w;             /* view -> thumbnail pixels */
        preview.hotspot = Vector2i(
            (int) std::min(std::max((content_p.x() - x) * k, 0.0f), (float) ct.w),
            (int) std::min(std::max((content_p.y() - y) * k, 0.0f), (float) ct.h));
    }
    screen()->start_drag(this, std::move(data), std::move(preview), m_drag_actions);
}

bool ImageStashWidget::drag_motion_event(const Vector2i &, DragEvent &e) {
    bool accept = m_drop_callback && e.source != this && e.data.has("text/uri-list") &&
                  e.accept(this, DropCopy);
    if (accept != m_drop_hover) {
        m_drop_hover = accept;
        set_needs_redraw();
    }
    return accept;
}

void ImageStashWidget::drag_leave_event() {
    m_drop_hover = false;
    set_needs_redraw();
}

bool ImageStashWidget::drag_drop_event(const Vector2i &, DragEvent &e) {
    m_drop_hover = false;
    set_needs_redraw();
    if (!m_drop_callback || e.source == this)
        return false;
    std::vector<std::string> paths = e.data.file_paths();
    if (paths.empty() || !m_drop_callback(paths))
        return false;
    e.accept(this, DropCopy);
    return true;
}

bool ImageStashWidget::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return Widget::keyboard_event(key, scancode, action, modifiers);

    /* Preview open (or opening): Left/Right slide, Escape closes */
    if (m_fullscreen != -1) {
        if (!m_entering)
            return true;
        if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) {
            show_neighbor(key == GLFW_KEY_RIGHT ? 1 : -1);
            return true;
        }
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            close_fullscreen();
            return true;
        }
        return Widget::keyboard_event(key, scancode, action, modifiers);
    }

    /* Grid: arrows move the selection, Enter/Space open the preview */
    int dx = key == GLFW_KEY_LEFT ? -1 : key == GLFW_KEY_RIGHT ? 1 : 0;
    int dy = key == GLFW_KEY_UP   ? -1 : key == GLFW_KEY_DOWN  ? 1 : 0;
    if (dx || dy) {
        int item = m_selected >= 0 && m_selected < (int) m_item_of_entry.size()
                   ? m_item_of_entry[m_selected] : -1;
        if (item < 0) {
            int first, last;            /* nothing selected: start in view */
            visible_items(first, last, 0.0f);
            item = first <= last ? neighbor_item(first - 1, 1, 0) : -1;
        } else {
            item = neighbor_item(item, dx, dy);
        }
        if (item >= 0)
            set_selected(m_items[item].entry);
        return true;
    }
    if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_SPACE) &&
        action == GLFW_PRESS && m_selected >= 0) {
        open_fullscreen(m_selected);
        return true;
    }
    return Widget::keyboard_event(key, scancode, action, modifiers);
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

/* Source rect within an entry's thumbnail.  `crop` blends from the whole
   thumbnail (0) to the centered 3:4 portrait crop (1). */
void ImageStashWidget::thumb_source(int entry, float crop, float &sx0, float &sy0,
                                    float &sw, float &sh) const {
    const Entry &e = m_entries[entry];
    float fw = e.thumb_width  > 0 ? (float) e.thumb_width  : (float) m_thumb_size;
    float fh = e.thumb_height > 0 ? (float) e.thumb_height : (float) m_thumb_size;
    float cw = fw, ch = fh;
    if (fw * 4.0f > fh * 3.0f)
        cw = fh * 0.75f;              /* too wide: crop sides */
    else
        ch = fw * 4.0f / 3.0f;        /* too tall: crop top/bottom */
    sw = fw + crop * (cw - fw);
    sh = fh + crop * (ch - fh);
    sx0 = (fw - sw) * 0.5f;
    sy0 = (fh - sh) * 0.5f;
}

/* Thumbnail rect for a grid item: source aspect fitted and centered inside
   the cell's inner box. */
void ImageStashWidget::cell_rect(int item, float sw, float sh,
                                 float &x, float &y, float &w, float &h) const {
    Vector2f cell = m_item_pos[item];
    float scale = std::min((float) m_thumb_size / sw, (float) m_thumb_size / sh);
    w = sw * scale;
    h = sh * scale;
    x = cell.x() + m_border + ((float) m_thumb_size - w) * 0.5f;
    y = cell.y() + m_border + ((float) m_thumb_size - h) * 0.5f;
}

/* Centered fullscreen rect for an entry (never cropped), sized from the
   high-res image if loaded, else the original or thumbnail dimensions. */
void ImageStashWidget::fullscreen_rect(int entry, int image_w, int image_h,
                                       float &x, float &y, float &w, float &h) const {
    const Entry &e = m_entries[entry];
    w = (float) (image_w > 0 ? image_w : e.orig_width  > 0 ? e.orig_width
               : e.thumb_width  > 0 ? e.thumb_width  : m_thumb_size);
    h = (float) (image_h > 0 ? image_h : e.orig_height > 0 ? e.orig_height
               : e.thumb_height > 0 ? e.thumb_height : m_thumb_size);
    float vx, vy, vw, vh;
    view_rect(vx, vy, vw, vh);
    float scale = std::min(vw / w, vh / h) * 0.9f;
    w *= scale;
    h *= scale;
    x = vx + (vw - w) * 0.5f;
    y = vy + (vh - h) * 0.5f;
}

/* Draw an entry into (x, y, w, h): the high-res texture `image` if nonzero,
   else the atlas thumbnail's source rect, else a placeholder. */
void ImageStashWidget::draw_photo(NVGcontext *ctx, int entry, float x, float y,
                                  float w, float h, float sx0, float sy0,
                                  float sw, float sh, float alpha, int image) {
    const Slot &s = m_slots[entry];
    if (image != 0) {
        NVGpaint paint = nvgImagePattern(ctx, x, y, w, h, 0.0f, image, alpha);
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, w, h);
        nvgFillPaint(ctx, paint);
        nvgFill(ctx);
    } else if (s.atlas_x >= 0) {
        /* Scale the whole atlas so the source rect lands on the target */
        float kx = w / sw, ky = h / sh;
        float ax = x - (s.atlas_x + IMGS_PAD + sx0) * kx;
        float ay = y - (s.atlas_y + IMGS_PAD + sy0) * ky;
        NVGpaint paint = nvgImagePattern(ctx, ax, ay, m_atlas_width * kx,
                                         m_atlas_height * ky, 0.0f,
                                         m_atlas_image, alpha);
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, w, h);
        nvgFillPaint(ctx, paint);
        nvgFill(ctx);
    } else {
        /* Placeholder while the thumbnail is not materialized */
        ImgswColors ink = imgsw_colors(m_theme.get());
        Color tile = s.failed ? ink.failed : ink.placeholder;
        if (s.failed)
            tile.a() *= 160.f / 255.f;
        tile.a() *= alpha;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x, y, w, h, 3.0f);
        nvgFillColor(ctx, tile);
        nvgFill(ctx);
    }
}

/* The fullscreen photo: zooms from its grid cell as t goes 0 -> 1, and
   slides in from the side after an arrow key (the outgoing photo slides off
   the opposite side). */
void ImageStashWidget::draw_fullscreen(NVGcontext *ctx, float t) {
    float vx, vy, view_w, vh;
    view_rect(vx, vy, view_w, vh);

    float slide = 1.0f;
    if (m_slide_from != -1) {
        int slide_ms = std::min(m_anim_ms, kSlideMs);
        float elapsed = std::chrono::duration<float, std::milli>(
                            std::chrono::steady_clock::now() - m_slide_start).count();
        slide = slide_ms > 0 ? std::min(elapsed / slide_ms, 1.0f) : 1.0f;
        if (slide >= 1.0f) {
            if (m_slide_image != 0) {
                nvgDeleteImage(m_nvg_context, m_slide_image);
                m_slide_image = 0;
            }
            m_slide_from = -1;
        } else {
            slide = 1.0f - (1.0f - slide) * (1.0f - slide);  /* ease-out */
            m_redraw = true;

            float sx0, sy0, sw, sh, x, y, w, h;
            thumb_source(m_slide_from, 0.0f, sx0, sy0, sw, sh);
            fullscreen_rect(m_slide_from, m_slide_image ? m_slide_w : 0,
                            m_slide_image ? m_slide_h : 0, x, y, w, h);
            x -= m_slide_dir * view_w * slide;
            draw_photo(ctx, m_slide_from, x, y, w, h, sx0, sy0, sw, sh, 1.0f, m_slide_image);
        }
    }

    /* Uncrop the source rect along with the zoom so the thumbnail is not
       stretched to the uncropped fullscreen aspect */
    float sx0, sy0, sw, sh, x, y, w, h;
    thumb_source(m_fullscreen, m_crop ? 1.0f - t : 0.0f, sx0, sy0, sw, sh);
    fullscreen_rect(m_fullscreen, m_highres_image ? m_highres_w : 0,
                    m_highres_image ? m_highres_h : 0, x, y, w, h);
    int item = m_fullscreen < (int) m_item_of_entry.size()
               ? m_item_of_entry[m_fullscreen] : -1;
    if (t < 1.0f && item >= 0) {
        float cx, cy, cw, ch;
        cell_rect(item, sw, sh, cx, cy, cw, ch);
        x = cx + t * (x - cx);
        y = cy + t * (y - cy);
        w = cw + t * (w - cw);
        h = ch + t * (h - ch);
    }
    if (m_slide_from != -1)
        x += m_slide_dir * view_w * (1.0f - slide);
    draw_photo(ctx, m_fullscreen, x, y, w, h, sx0, sy0, sw, sh, 1.0f,
               t > 0.2f ? m_highres_image : 0);
}

void ImageStashWidget::draw(NVGcontext *ctx) {
    Widget::draw(ctx);
    m_redraw = false;

    /* Opaque screen color: a translucent window fill would show the page through. */
    const ImgswColors ink = imgsw_colors(m_theme.get());
    nvgBeginPath(ctx);
    nvgRect(ctx, (float) m_pos.x(), (float) m_pos.y(),
            (float) m_size.x(), (float) m_size.y());
    nvgFillColor(ctx, ink.sheet);
    nvgFill(ctx);

    /* Move freshly loaded thumbnails into the atlas */
    flush_pending(ctx);

    /* Create the high-res texture once the background load finishes */
    pthread_mutex_lock(&m_highres_mutex);
    if (m_pending_highres) {
        if (m_highres_image != 0)
            nvgDeleteImage(m_nvg_context, m_highres_image);
        m_highres_image = nvgCreateImageRGBA(m_nvg_context, m_pending_highres_w,
                                             m_pending_highres_h, 0, m_pending_highres);
        m_highres_w = m_pending_highres_w;
        m_highres_h = m_pending_highres_h;
        stbi_image_free(m_pending_highres);
        m_pending_highres = nullptr;
    }
    pthread_mutex_unlock(&m_highres_mutex);

    /* Rebuild the layout cache if items or options changed */
    ensure_layout();

    /* Grow the scroll area when the content height changes */
    if ((int) m_total_height != m_laid_out_height) {
        m_laid_out_height = (int) m_total_height;
        ref<ImageStashWidget> self = this;   /* keep alive until it runs */
        async([self]() mutable { if (self->screen()) self->screen()->perform_layout(); });
    }

    /* ScrollWidget adjusts m_pos and clips the visual */
    nvgTranslate(ctx, m_pos.x(), m_pos.y());

    /* Header (grid mode only) */
    float vx, vy, vw, vh;
    view_rect(vx, vy, vw, vh);
    if (m_fullscreen == -1) {
        std::string title = std::to_string(m_entries.size()) + " images";
        nvgFontSize(ctx, 18.0f);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, ink.caption);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(ctx, vx + vw * 0.5f, 12.0f, title.c_str(), nullptr);
    }

    /* Fullscreen animation progress */
    float t = 0.0f;
    if (m_fullscreen != -1) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            m_anim_end - m_anim_start).count();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - m_anim_start).count();
        float raw = duration > 0
            ? std::min(std::max((float) elapsed / duration, 0.0f), 1.0f) : 1.0f;
        raw = raw * raw * (3.0f - 2.0f * raw); /* ease-in-out */
        t = m_entering ? raw : 1.0f - raw;
        if (now < m_anim_end) {
            m_redraw = true; /* keep animating */
        } else if (!m_entering) {
            /* Exit animation finished: release the high-res resources */
            release_high_res();
            m_fullscreen = -1;
            t = 0.0f;
            m_redraw = true;
        }
    }

    /* The preview stays inside the widget (or the scroll viewport), so a
       dialog can host the grid without the photo covering the screen. */
    const bool preview = t > 0.0f || m_fullscreen != -1;
    if (preview) {
        nvgSave(ctx);
        nvgIntersectScissor(ctx, vx, vy, std::max(vw, 0.0f), std::max(vh, 0.0f));
    }

    /* Fade the preview onto the same sheet, so light mode does not go black. */
    if (t > 0.0f) {
        nvgBeginPath(ctx);
        nvgRect(ctx, vx, vy, vw, vh);
        nvgFillColor(ctx, imgsw_alpha(ink.scrim, t));
        nvgFill(ctx);
    }

    float box = (float) (m_thumb_size + 2 * m_border);

    /* Visible range (culling: thousands of images, O(visible) draw) */
    int first, last;
    visible_items(first, last, 0.0f);

    /* Demand-driven loading: fetch thumbnails for the visible rows plus a
       small margin; nothing else is read from disk. */
    if (m_fullscreen == -1 && first <= last) {
        int m_first, m_last;
        visible_items(m_first, m_last, kMarginRows * cell_stride_y());
        ensure_items_visible(m_first, m_last);
    }

    float content_w = (float) width();
    float grid_x0 = grid_offset_x();

    /* Grid, fading out behind the fullscreen preview */
    float grid_alpha = 1.0f - t;
    for (int i = first; grid_alpha > 0.0f && i <= last; ++i) {
        const Item &item = m_items[i];
        Vector2f cell = m_item_pos[i];

        /* ---- Section header (H1 text, alignment, optional rule) ---- */
        if (item.section >= 0) {
            const Section &sec = m_sections[item.section];
            float tx;
            int align;
            switch ((SectionAlign) sec.align) {
                case SectionAlign::Center:
                    tx = content_w * 0.5f;
                    align = NVG_ALIGN_CENTER;
                    break;
                case SectionAlign::Right:
                    tx = content_w - grid_x0;
                    align = NVG_ALIGN_RIGHT;
                    break;
                default:
                    tx = grid_x0;
                    align = NVG_ALIGN_LEFT;
                    break;
            }
            nvgFontSize(ctx, 22.0f);
            nvgFontFace(ctx, "sans-bold");
            nvgFillColor(ctx, imgsw_alpha(ink.heading, grid_alpha));
            nvgTextAlign(ctx, align | NVG_ALIGN_TOP);
            nvgText(ctx, tx, cell.y() + 8.0f, sec.text.c_str(), nullptr);
            if (sec.rule) {
                nvgBeginPath(ctx);
                nvgRect(ctx, grid_x0, cell.y() + section_height() - 5.0f,
                        content_w - 2.0f * grid_x0, 1.0f);
                nvgFillColor(ctx, imgsw_alpha(ink.rule, grid_alpha));
                nvgFill(ctx);
            }
            continue;
        }

        /* ---- Photo thumbnail (the fullscreen one is drawn on top below) ---- */
        int ei = item.entry;
        if (ei == m_selected) {
            /* Selection: tinted cell, brighter while the widget has focus */
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, cell.x(), cell.y(), box, box, 4.0f);
            nvgFillColor(ctx, imgsw_alpha(ink.accent, ((focused() ? 90.f : 45.f) / 255.f) * grid_alpha));
            nvgFill(ctx);
            nvgStrokeColor(ctx, imgsw_alpha(ink.accent, ((focused() ? 230.f : 120.f) / 255.f) * grid_alpha));
            nvgStrokeWidth(ctx, 2.0f);
            nvgStroke(ctx);
        }
        if (ei != m_fullscreen) {
            float sx0, sy0, sw, sh, x, y, w, h;
            thumb_source(ei, m_crop ? 1.0f : 0.0f, sx0, sy0, sw, sh);
            cell_rect(i, sw, sh, x, y, w, h);
            draw_photo(ctx, ei, x, y, w, h, sx0, sy0, sw, sh, grid_alpha, 0);
        }

        /* Label, clipped to the cell width so it never overlaps neighbors */
        if (m_show_labels) {
            const std::string &caption = m_entries[ei].label.empty()
                ? m_entries[ei].name : m_entries[ei].label;
            nvgSave(ctx);
            nvgIntersectScissor(ctx, cell.x(), cell.y() + box,
                                cell_stride_x() - m_gap_x, (float) m_label_h);
            nvgFontSize(ctx, 13.0f);
            nvgFontFace(ctx, "sans");
            nvgFillColor(ctx, imgsw_alpha(ink.label, grid_alpha));
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            nvgText(ctx, cell.x() + (cell_stride_x() - m_gap_x) * 0.5f,
                    cell.y() + box, caption.c_str(), nullptr);
            nvgRestore(ctx);
        }
        if (m_badge && grid_alpha > 0.0f)
            m_badge(ctx, m_entries[ei], cell.x(), cell.y(), box);
    }

    if (m_fullscreen != -1)
        draw_fullscreen(ctx, t);
    if (preview)
        nvgRestore(ctx);

    /* Drop highlight around the visible area */
    if (m_drop_hover) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, vx + 2.0f, vy + 2.0f, vw - 4.0f, vh - 4.0f, 4.0f);
        nvgStrokeColor(ctx, imgsw_alpha(ink.accent, 220.f / 255.f));
        nvgStrokeWidth(ctx, 3.0f);
        nvgStroke(ctx);
    }

    /* Upload atlas changes to the GPU (only the changed rows/columns; the
       draw calls above are not rendered until the end of the frame) */
    int dirty[4];
    if (imgsValidateTexture(m_stash, dirty))
        nvgUpdateImageRegion(m_nvg_context, m_atlas_image, dirty[0], dirty[1],
                             dirty[2] - dirty[0], dirty[3] - dirty[1],
                             imgsGetTextureData(m_stash, nullptr, nullptr));

    if (m_redraw)
        set_needs_redraw();
}

void ImageStashWidget::set_needs_redraw() {
    if (screen())
        screen()->redraw();
}

NAMESPACE_END(nanogui)
