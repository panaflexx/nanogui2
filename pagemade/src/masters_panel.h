/*
 * pagemade/masters_panel.h — the Masters tab of the Image Assets window.
 *
 * The publication's master pages (PageDoc::masters) as previews, [None]
 * first. Drag one onto a page, or onto a page icon, to apply it (Shift: the
 * page's spread); Apply... takes a page range ("1-5, 8, even, all").
 * Double-click a master (or Edit) to put its pages on the pasteboard and
 * edit them with every tool; Done (or Edit again) puts the publication
 * back, and every page that uses the master follows. New... makes a blank
 * master, From Page turns the page on screen (its spread, with facing
 * pages) into one. Make Default is the master new pages get. Options...
 * sets the name, prefix, based-on master and its own margins and columns.
 * Numbering... is Layout > Numbering & Section Options for the page on
 * screen.
 *
 * Every change is one undo step (ComposerView does the work).
 */
#pragma once

#include "page.h"
#include "tool_tabs.h"

#include <string>
#include <vector>

class ComposerView;
namespace nanogui {
class Button;
class Label;
class ScrollPanel;
}

class MastersPanel : public ToolPanel {
public:
    MastersPanel(nanogui::Widget *parent, ComposerView *view);

    std::string title() const override { return "Masters"; }
    void refresh() override;
    nanogui::Vector2i floor_size() const override { return nanogui::Vector2i(200, 420); }

    /* Panel commands, also used by the menus. */
    void new_master();
    void master_from_page();
    void master_options(pagemade::MasterId id);
    void numbering_options();
    void apply_to_pages(pagemade::MasterId id);

private:
    class Grid;

    void selection_changed();
    /* The chosen master (0: [None] or nothing chosen); `none` when [None]. */
    pagemade::MasterId chosen(bool *none = nullptr) const;
    void edit_or_done();
    void delete_chosen();
    /* Delete with [None] chosen: its pages go to a master. */
    void delete_none();
    std::vector<std::pair<pagemade::MasterId, std::string>> parents_for(pagemade::MasterId id) const;

    ComposerView *m_view = nullptr;
    nanogui::ScrollPanel *m_scroll = nullptr;
    Grid *m_grid = nullptr;
    nanogui::Label *m_info = nullptr, *m_banner = nullptr, *m_banner_where = nullptr,
                   *m_banner_info = nullptr, *m_banner_hint = nullptr;
    nanogui::Widget *m_banner_box = nullptr;
    nanogui::Button *m_revert = nullptr;
    nanogui::Button *m_new = nullptr, *m_from_page = nullptr, *m_edit = nullptr, *m_apply = nullptr,
                    *m_default = nullptr, *m_options = nullptr, *m_delete = nullptr,
                    *m_numbering = nullptr;
    std::string m_sig;
};
