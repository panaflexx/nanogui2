/*
 * pagemade/image_assets.cpp — see image_assets.h.
 *
 * Folder scans run on their own thread and hand paths back in batches.
 * The thread never holds the panel: it writes a ScanState, and the UI
 * thread applies the batch only while that state still points here.
 * File probes for the metadata pane work the same way, so closing the
 * window does not wait on a read and does not touch a freed panel.
 */
#include "image_assets.h"

#include "composerview.h"
#include "dialogs.h"
#include "image.h"
#include "page.h"
#include "uri.h"

#include <GLFW/glfw3.h>
#include <nanogui/button.h>
#include <nanogui/opengl.h>
#include <nanogui/imagestash.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/screen.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/textarea.h>
#include <nanovg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <utility>

#if !defined(_WIN32)
#include <stdio.h>
#endif

using namespace nanogui;
using namespace pagemade;

namespace {

constexpr int kScanBatch = 100;

struct ScanBox {
    std::mutex mu;
    std::atomic<bool> stop{false};
    ImageAssetsPanel *panel = nullptr;
    std::vector<std::string> roots;
    std::vector<std::string> batch;
};

struct ProbeBox {
    std::mutex mu;
    ImageAssetsPanel *panel = nullptr;
};

bool is_image_file(const std::filesystem::path &p) {
    std::string ext = p.extension().string();
    for (char &c : ext)
        c = (char) std::tolower((unsigned char) c);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}

std::string leaf_name(const std::string &path) {
    const std::string name = std::filesystem::path(path).filename().string();
    return name.empty() ? path : name;
}

std::string format_bytes(uint64_t n) {
    char buf[64];
    if (n >= 1024ull * 1024ull * 1024ull)
        std::snprintf(buf, sizeof buf, "%.1f GB", n / (1024.0 * 1024.0 * 1024.0));
    else if (n >= 1024ull * 1024ull)
        std::snprintf(buf, sizeof buf, "%.1f MB", n / (1024.0 * 1024.0));
    else if (n >= 1024ull)
        std::snprintf(buf, sizeof buf, "%.1f KB", n / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%llu bytes", (unsigned long long) n);
    return buf;
}

std::string format_time(int64_t sec) {
    if (sec <= 0)
        return {};
    std::time_t t = (std::time_t) sec;
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0)
        return {};
#else
    if (!localtime_r(&t, &tm))
        return {};
#endif
    char buf[64];
    if (std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm) == 0)
        return {};
    return buf;
}

void add_line(std::string &out, const std::string &key, const std::string &value) {
    if (!out.empty())
        out += '\n';
    out += key;
    out += ": ";
    out += value.empty() ? std::string("—") : value;
}

std::string dpi_text(const ImageMetadata &meta) {
    if (meta.ppi_x <= 0 && meta.ppi_y <= 0)
        return "not in file";
    char buf[64];
    if (std::fabs(meta.ppi_x - meta.ppi_y) < 0.5)
        std::snprintf(buf, sizeof buf, "%.0f", meta.ppi_x > 0 ? meta.ppi_x : meta.ppi_y);
    else
        std::snprintf(buf, sizeof buf, "%.0f \u00D7 %.0f", meta.ppi_x, meta.ppi_y);
    return buf;
}

std::string size_text(uint64_t bytes, int w, int h) {
    std::string s = bytes ? format_bytes(bytes) : std::string();
    if (w > 0 && h > 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%d \u00D7 %d px", w, h);
        if (!s.empty())
            s += "   ";
        s += buf;
    }
    return s;
}

std::string date_text(int64_t mtime, const ImageMetadata &meta) {
    std::string s = format_time(mtime);
    for (const auto &tag : meta.exif) {
        if (tag.first != "Taken" && tag.first != "Date")
            continue;
        if (!s.empty())
            s += "    ";
        s += tag.first;
        s += " ";
        s += tag.second;
    }
    return s;
}

