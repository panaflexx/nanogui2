/*
 * pagemade/image_assets.h — the Image Assets window.
 *
 * A non-modal tool window: folders and files the user is browsing, then an
 * image grid in two sections (pictures kept by this publication, and the
 * files under those folders), then metadata for the selected picture.
 * Drag a publication picture onto a page to place it. Drag a library file
 * and the composer imports it, then places it.
 *
 * Library folders live here, not in the publication, so New and Open leave
 * them alone. Thumbnails of pictures the publication already holds are
 * decoded from the ImageStore on the widget's loader thread.
 */
#pragma once

#include <nanogui/window.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pagemade {
class ImageStore;
struct ImageImport;
}
class ComposerView;
namespace nanogui {
class ImageStashWidget;
class TextArea;
}

class ImageAssetsPanel : public nanogui::Window {
public:
    ImageAssetsPanel(nanogui::Widget *parent, ComposerView *view, pagemade::ImageStore *images);
    ~ImageAssetsPanel() override;

    /* The publication section follows the open document. Called on every
     * document change; the grid is rebuilt only when the picture list
     * itself changed. Badges (use count, missing, modified) redraw either way. */
    void refresh();

    /* Close from the title bar after this event returns, so the click that
     * asked for it is not still walking the window. */
    void close_soon();

private:
    struct ScanState;
    struct ProbeGate;
    class RootList;

    void rebuild();
    void recount();
    void show_metadata(int index);
    void apply_probe(uint64_t gen, uint32_t asset_id, const pagemade::ImageImport &imported);
    void relayout();
    bool add_root(const std::string &path);
    void restart_scan();
    void stop_scan();
    static void scan_main(const std::shared_ptr<ScanState> &state);
    void take_scanned(const std::shared_ptr<ScanState> &state);
    void add_library_files(std::vector<std::string> paths);
    void import_library_file(const std::string &path);
    std::string asset_signature() const;
    std::string initial_folder() const;

    ComposerView *m_view = nullptr;
    pagemade::ImageStore *m_images = nullptr;

    RootList *m_roots_list = nullptr;
    nanogui::ImageStashWidget *m_stash = nullptr;
    nanogui::TextArea *m_meta = nullptr;

    std::vector<std::string> m_roots;
    std::vector<std::string> m_library_files;
    std::unordered_set<std::string> m_library_set;
    std::unordered_map<uint32_t, int> m_uses;
    std::string m_sig;
    bool m_rebuilding = false;

    std::shared_ptr<ScanState> m_scan_state;
    std::thread m_scan;
    std::shared_ptr<ProbeGate> m_probe_gate;
    uint64_t m_probe_gen = 0;
};
