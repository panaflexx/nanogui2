/*
 * pagemade/image_assets.h — the Images tab of the Image Assets window.
 *
 * A tool panel (tool_tabs.h): folders and files the user is browsing, then an
 * image grid in two sections (pictures kept by this publication, and the
 * files under those folders), then metadata for the selected picture.
 * Drag a publication picture onto a page to place it. Drag a library file
 * and the composer imports it, then places it.
 *
 * Library folders live here, not in the publication, so New and Open leave
 * them alone. They are saved to assets.conf whenever they change and read
 * back when the panel is first made. The app hides the window when it is closed rather than
 * destroying it, so the folders, files and thumbnails are kept for the
 * session. Thumbnails of pictures the publication already holds are
 * decoded from the ImageStore on the widget's loader thread.
 */
#pragma once

#include "tool_tabs.h"

#include "bg_worker.h"
#include "image.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ComposerView;
namespace nanogui {
class ImageStashWidget;
class Button;
class Split;
class TextArea;
}

class ImageAssetsPanel : public ToolPanel {
public:
    ImageAssetsPanel(nanogui::Widget *parent, ComposerView *view, pagemade::ImageStore *images);
    ~ImageAssetsPanel() override;

    std::string title() const override { return "Images"; }
    /* The publication section follows the open document. Called on every
     * document change; the grid is rebuilt only when the picture list
     * itself changed. Badges (use count, missing, modified) redraw either way. */
    void refresh() override;
    /* Both floors explicit: the layout's own floor counts each scroll panel
     * as half the screen, which would pin the window near full height. */
    nanogui::Vector2i floor_size() const override { return nanogui::Vector2i(200, 440); }

private:
    struct ScanState;
    struct ProbeGate;
    class RootList;

    void rebuild();
    void recount();
    /* Link status (missing, modified) of every picture, stat'ing the files
     * on the worker: a network share answers slowly. At most every few
     * seconds unless `force`. */
    void check_status(bool force);
    void apply_status(uint64_t gen, std::unordered_map<uint32_t, pagemade::AssetStatus> status);
    void show_metadata(int index);
    void apply_probe(uint64_t gen, uint32_t asset_id, const pagemade::ImageImport &imported);
    void relayout();
    /* Fold the details pane away, or bring it back where it was. */
    void toggle_details();
    bool add_root(const std::string &path);
    /* The folder list changed: save it, then scan again. */
    void roots_changed();
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
    nanogui::Split *m_split = nullptr;    // grid above, details below
    nanogui::Button *m_clean = nullptr;
    float m_details_at = 0.72f;           // divider to restore the details to

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

    /* File work off the UI thread, the newest request winning: probes of
     * the selection, and the (slower) status sweep on its own thread */
    pagemade::BackgroundWorker m_worker, m_status_worker;
    std::unordered_map<uint32_t, pagemade::AssetStatus> m_status;
    uint64_t m_status_gen = 0;
    double m_status_time = -1e9;
    bool m_status_busy = false, m_status_again = false;
};