std::string describe_picture(const std::string &filename, const std::string &path,
                             const ImageMetadata &meta, uint64_t bytes, int64_t mtime,
                             const ImageAsset *asset, int uses, const ImageStore *images) {
    std::string out;
    add_line(out, "Filename", filename);
    if (!path.empty())
        add_line(out, "Path", path);
    add_line(out, "Date", date_text(mtime, meta));
    add_line(out, "Size", size_text(bytes, meta.width_px, meta.height_px));
    add_line(out, "DPI", dpi_text(meta));
    add_line(out, "Color", meta.channels ? color_model_name(meta.model) : "");
    add_line(out, "Format", meta.format);
    if (meta.bit_depth > 0)
        add_line(out, "Depth", std::to_string(meta.bit_depth) + "-bit");
    if (meta.channels > 0)
        add_line(out, "Channels", std::to_string(meta.channels));
    if (meta.width_px > 0)
        add_line(out, "Alpha", meta.alpha ? "yes" : "no");
    if (meta.orientation > 0)
        add_line(out, "Orientation", std::to_string(meta.orientation));
    if (asset && images) {
        add_line(out, "Status", asset_status_name(asset_status(*asset, images)));
        add_line(out, "Placed", uses > 0 ? std::to_string(uses) : std::string("unused"));
    }
    for (const auto &tag : meta.exif) {
        if (tag.first == "Taken" || tag.first == "Date")
            continue;
        add_line(out, tag.first, tag.second);
    }
    return out;
}

std::string shell_quote(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::string ask_folder(const std::string &initial) {
#if defined(_WIN32)
    (void) initial;
    return {};
#else
    std::string cmd = "zenity --file-selection --directory --title='Add Folder'";
    if (!initial.empty()) {
        std::string filename = initial;
        if (filename.back() != '/')
            filename += '/';
        cmd += " --filename=";
        cmd += shell_quote(filename);
    }
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return {};
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe))
        out += buf;
    const int rc = pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (rc != 0)
        return {};
    return out;
#endif
}

unsigned char *load_asset_pixels(ImageStore *images, const ImageStashWidget::Entry &entry,
                                 int max_edge, int *w, int *h) {
    if (!images || entry.user_id == 0)
        return nullptr;
    ImageStore::Bytes bytes = images->source_of(entry.user_id);
    if (!bytes || bytes->empty())
        return nullptr;
    std::vector<uint8_t> rgba;
    int dw = 0, dh = 0;
    if (!make_display(bytes->data(), bytes->size(), max_edge, &rgba, &dw, &dh, nullptr) ||
        rgba.empty() || dw <= 0 || dh <= 0)
        return nullptr;
    unsigned char *out = (unsigned char *) std::malloc(rgba.size());
    if (!out)
        return nullptr;
    std::memcpy(out, rgba.data(), rgba.size());
    *w = dw;
    *h = dh;
    return out;
}

ImageStashWidget::Entry library_entry(const std::string &path) {
    ImageStashWidget::Entry e;
    e.path = path;
    e.name = leaf_name(path);
    e.label = e.name;
    return e;
}

std::string asset_path(const ImageAsset &asset, const ImageStore *images) {
    std::string path;
    if (!asset.source.empty() && file_path_from_uri(asset.source, path))
        return path;
    return images ? images->path_of(asset.id) : std::string();
}

} // namespace

struct ImageAssetsPanel::ScanState : ScanBox {};
struct ImageAssetsPanel::ProbeGate : ProbeBox {};

class ImageAssetsPanel::RootList : public Widget {
public:
    explicit RootList(Widget *parent) : Widget(parent) { set_live(true); }

    void set_paths(std::vector<std::string> paths) {
        int next = -1;
        const std::string previous = (m_sel >= 0 && m_sel < (int) m_captions.size())
            ? m_captions[(size_t) m_sel].path : std::string();
        m_captions.clear();
        for (const std::string &path : paths) {
            Row row;
            row.path = path;
            std::error_code ec;
            const bool dir = std::filesystem::is_directory(path, ec);
            row.caption = dir && (path.empty() || path.back() != '/') ? path + "/" : path;
            if (!previous.empty() && path == previous)
                next = (int) m_captions.size();
            m_captions.push_back(std::move(row));
        }
        m_sel = next;
        if (screen())
            screen()->redraw();
    }

