/*
    nanogui/imagestash.h -- Scrollable image asset library widget backed by
    the NanoVG imagestash texture atlas.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/widget.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <nanogui/pthread_win.h>
#else
#include <pthread.h>
#endif

struct IMGcontext;
struct IMGimage;

NAMESPACE_BEGIN(nanogui)

/// Text alignment for section headers (\ref ImageStashWidget::add_section).
enum class SectionAlign {
    Left = 0,
    Center,
    Right
};

/**
 * \class ImageStashWidget imagestash.h nanogui/imagestash.h
 *
 * \brief Scrollable grid of image thumbnails backed by the NanoVG
 *        imagestash texture atlas.
 *
 * Images are pushed into the widget with \ref add_image / \ref add_images
 * and loaded asynchronously on a background thread.  Only downscaled
 * thumbnails are kept on the GPU (in a shared texture atlas); the widget
 * keeps just metadata (name, path, file size, dimensions) per entry, so
 * thousands of images can be catalogued with modest memory use.
 *
 * Sections (H1 text headers, e.g. year / event / album) can be inserted
 * arbitrarily between photos with \ref add_section / \ref insert_section.
 *
 * View options (all default to the plain photo grid):
 *  - \ref set_animation_time -- fullscreen zoom duration, 0..2 s (0 = instant)
 *  - \ref set_show_labels    -- show/hide the per-photo text label
 *  - \ref set_spacing        -- x/y gap between photos
 *  - \ref set_border         -- padding inside each cell, around the photo
 *  - \ref set_cell_size      -- on-screen cell size; thumbnails stay as decoded
 *  - \ref set_crop           -- center-crop thumbnails to portrait (3:4)
 *
 * Scalability notes:
 *  - Only visible rows (plus a small margin) are drawn and loaded.
 *  - The atlas grows on demand up to a cap.  Once full it is compacted: the
 *    thumbnails nearest the view (about half the atlas) are repacked from a
 *    CPU-side LRU pixel cache, and the rest come back from that cache, not
 *    the disk, when scrolled to.  The cache is sized in bytes
 *    (\ref set_cache_budget) and is several times larger than the atlas.
 *  - While bulk-loading, the view is refreshed at most twice per second.
 *
 * Clicking a thumbnail selects it (and focuses the widget, so the arrow keys
 * move the selection).  Double-click, Enter or Space opens an animated
 * preview that fills the widget's visible area and loads the original
 * resolution image in the background; a click or Escape dismisses it, and
 * Left/Right slide to the previous/next photo.  The previewed photo drags
 * out like a thumbnail.
 *
 * Dragging a thumbnail starts a drag and drop (nanogui/dragdrop.h) offering
 * the file as "text/uri-list" and "text/plain", with the thumbnail and its
 * label as the preview; \ref set_drag_data_callback adds formats.
 */
class NANOGUI_EXPORT ImageStashWidget : public Widget {
public:
    /// Metadata + file info for one catalogued image.
    struct Entry {
        std::string name;                 ///< Base file name (display label)
        std::string path;                 ///< Full path to the image file (may be empty)
        std::string label;                ///< Shown instead of name when set
        /// Identity in the thumbnail cache, which outlives clear(): the same
        /// key brings back the same thumbnail without a read.  Empty uses
        /// `path`; with neither, the thumbnail is not kept across a clear().
        std::string key;
        uint32_t    user_id = 0;          ///< Caller's id, 0 if none
        uint64_t    file_size = 0;        ///< File size in bytes
        int         orig_width  = 0;      ///< Displayed image width  (0 until loaded)
        int         orig_height = 0;      ///< Displayed image height (0 until loaded)
        int         thumb_width  = 0;     ///< Thumbnail width   (0 until loaded)
        int         thumb_height = 0;     ///< Thumbnail height  (0 until loaded)

        bool loaded() const { return thumb_width > 0; }
    };

