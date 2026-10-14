/*
    examples/imagestash_test.cpp -- Test program for the ImageStashWidget
    asset library.

    Usage:
        imagestash_test <image-or-directory> [more...]

    Directories are scanned recursively on a background thread in chunks of
    ~100 files, so the UI stays responsive even on slow (SMB/network)
    shares.  The widget itself only reads image data for the visible region
    plus a small margin -- scrolling requests more as needed.
*/

#include <nanogui/nanogui.h>
#include <nanogui/screen.h>
#include <nanogui/window.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/imagestash.h>
#include <nanogui/fscompat.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace nanogui;

/// True for file extensions supported by stb_image.
static bool is_image_file(const std::string &path) {
    std::string ext = fs::extension(path);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    static const char *kExts[] = {
        ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif",
        ".psd", ".hdr", ".pic", ".pgm", ".ppm", ".pnm"
    };
    for (const char *e : kExts)
        if (ext == e)
            return true;
    return false;
}

/**
 * Background directory scanner.  Recursively walks the given roots on its
 * own thread and pushes image paths into the widget in chunks, keeping the
 * UI thread completely free of filesystem stalls.
 */
class ScanThread {
public:
    static constexpr size_t kChunkSize = 100;

    ScanThread(ImageStashWidget *widget, std::vector<std::string> roots)
        : m_widget(widget), m_roots(std::move(roots)) {}

    void start() { m_thread = std::thread([this] { run(); }); }

    void stop() {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void run() {
        namespace fs = nanogui::fs;
        std::vector<std::string> batch;

        for (const auto &root : m_roots) {
            if (m_stop) break;
            if (fs::is_regular_file(root)) {
                if (is_image_file(root))
                    batch.push_back(root);
            } else if (fs::is_directory(root)) {
                /* Unreadable directories are skipped silently. */
                fs::walk_files(root, [&](const std::string &path) {
                    if (is_image_file(path))
                        batch.push_back(path);
                    if (batch.size() >= kChunkSize)
                        flush(batch);
                    return !m_stop.load();
                });
            } else {
                std::cerr << "Skipping (not a file or directory): " << root << std::endl;
            }
        }
        flush(batch);
        std::cout << "Scan complete: " << m_found << " images." << std::endl;
    }

    void flush(std::vector<std::string> &batch) {
        if (batch.empty())
            return;
        std::sort(batch.begin(), batch.end());
        m_widget->add_images(batch); /* thread-safe */
        m_found += (int) batch.size();
        std::cout << "\rScanning... " << m_found << " images found" << std::flush;
        batch.clear();
    }

    ImageStashWidget *m_widget;
    std::vector<std::string> m_roots;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    int m_found = 0;
};

/**
 * A drop target for testing drags out of the grid: shows the name of the
 * last file dropped on it, and highlights while a drag it accepts is over it.
 */
class DropLabel : public Label {
public:
    DropLabel(Widget *parent) : Label(parent, "Drop here") {
        set_fixed_size(Vector2i(160, 28));
    }

    bool drag_motion_event(const Vector2i &, DragEvent &e) override {
        bool accept = e.data.has("text/uri-list") && e.accept(this, DropCopy);
        set_hover(accept);
        return accept;
    }
    void drag_leave_event() override { set_hover(false); }
    bool drag_drop_event(const Vector2i &, DragEvent &e) override {
        set_hover(false);
        std::vector<std::string> paths = e.data.file_paths();
        if (paths.empty())
            return false;
        set_caption(nanogui::fs::filename(paths[0]));
        e.accept(this, DropCopy);
        return true;
    }

    void draw(NVGcontext *ctx) override {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + 0.5f, m_pos.y() + 0.5f, m_size.x() - 1.0f,
                       m_size.y() - 1.0f, 4.0f);
        nvgStrokeColor(ctx, m_hover ? nvgRGBA(70, 130, 255, 255) : nvgRGBA(255, 255, 255, 60));
        nvgStrokeWidth(ctx, m_hover ? 2.0f : 1.0f);
        nvgStroke(ctx);
        Label::draw(ctx);
    }

private:
    void set_hover(bool hover) {
        if (hover != m_hover && screen())
            screen()->redraw();
        m_hover = hover;
    }
    bool m_hover = false;
};