    int selected() const { return m_sel; }

    Vector2i preferred_size(NVGcontext *) const override {
        const int rows = std::max(1, (int) m_captions.size());
        return Vector2i(120, rows * kRow + 4);
    }

    bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (button != GLFW_MOUSE_BUTTON_1 || !down || m_captions.empty())
            return Widget::mouse_button_event(p, button, down, modifiers);
        const int row = (p.y() - m_pos.y()) / kRow;
        if (row < 0 || row >= (int) m_captions.size())
            return true;
        m_sel = row;
        if (screen())
            screen()->redraw();
        return true;
    }

    void draw(NVGcontext *ctx) override {
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 14.0f);
        if (m_captions.empty()) {
            nvgFillColor(ctx, m_theme ? m_theme->m_text_color : nanogui::Color(80, 80, 80, 255));
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(ctx, m_pos.x() + 8.0f, m_pos.y() + kRow * 0.5f, "No folders or files yet", nullptr);
            return;
        }
        for (int i = 0; i < (int) m_captions.size(); ++i) {
            const float y = (float) (m_pos.y() + i * kRow);
            if (i == m_sel) {
                nvgBeginPath(ctx);
                nvgRoundedRect(ctx, m_pos.x() + 2.0f, y + 1.0f, (float) m_size.x() - 4.0f,
                               (float) kRow - 2.0f, 3.0f);
                nvgFillColor(ctx, nvgRGBA(70, 130, 255, 70));
                nvgFill(ctx);
            }
            nvgSave(ctx);
            nvgIntersectScissor(ctx, m_pos.x() + 4.0f, y, (float) m_size.x() - 8.0f, (float) kRow);
            nvgFillColor(ctx, m_theme ? m_theme->m_text_color : nanogui::Color(30, 30, 30, 255));
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(ctx, m_pos.x() + 8.0f, y + kRow * 0.5f, m_captions[(size_t) i].caption.c_str(),
                    nullptr);
            nvgRestore(ctx);
        }
    }

private:
    struct Row {
        std::string path;
        std::string caption;
    };
    std::vector<Row> m_captions;
    int m_sel = -1;
    static constexpr int kRow = 22;
};