    /// malloc'd upright RGBA (freed with free), or null to load `path` instead.
    using Loader = std::function<unsigned char *(const Entry &entry, int max_edge,
                                                 int *w, int *h)>;
    /// Draws a mark on a cell (use count, missing file). `size` is the cell box.
    using BadgeCallback = std::function<void(NVGcontext *ctx, const Entry &entry,
                                             float x, float y, float size)>;

    ImageStashWidget(Widget *parent, int thumb_size = 128);
    virtual ~ImageStashWidget();

    /// Catalogue one image file (thread-safe).  Only records metadata;
    /// thumbnails load on demand for the visible region.
    void add_image(const std::string &path);
    /// Catalogue several image files (thread-safe).
    void add_images(const std::vector<std::string> &paths);
    /// Catalogue one entry, including one with no file (thread-safe).
    void add_entry(const Entry &entry);
    /// Remove all images and sections and reset the atlas.
    void clear();

    /// Thumbnail and preview pixels. Null falls back to loading `path`.
    void set_loader(const Loader &loader);
    /// Called while a cell is drawn. Empty draws nothing.
    void set_badge_callback(const BadgeCallback &cb);

    /// Append a section header after the current items (thread-safe).
    void add_section(const std::string &text,
                     SectionAlign align = SectionAlign::Left, bool rule = false);
    /// Insert a section header before the given item index (thread-safe).
    void insert_section(int item_index, const std::string &text,
                        SectionAlign align = SectionAlign::Left, bool rule = false);

    /* View options */
    void set_animation_time(float seconds);           ///< 0..2 s; 0 = instant
    float animation_time() const { return m_anim_ms / 1000.0f; }
    void set_show_labels(bool show);                  ///< per-photo label text
    bool show_labels() const { return m_show_labels; }
    void set_spacing(int x, int y);                   ///< gap between photos
    void set_border(int px);                          ///< padding inside each cell
    int border() const { return m_border; }
    /// On-screen cell size. Does not decode again: the constructor size is
    /// the thumbnail that stays in the atlas.
    void set_cell_size(int size);
    int cell_size() const { return m_cell_size; }
    void set_crop(bool crop);                         ///< center-crop to portrait 3:4
    bool crop() const { return m_crop; }

    size_t image_count() const { return m_entries.size(); }
    const Entry &image_at(size_t i) const { return m_entries[i]; }

    int thumbnail_size() const { return m_thumb_size; }

    /// Memory for decoded thumbnails kept off the GPU (default 256 MB, about
    /// 4000 thumbnails at 128 px).  Past it the least recently shown go, and
    /// are read from disk again when next shown.  Keep it well above the
    /// atlas (2048 x 4096 x 4 = 32 MB), or an atlas compaction sends the
    /// rows just scrolled past back to the disk.
    void set_cache_budget(size_t bytes);
    size_t cache_budget() const { return m_cache_budget; }
    size_t cache_bytes() const { return m_cache_bytes; }

    /// A few lines on the atlas (size, rows, where its pixels go), the CPU
    /// cache (thumbnails, bytes against the budget) and how thumbnails got
    /// there (decoded, decoded again, restored from the cache, compactions).
    std::string stats() const;
    /// Checks the cache, the per-photo atlas slots and the atlas itself are
    /// consistent with each other. Returns the number of problems, and
    /// lists them in `problems` when given. UI thread only.
    int validate(std::string *problems = nullptr) const;
    /// Print stats() and validate() to stderr each time the atlas grows,
    /// is compacted or is cleared. On when IMGSW_STATS is set in the
    /// environment.
    void set_debug_stats(bool on) { m_debug_stats = on; }
    bool debug_stats() const { return m_debug_stats; }

    /// Invoked when a thumbnail is clicked (index into the photo entries).
    std::function<void(int)> callback() const { return m_callback; }
    void set_callback(const std::function<void(int)> &callback) { m_callback = callback; }

