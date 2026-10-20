/*
 * pagemade/layouts_panel.h — the Layouts tab of the Image Assets window.
 *
 * Previews of the layout templates (layout_templates.h) in a scrolling
 * grid, picked by category and turned by the Rotate menu. Drag a preview
 * onto a page, or double-click it (or Place) for the page on screen: the
 * layout is fitted inside the page margins and its items are grouped. Save
 * Page keeps the selection (or the whole page) as a new layout; Remove
 * takes the chosen one out. Both rewrite layouts.json at once. Reload
 * reads the file again after it was edited by hand.
 *
 * The templates are read the first time the tab is shown, not when the
 * window is made.
 */
#pragma once

#include "layout_templates.h"
#include "tool_tabs.h"

#include <string>
#include <vector>

class ComposerView;
namespace nanogui {
class Button;
class Dropdown;
class Label;
class ScrollPanel;
}

class LayoutsPanel : public ToolPanel {
public:
    LayoutsPanel(nanogui::Widget *parent, ComposerView *view);

    std::string title() const override { return "Layouts"; }
    void refresh() override;
    void shown() override;
    nanogui::Vector2i floor_size() const override { return nanogui::Vector2i(200, 440); }

private:
    class Grid;

    void load();
    /* The category menu from the templates, keeping the choice. */
    void rebuild_categories();
    /* The grid shows the chosen category, turned. */
    void apply_filter();
    void selection_changed();
    void relayout();
    /* The chosen template turned as shown; null when none is chosen. */
    const pagemade::LayoutTemplate *chosen() const;
    void place_chosen();
    void save_page();
    void remove_chosen();
    bool save_file();

    ComposerView *m_view = nullptr;
    nanogui::Dropdown *m_category = nullptr, *m_rotation = nullptr;
    nanogui::ScrollPanel *m_scroll = nullptr;
    Grid *m_grid = nullptr;
    nanogui::Label *m_info = nullptr;
    nanogui::Button *m_place = nullptr, *m_remove = nullptr;

    std::string m_path;
    bool m_loaded = false;
    std::vector<pagemade::LayoutTemplate> m_all;   // the file's order
    std::vector<std::string> m_categories;         // "All", then each in file order
    std::vector<size_t> m_shown;                   // indices into m_all
    std::vector<pagemade::LayoutTemplate> m_turned; // m_shown, turned
    int m_turns = 0;
};