ImageAssetsPanel::ImageAssetsPanel(Widget *parent, ComposerView *view, ImageStore *images)
    : Window(parent, "Image Assets", true), m_view(view), m_images(images) {
    m_probe_gate = std::make_shared<ProbeGate>();
    m_probe_gate->panel = this;

    set_modal(false);
    set_traffic_lights_mask(0x1);

    auto *flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                AlignItems::Stretch, 12, 8);
    set_layout(flex);

    new Label(this, "Folders and files", "sans", 16);

    auto *list_scroll = new ScrollPanel(this, ScrollPanel::ScrollTypes::Vertical);
    list_scroll->set_min_height(110);
    flex->set_flex_item(list_scroll, FlexLayout::FlexItem(0.f, 0.f, 110));
    m_roots_list = new RootList(list_scroll);

    /* Two rows: one row of these captions is wider than the dialog. */
    Widget *row_add = new Widget(this);
    row_add->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 6));
    auto *add_images = new Button(row_add, "Add Images...");
    auto *add_folder = new Button(row_add, "Add Folder...");
    Widget *row_edit = new Widget(this);
    row_edit->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 6));
    auto *remove = new Button(row_edit, "Remove");
    auto *add_pub = new Button(row_edit, "Add to Publication");
    add_images->set_tooltip("Add picture files to the library list");
    add_folder->set_tooltip("Add a folder to the library list");
    remove->set_tooltip("Remove the selected folder or file from the list");
    add_pub->set_tooltip("Keep the selected library picture in this publication, without placing it");
    add_images->set_callback([this] {
        std::vector<std::string> paths;
        try {
            paths = file_dialog({{"png", "PNG"}, {"jpg", "JPEG"}, {"jpeg", "JPEG"}},
                                false, true, initial_folder());
        } catch (const std::exception &ex) {
            if (screen())
                show_alert(screen(), "The files couldn't be chosen.", ex.what());
            return;
        }
        bool any = false;
        for (const std::string &path : paths)
            any = add_root(path) || any;
        if (any)
            restart_scan();
    });
    add_folder->set_callback([this] {
        const std::string dir = ask_folder(initial_folder());
#if defined(_WIN32)
        if (dir.empty() && screen())
            show_alert(screen(), "Add Folder isn't available here.",
                       "Drop a folder onto the picture grid.");
#endif
        if (!dir.empty() && add_root(dir))
            restart_scan();
    });
    remove->set_callback([this] {
        const int sel = m_roots_list ? m_roots_list->selected() : -1;
        if (sel < 0 || sel >= (int) m_roots.size())
            return;
        m_roots.erase(m_roots.begin() + sel);
        if (m_roots_list)
            m_roots_list->set_paths(m_roots);
        restart_scan();
    });
    add_pub->set_callback([this] {
        if (!m_stash)
            return;
        (void) m_stash->preferred_size(nullptr);
        const int sel = m_stash->selected();
        if (sel >= 0 && sel < (int) m_stash->image_count()) {
            const ImageStashWidget::Entry &e = m_stash->image_at((size_t) sel);
            if (e.user_id == 0 && !e.path.empty()) {
                import_library_file(e.path);
                return;
            }
        }
        const int root = m_roots_list ? m_roots_list->selected() : -1;
        if (root >= 0 && root < (int) m_roots.size() && is_image_file(m_roots[(size_t) root]))
            import_library_file(m_roots[(size_t) root]);
    });
    auto *stash_scroll = new ScrollPanel(this, ScrollPanel::ScrollTypes::Vertical);
    stash_scroll->set_min_height(160);
    flex->set_flex_item(stash_scroll, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_stash = new ImageStashWidget(stash_scroll, 112);
    m_stash->set_live(true);
    m_stash->set_loader([images](const ImageStashWidget::Entry &entry, int max_edge,
                                 int *w, int *h) {
        return load_asset_pixels(images, entry, max_edge, w, h);
    });
    m_stash->set_badge_callback([this](NVGcontext *ctx, const ImageStashWidget::Entry &entry,
                                       float x, float y, float size) {
        if (entry.user_id == 0 || !m_view || !m_images)
            return;
        const ImageAsset *asset = m_view->document().find_image(entry.user_id);
        if (!asset)
            return;
        const int uses = m_uses.count(entry.user_id) ? m_uses.at(entry.user_id) : 0;
        const AssetStatus status = asset_status(*asset, m_images);
        std::string text = std::to_string(uses);
        NVGcolor bg = nvgRGBA(30, 30, 36, 210);
        if (status == AssetStatus::Missing) {
            text = "!";
            bg = nvgRGBA(190, 40, 40, 235);
        } else if (status == AssetStatus::Modified) {
            bg = nvgRGBA(200, 140, 30, 235);
        } else if (uses == 0) {
            bg = nvgRGBA(90, 90, 100, 220);
        }
        const float tw = text.size() > 2 ? 28.0f : text.size() > 1 ? 22.0f : 18.0f;
        const float th = 18.0f;
        const float bx = x + size - tw - 6.0f;
        const float by = y + 6.0f;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, bx, by, tw, th, 8.0f);
        nvgFillColor(ctx, bg);
        nvgFill(ctx);
        nvgFontSize(ctx, 12.0f);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, nvgRGBA(255, 255, 255, 240));
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(ctx, bx + tw * 0.5f, by + th * 0.5f, text.c_str(), nullptr);
    });
    m_stash->set_selection_callback([this](int index) {
        if (!m_rebuilding)
            show_metadata(index);
    });
    m_stash->set_drag_data_callback([this](int index, DragData &data) {
        if (!m_stash || index < 0 || index >= (int) m_stash->image_count())
            return;
        const ImageStashWidget::Entry &e = m_stash->image_at((size_t) index);
        if (e.user_id != 0)
            data.set(kImageAssetDragType, std::to_string(e.user_id));
    });
    m_stash->set_drop_callback([this](const std::vector<std::string> &paths) {
        bool any = false;
        for (const std::string &path : paths) {
            std::error_code ec;
            if (std::filesystem::is_directory(path, ec) || is_image_file(path))
                any = add_root(path) || any;
        }
        if (any)
            restart_scan();
        return any;
    });

    auto *meta_scroll = new ScrollPanel(this, ScrollPanel::ScrollTypes::Vertical);
    meta_scroll->set_min_height(150);
    flex->set_flex_item(meta_scroll, FlexLayout::FlexItem(0.f, 0.f, 150));
    m_meta = new TextArea(meta_scroll);
    m_meta->set_live(true);
    m_meta->set_font("sans");
    m_meta->set_padding(8);
    m_meta->set_foreground_color(nanogui::Color(30, 30, 30, 255));
    m_meta->set_background_color(nanogui::Color(248, 248, 248, 255));

    set_min_width(150);
    set_size(Vector2i(350, 780));
    rebuild();
    if (Screen *s = screen())
        if (s->nvg_context())
            perform_layout(s->nvg_context());
}