    /// The selected photo entry, -1 for none.
    int selected() const { return m_selected; }
    /// Select a photo entry (-1 clears) and scroll it into view.
    void set_selected(int entry);
    /// Invoked when the selection changes (click, keys, preview navigation).
    void set_selection_callback(const std::function<void(int)> &cb) { m_selection_callback = cb; }

    /// Actions offered when a thumbnail is dragged out (DropNone disables).
    void set_drag_actions(int actions) { m_drag_actions = actions; }
    /// Adds formats to the data of a drag starting on `entry`.
    void set_drag_data_callback(const std::function<void(int, DragData &)> &cb) {
        m_drag_data_callback = cb;
    }

    /// Accept files dragged in (from the desktop or another widget, not from
    /// this one): the callback gets the local paths and returns true if taken.
    void set_drop_callback(const std::function<bool(const std::vector<std::string> &)> &cb) {
        m_drop_callback = cb;
    }

    /// Right-click anywhere on the grid. The point is in screen coordinates.
    void set_context_callback(const std::function<void(const Vector2i &)> &cb) {
        m_context_callback = cb;
    }

    virtual bool drag_motion_event(const Vector2i &p, DragEvent &e) override;
    virtual void drag_leave_event() override;
    virtual void drag_finished(DropAction action) override;
    virtual bool drag_drop_event(const Vector2i &p, DragEvent &e) override;
    virtual bool mouse_button_event(const Vector2i &p, int button, bool down,
                                    int modifiers) override;
    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button,
                                  int modifiers) override;
    virtual bool keyboard_event(int key, int scancode, int action,
                                int modifiers) override;
    virtual Vector2i preferred_size(NVGcontext *ctx) const override;
    virtual void draw(NVGcontext *ctx) override;

protected:
    /* Layout: photos on a fixed-cell centered grid, sections full width */
    float content_width() const;              ///< width the grid lays out in
    int   columns() const;
    float grid_offset_x() const;              ///< left offset that centers the grid
    float cell_stride_x() const;              ///< thumb + border + horizontal gap
    float cell_stride_y() const;              ///< thumb + border + label? + vertical gap
    float section_height() const { return 40.0f; }
    float header_height() const { return 40.0f; }

    void ensure_layout() const;               ///< rebuild the layout cache if dirty
    void rebuild_layout();                    ///< UI thread only
    /// The visible part of the widget in content coordinates (the scroll
    /// panel's window onto it, else the whole widget).
    void view_rect(float &x, float &y, float &w, float &h) const;
    void visible_items(int &first, int &last, float margin_px) const;
    int  item_at(const Vector2f &content_p) const;    ///< hit test, -1 if none
    /// Photo item beside `item`: dx steps in display order, dy picks the
    /// nearest photo in the next/previous row.  -1 if none.
    int  neighbor_item(int item, int dx, int dy) const;
    void begin_drag(int entry, const Vector2i &content_p);

    /* Thumbnail pipeline */
    void ensure_items_visible(int first, int last);
    void flush_pending(NVGcontext *ctx);
    bool add_to_atlas(int index, unsigned char *pixels, int w, int h);
    bool sync_atlas_size();
    void reset_atlas();

    /* Fullscreen preview */
    void open_fullscreen(int entry);
    void close_fullscreen();                  ///< start the exit animation
    void show_neighbor(int dir);              ///< -1 = previous, +1 = next photo
    void scroll_to_item(int item);            ///< bring a grid cell into view
    void request_high_res(int entry);
    void release_high_res();                  ///< drop textures and in-flight loads

    /* Drawing helpers */
    void thumb_source(int entry, float crop, float &sx0, float &sy0,
                      float &sw, float &sh) const;
    void cell_rect(int item, float sw, float sh,
                   float &x, float &y, float &w, float &h) const;
    void fullscreen_rect(int entry, int image_w, int image_h,
                         float &x, float &y, float &w, float &h) const;
    void draw_photo(NVGcontext *ctx, int entry, float x, float y, float w, float h,
                    float sx0, float sy0, float sw, float sh, float alpha, int image);
    void draw_fullscreen(NVGcontext *ctx, float t);

    void set_needs_redraw();

    static void *load_thumbnails(void *arg);
    static void *load_high_res(void *arg);

