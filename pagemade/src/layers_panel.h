/*
 * pagemade/layers_panel.h — the Layers window.
 *
 * One row per layer, top layer first: an eye to show or hide it, a lock,
 * and its name (the active layer, where new items go, is pressed). Below,
 * New, Delete, Up, Down, Rename, and Move Selection Here. A publication
 * has at most kMaxLayers layers. The window docks beside the pages like
 * Image Assets, and is only hidden when closed.
 */
#pragma once

#include "tool_tabs.h"

#include "page.h"

#include <string>

class ComposerView;
namespace nanogui { class Button; class Widget; }

class LayersPanel : public ToolPanel {
public:
    LayersPanel(nanogui::Widget *parent, ComposerView *view);

    std::string title() const override { return "Layers"; }
    nanogui::Vector2i floor_size() const override { return nanogui::Vector2i(150, 120); }
    /* Follow the document: rebuilt when the layers changed. */
    void refresh() override;
    int preferred_content_height(NVGcontext *ctx, int width) override;

private:
    void rebuild();
    std::string signature() const;
    /* The layer row `k` shows (top first), or 0 for an unused row. */
    pagemade::LayerId layer_at_row(size_t k) const;

    ComposerView *m_view = nullptr;
    nanogui::Widget *m_rows = nullptr;
    /* One row per possible layer, made once and only shown, hidden and
     * relabeled: a button that is clicked is never taken down under the
     * pointer (or out of the screen's focus path). */
    struct Row {
        nanogui::Widget *row = nullptr;
        nanogui::Button *eye = nullptr, *lock = nullptr, *name = nullptr;
    };
    Row m_row[8];
    nanogui::Button *m_new = nullptr, *m_delete = nullptr, *m_up = nullptr,
                    *m_down = nullptr, *m_rename = nullptr, *m_move_here = nullptr;
    std::string m_sig;
};