ImageAssetsPanel::~ImageAssetsPanel() {
    if (m_probe_gate) {
        std::lock_guard<std::mutex> lock(m_probe_gate->mu);
        m_probe_gate->panel = nullptr;
    }
    if (m_stash) {
        m_stash->set_loader({});
        m_stash->set_badge_callback({});
        m_stash->set_selection_callback({});
        m_stash->set_drop_callback({});
        m_stash->set_drag_data_callback({});
    }
    stop_scan();
}

void ImageAssetsPanel::close_soon() {
    auto gate = m_probe_gate;
    async([this, gate] {
        {
            std::lock_guard<std::mutex> lock(gate->mu);
            if (gate->panel != this)
                return;
        }
        if (parent())
            dispose();
    });
}

void ImageAssetsPanel::refresh() {
    if (!m_view)
        return;
    const std::string sig = asset_signature();
    if (sig != m_sig)
        rebuild();
    else
        recount();
    if (screen())
        screen()->redraw();
}

void ImageAssetsPanel::recount() {
    m_uses.clear();
    if (!m_view)
        return;
    for (const Page &page : m_view->document().pages)
        for (const Item &it : page.items)
            if (const PlacedImage *im = it.image())
                m_uses[im->asset] += 1;
}

std::string ImageAssetsPanel::asset_signature() const {
    std::string s;
    if (!m_view)
        return s;
    for (const ImageAsset &asset : m_view->document().images) {
        s += std::to_string(asset.id);
        s += '\0';
        s += asset.name;
        s += '\0';
        s += asset.source;
        s += '\0';
    }
    return s;
}

std::string ImageAssetsPanel::initial_folder() const {
    if (m_roots.empty())
        return {};
    std::error_code ec;
    const std::filesystem::path p(m_roots.back());
    if (std::filesystem::is_directory(p, ec))
        return p.string();
    return p.parent_path().string();
}

bool ImageAssetsPanel::add_root(const std::string &path) {
    if (path.empty())
        return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return false;
    if (!std::filesystem::is_directory(path, ec) && !is_image_file(path))
        return false;
    for (const std::string &have : m_roots)
        if (have == path)
            return false;
    m_roots.push_back(path);
    if (m_roots_list)
        m_roots_list->set_paths(m_roots);
    return true;
}

void ImageAssetsPanel::relayout() {
    if (Screen *s = screen())
        if (s->nvg_context())
            perform_layout(s->nvg_context());
}