protected:
    /// One row of content: either a photo (entry >= 0) or a section header.
    struct Item {
        int entry   = -1;
        int section = -1;
    };

    struct Section {
        std::string text;
        int  align;     ///< SectionAlign
        bool rule;      ///< draw a thin horizontal rule under the text
    };

    /// Cross-thread input, drained into m_items/m_sections by rebuild_layout.
    struct PendingItem {
        Entry photo;            ///< photo to catalogue
        bool section = false;   ///< a header, not a photo (path may be empty either way)
        std::string text;       ///< section text
        int  align = 0;
        bool rule = false;
        int  insert_pos = -1;   ///< -1 = append
    };

    /// Internal per-entry atlas state (parallel to m_entries).
    struct Slot {
        int  atlas_x   = -1;    ///< x in atlas, -1 when not materialized
        int  atlas_y   = -1;    ///< y in atlas, -1 when not materialized
        bool requested = false; ///< queued or in flight
        bool failed    = false; ///< file could not be decoded
    };

    struct PendingThumb {
        int index;
        unsigned char *pixels;  ///< downscaled RGBA thumbnail, null = failed
        int orig_w, orig_h;
        int thumb_w, thumb_h;
        uint64_t file_size;     ///< discovered by the loader thread
    };

    /// Rows above/below the viewport whose thumbnails are also loaded.
    static constexpr int kMarginRows = 2;

    int m_thumb_size;                 ///< decode edge; fixed for the widget's life
    int m_cell_size;                  ///< on-screen cell; may be smaller than the thumbnail
    int m_gap_x    = 10;
    int m_gap_y    = 10;
    int m_border   = 4;
    int m_label_h  = 20;
    bool m_show_labels = true;
    bool m_crop        = false;
    int  m_anim_ms     = 400;

    /* Photo entries.  Appended only by the UI thread (rebuild_layout) under
       m_mutex, so the UI reads them lock-free; the loader reads under m_mutex. */
    std::vector<Entry> m_entries;
    std::vector<Slot>  m_slots;

    /* Content rows + layout cache (UI thread only) */
    std::vector<Item>     m_items;
    std::vector<Section>  m_sections;
    std::vector<Vector2f> m_item_pos;
    std::vector<int>      m_item_of_entry;
    float m_total_height = 0.0f;
    std::atomic<bool> m_layout_dirty{true};   ///< also set by add_* from any thread
    int   m_layout_width = -1;
    int   m_laid_out_height = -1;

    /* Loader thread state (guarded by m_mutex) */
    std::deque<int>           m_queue;
    std::vector<PendingThumb> m_pending;
    std::vector<PendingItem>  m_pending_items;
    pthread_t       m_loader_thread;
    pthread_mutex_t m_mutex;
    pthread_cond_t  m_cond;
    bool m_stop    = false;
    int  m_generation = 0;    ///< bumped by clear() to drop in-flight loads

    /* CPU thumbnail cache (UI thread only), keyed by Entry::key (or path)
       rather than position, and kept across clear(): a rebuilt grid takes
       its thumbnails back from memory.  Front of LRU = most recent; each
       node keeps its place in the list, so a touch is O(1). */
    struct CachedThumb {
        unsigned char *pixels = nullptr;  ///< owned, stbi_image_free'd
        int w = 0, h = 0;
        int orig_w = 0, orig_h = 0;
        uint64_t file_size = 0;
        size_t bytes() const { return (size_t) w * (size_t) h * 4; }
    };
    struct CacheNode {
        CachedThumb thumb;
        std::list<std::string>::iterator lru;
    };
    std::unordered_map<std::string, CacheNode> m_thumb_cache;
    std::list<std::string> m_cache_lru;
    size_t m_cache_budget = size_t(256) << 20;
    size_t m_cache_bytes  = 0;
    void cache_trim();

    /* How thumbnails reached the atlas (UI thread). A key decoded twice was
       evicted from the cache in between: the reloads to keep at zero. */
    std::unordered_set<std::string> m_decoded_keys;
    size_t m_decoded   = 0;   ///< thumbnails from the loader
    size_t m_redecoded = 0;   ///< ... whose key had been decoded before
    size_t m_restored  = 0;   ///< put back into the atlas from the cache
    size_t m_compactions = 0;
    size_t m_last_kept = 0;   ///< thumbnails the last compaction repacked
    bool m_debug_stats = false;
    void debug_report(const char *event) const;
    static std::string cache_key(const Entry &e) { return e.key.empty() ? e.path : e.key; }
    const CachedThumb *cached(int entry) const;
    /// Takes ownership of `thumb.pixels`; frees them if the entry has no key.
    void cache_put(int entry, const CachedThumb &thumb);
    /// Put a cached thumbnail into the atlas for `entry`.  False if it doesn't fit.
    bool materialize(int entry, const CachedThumb &thumb);

    /* Atlas / GPU resources */
    NVGcontext *m_nvg_context = nullptr;
    IMGcontext *m_stash       = nullptr;
    int m_atlas_image  = 0;
    int m_atlas_width  = 0;
    int m_atlas_height = 0;

    /* Fullscreen preview state (m_fullscreen is a photo entry index) */
    int m_fullscreen = -1;
    std::chrono::steady_clock::time_point m_anim_start, m_anim_end;
    bool  m_entering = false;
    int   m_highres_image = 0;  ///< NVG texture of m_fullscreen (UI thread)
    int   m_highres_w = 0, m_highres_h = 0;

    /* Slide between neighbors (arrow keys); the outgoing photo keeps its
       high-res texture until it has slid off screen. */
    static constexpr int kSlideMs = 200;      ///< capped by the animation time
    int m_slide_from  = -1;                   ///< outgoing entry, -1 = no slide
    int m_slide_dir   = 0;                    ///< +1 = to next, -1 = to previous
    int m_slide_image = 0;
    int m_slide_w = 0, m_slide_h = 0;
    std::chrono::steady_clock::time_point m_slide_start;

    /* High-res loader thread (guarded by m_highres_mutex).  It always loads
       the latest request; results of superseded requests are discarded. */
    pthread_t       m_highres_thread;
    pthread_mutex_t m_highres_mutex;
    pthread_cond_t  m_highres_cond;
    bool            m_highres_pending = false;
    Entry           m_highres_entry;
    int  m_highres_max_w = 0, m_highres_max_h = 0;   ///< downscale target (px)
    int             m_highres_req = 0;        ///< bumped by each request/release
    unsigned char  *m_pending_highres = nullptr;
    int  m_pending_highres_w = 0, m_pending_highres_h = 0;
    bool m_highres_stop = false;

    /* Selection and pointer state (UI thread) */
    int      m_selected = -1;
    int      m_press_entry = -1;      ///< photo under the press, for dragging
    bool     m_press_closes = false;  ///< a press on the open preview: a click closes it
    Vector2i m_press_pos;             ///< press point, content coordinates
    int      m_last_click_entry = -1; ///< for double-click detection
    double   m_last_click_time = 0.0;
    int      m_drag_actions = DropCopy | DropLink;
    bool     m_drop_hover = false;    ///< accepted drag over the widget

    bool m_redraw = true;
    std::function<void(int)> m_callback;
    std::function<void(int)> m_selection_callback;
    std::function<void(int, DragData &)> m_drag_data_callback;
    std::function<bool(const std::vector<std::string> &)> m_drop_callback;
    std::function<void(const Vector2i &)> m_context_callback;
    Loader m_loader;
    BadgeCallback m_badge;
};

NAMESPACE_END(nanogui)