int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <image-or-directory> [more...]" << std::endl;
            return 1;
        }

        nanogui::init();

        /* Scope so the scanner, screen and widget are destroyed before shutdown */
        {
            ref<Screen> screen = new Screen(Vector2i(1200, 900), "NanoGUI ImageStash Test");

            const int kToolbarHeight = 40;

            Window *window = new Window(screen, "", true);
            window->set_position(Vector2i(0, 0));
            window->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill));
            window->set_size(screen->size());

            /* View-option toolbar above the grid */
            Widget *toolbar = new Widget(window);
            toolbar->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 6, 8));
            toolbar->set_fixed_size(Vector2i(screen->width(), kToolbarHeight));

            /* The window only auto-sizes a lone ScrollPanel child, so size it here */
            ScrollPanel *scroll = new ScrollPanel(window);
            scroll->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);
            scroll->set_fixed_size(screen->size() - Vector2i(0, kToolbarHeight));

            ref<ImageStashWidget> widget = new ImageStashWidget(scroll);
            widget->set_callback([](int index) {
                printf("clicked image %d\n", index);
            });
            ImageStashWidget *stash = widget;

            /* Fullscreen zoom animation time, 0..2 s (0 = instant) */
            new Label(toolbar, "Zoom");
            Slider *anim = new Slider(toolbar);
            anim->set_range({ 0.0f, 2.0f });
            anim->set_value(stash->animation_time());
            anim->set_fixed_size(Vector2i(80, 0));
            Label *anim_label = new Label(toolbar, "");
            auto set_anim = [stash, anim_label](float seconds) {
                stash->set_animation_time(seconds);
                char buf[16];
                snprintf(buf, sizeof(buf), "%.1f s", stash->animation_time());
                anim_label->set_caption(buf);
            };
            set_anim(stash->animation_time());
            anim->set_callback(set_anim);

            /* Per-photo labels */
            CheckBox *labels = new CheckBox(toolbar, "Labels",
                [stash](bool on) { stash->set_show_labels(on); });
            labels->set_checked(stash->show_labels());

            /* Center-crop to portrait */
            CheckBox *crop = new CheckBox(toolbar, "Crop 3:4",
                [stash](bool on) { stash->set_crop(on); });
            crop->set_checked(stash->crop());

            /* x/y spacing between photos (initial values match the widget defaults) */
            new Label(toolbar, "Gap x/y");
            IntBox<int> *gap_x = new IntBox<int>(toolbar, 10);
            IntBox<int> *gap_y = new IntBox<int>(toolbar, 10);
            for (IntBox<int> *gap : { gap_x, gap_y }) {
                gap->set_editable(true);
                gap->set_spinnable(true);
                gap->set_min_max_values(0, 100);
                gap->set_fixed_size(Vector2i(60, 0));
                gap->set_callback([stash, gap_x, gap_y](int) {
                    stash->set_spacing(gap_x->value(), gap_y->value());
                });
            }

            /* Section header: text, alignment, rule, insert position (-1 = append) */
            TextBox *sec_text = new TextBox(toolbar, "Section");
            sec_text->set_editable(true);
            sec_text->set_fixed_size(Vector2i(120, 0));
            ComboBox *sec_align = new ComboBox(toolbar, { "Left", "Center", "Right" });
            sec_align->set_fixed_size(Vector2i(90, 0));
            CheckBox *sec_rule = new CheckBox(toolbar, "Rule");
            new Label(toolbar, "at");
            IntBox<int> *sec_pos = new IntBox<int>(toolbar, 0);
            sec_pos->set_editable(true);
            sec_pos->set_spinnable(true);
            sec_pos->set_min_max_values(-1, std::numeric_limits<int>::max());
            sec_pos->set_fixed_size(Vector2i(70, 0));
            Button *sec_add = new Button(toolbar, "Insert section");
            sec_add->set_callback([stash, sec_text, sec_align, sec_rule, sec_pos] {
                stash->insert_section(sec_pos->value(), sec_text->value(),
                                      (SectionAlign) sec_align->selected_index(),
                                      sec_rule->checked());
            });

            /* Drag and drop: thumbnails drag out to this label; image files
               dragged in from the desktop join the grid */
            new DropLabel(toolbar);
            stash->set_drop_callback([stash](const std::vector<std::string> &paths) {
                std::vector<std::string> images;
                for (const auto &path : paths)
                    if (is_image_file(path))
                        images.push_back(path);
                stash->add_images(images);
                return !images.empty();
            });
            stash->set_selection_callback([](int index) {
                printf("selected image %d\n", index);
            });

            std::vector<std::string> inputs(argv + 1, argv + argc);
            ScanThread scanner(widget, std::move(inputs));
            scanner.start();

            screen->set_visible(true);
            screen->perform_layout();

            nanogui::mainloop();

            /* Stop scanning before the widget is destroyed */
            scanner.stop();
        }

        nanogui::shutdown();
    } catch (const std::runtime_error &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