void ImageAssetsPanel::rebuild() {
    if (!m_stash || !m_view)
        return;
    uint32_t keep_id = 0;
    std::string keep_path;
    (void) m_stash->preferred_size(nullptr);
    const int sel = m_stash->selected();
    if (sel >= 0 && sel < (int) m_stash->image_count()) {
        const ImageStashWidget::Entry &e = m_stash->image_at((size_t) sel);
        keep_id = e.user_id;
        if (!keep_id)
            keep_path = e.path;
    }

    m_rebuilding = true;
    m_stash->clear();
    m_stash->add_section("In this publication");
    for (const ImageAsset &asset : m_view->document().images) {
        ImageStashWidget::Entry e;
        e.user_id = asset.id;
        e.name = asset.name.empty() ? std::string("image") : asset.name;
        e.label = e.name;
        e.file_size = asset.source_size;
        e.orig_width = asset.meta.width_px;
        e.orig_height = asset.meta.height_px;
        e.path = asset_path(asset, m_images);
        m_stash->add_entry(e);
    }
    m_stash->add_section("Library");
    for (const std::string &path : m_library_files)
        m_stash->add_entry(library_entry(path));
    (void) m_stash->preferred_size(nullptr);

    int restore = -1;
    for (size_t i = 0; i < m_stash->image_count(); ++i) {
        const ImageStashWidget::Entry &e = m_stash->image_at(i);
        if (keep_id && e.user_id == keep_id) {
            restore = (int) i;
            break;
        }
        if (!keep_id && !keep_path.empty() && e.user_id == 0 && e.path == keep_path) {
            restore = (int) i;
            break;
        }
    }
    m_sig = asset_signature();
    recount();
    m_stash->set_selected(restore);
    m_rebuilding = false;
    show_metadata(restore);
}

void ImageAssetsPanel::show_metadata(int index) {
    const uint64_t gen = ++m_probe_gen;
    if (!m_meta)
        return;
    if (!m_stash || index < 0 || index >= (int) m_stash->image_count()) {
        m_meta->set_text("");
        relayout();
        return;
    }
    const ImageStashWidget::Entry entry = m_stash->image_at((size_t) index);
    const ImageAsset *asset = nullptr;
    int uses = 0;
    if (entry.user_id && m_view) {
        asset = m_view->document().find_image(entry.user_id);
        if (m_uses.count(entry.user_id))
            uses = m_uses.at(entry.user_id);
    }
    ImageMetadata meta;
    uint64_t bytes = entry.file_size;
    int64_t mtime = 0;
    std::string filename = entry.label.empty() ? entry.name : entry.label;
    if (asset) {
        meta = asset->meta;
        bytes = asset->source_size ? asset->source_size : bytes;
        mtime = asset->source_mtime;
        if (!asset->name.empty())
            filename = asset->name;
    }
    m_meta->set_text(describe_picture(filename, entry.path, meta, bytes, mtime,
                                      asset, uses, m_images));
    relayout();
    if (!entry.path.empty()) {
        const uint32_t asset_id = entry.user_id;
        const std::string path = entry.path;
        auto gate = m_probe_gate;
        std::thread([gate, path, gen, asset_id] {
            ImageImport imported = read_image_import(path);
            imported.bytes.clear();
            imported.bytes.shrink_to_fit();
            auto held = std::make_shared<ImageImport>(std::move(imported));
            async([gate, gen, asset_id, held] {
                ImageAssetsPanel *panel = nullptr;
                {
                    std::lock_guard<std::mutex> lock(gate->mu);
                    panel = gate->panel;
                }
                if (panel)
                    panel->apply_probe(gen, asset_id, *held);
            });
        }).detach();
    }
}

void ImageAssetsPanel::apply_probe(uint64_t gen, uint32_t asset_id, const ImageImport &imported) {
    if (gen != m_probe_gen || !m_meta || !m_stash)
        return;
    const int sel = m_stash->selected();
    if (sel < 0 || sel >= (int) m_stash->image_count())
        return;
    const ImageStashWidget::Entry &entry = m_stash->image_at((size_t) sel);
    if (entry.user_id != asset_id)
        return;
    if (!imported.ok) {
        /* text() drops the line breaks, so add the failure under what is
         * already shown. */
        m_meta->append_line(std::string("File: ") +
                            (imported.error.empty() ? "couldn't read the picture" : imported.error));
        relayout();
        return;
    }
    const ImageAsset *asset = nullptr;
    int uses = 0;
    if (asset_id && m_view) {
        asset = m_view->document().find_image(asset_id);
        if (m_uses.count(asset_id))
            uses = m_uses.at(asset_id);
    }
    const std::string filename = entry.label.empty() ? entry.name : entry.label;
    m_meta->set_text(describe_picture(filename, entry.path, imported.meta,
                                      imported.stamp.size, imported.stamp.mtime,
                                      asset, uses, m_images));
    relayout();
}

void ImageAssetsPanel::import_library_file(const std::string &path) {
    if (!m_view)
        return;
    auto gate = m_probe_gate;
    m_view->import_asset(path, [gate](const std::string &error) {
        ImageAssetsPanel *panel = nullptr;
        {
            std::lock_guard<std::mutex> lock(gate->mu);
            panel = gate->panel;
        }
        if (!panel || !panel->screen() || error.empty())
            return;
        show_alert(panel->screen(), "The picture couldn't be added.", error);
    });
}

void ImageAssetsPanel::stop_scan() {
    if (m_scan_state) {
        std::lock_guard<std::mutex> lock(m_scan_state->mu);
        m_scan_state->stop = true;
        m_scan_state->panel = nullptr;
    }
    if (m_scan.joinable())
        m_scan.join();
    m_scan_state.reset();
}

void ImageAssetsPanel::restart_scan() {
    stop_scan();
    m_library_files.clear();
    m_library_set.clear();
    rebuild();
    relayout();
    if (m_roots.empty())
        return;
    m_scan_state = std::make_shared<ScanState>();
    {
        std::lock_guard<std::mutex> lock(m_scan_state->mu);
        m_scan_state->panel = this;
        m_scan_state->roots = m_roots;
    }
    m_scan = std::thread([state = m_scan_state] { scan_main(state); });
}

void ImageAssetsPanel::scan_main(const std::shared_ptr<ScanState> &state) {
    namespace fs = std::filesystem;
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        roots = state->roots;
    }
    std::vector<std::string> batch;
    auto flush = [&] {
        if (batch.empty())
            return;
        std::sort(batch.begin(), batch.end());
        {
            std::lock_guard<std::mutex> lock(state->mu);
            if (state->stop)
                return;
            state->batch.insert(state->batch.end(), batch.begin(), batch.end());
        }
        batch.clear();
        async([state] {
            ImageAssetsPanel *panel = nullptr;
            {
                std::lock_guard<std::mutex> lock(state->mu);
                panel = state->panel;
            }
            if (panel)
                panel->take_scanned(state);
        });
    };

    for (const std::string &root : roots) {
        if (state->stop)
            break;
        std::error_code ec;
        const fs::path p(root);
        if (fs::is_regular_file(p, ec)) {
            if (is_image_file(p))
                batch.push_back(p.string());
        } else if (fs::is_directory(p, ec)) {
            fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
            while (!ec && it != end && !state->stop) {
                const fs::directory_entry entry = *it;
                it.increment(ec);
                if (ec)
                    break;
                if (entry.is_regular_file(ec) && !ec && is_image_file(entry.path()))
                    batch.push_back(entry.path().string());
                if ((int) batch.size() >= kScanBatch)
                    flush();
            }
        }
        if ((int) batch.size() >= kScanBatch)
            flush();
    }
    flush();
}

void ImageAssetsPanel::take_scanned(const std::shared_ptr<ScanState> &state) {
    std::vector<std::string> got;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (state->panel != this)
            return;
        got.swap(state->batch);
    }
    add_library_files(std::move(got));
}

void ImageAssetsPanel::add_library_files(std::vector<std::string> paths) {
    if (!m_stash)
        return;
    bool any = false;
    for (std::string &path : paths) {
        if (!m_library_set.insert(path).second)
            continue;
        m_library_files.push_back(path);
        m_stash->add_entry(library_entry(path));
        any = true;
    }
    if (any && screen())
        screen()->redraw();
}
