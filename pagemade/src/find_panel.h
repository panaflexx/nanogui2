/*
 * pagemade/find_panel.h — the Find & Replace window.
 *
 * Find and Replace boxes, Match Case and Whole Word, then Find All and
 * Replace All above a list of every find in the publication: its page
 * and its text in context, the find highlighted (yellow, or green once
 * replaced), and a button that replaces that one find or puts it back.
 * Choosing a row selects the text on its page. Each replace or revert is
 * one undo step, and Replace All is one for all of them.
 *
 * The list follows the document: after an undo, a redo or typing, each
 * find is looked for again (find_replace.h). One that has changed is
 * dimmed and its button does nothing until the next search.
 *
 * It docks beside the pages like the other tool windows, and is only
 * hidden when closed.
 */
#pragma once

#include "tool_tabs.h"

#include "find_replace.h"

#include <memory>
#include <string>
#include <vector>

class ComposerView;
namespace nanogui {
class Button;
class CheckBox;
class DataGrid;
class Label;
class TextBox;
}

class FindPanel : public ToolPanel {
public:
    FindPanel(nanogui::Widget *parent, ComposerView *view);

    std::string title() const override { return "Find & Replace"; }
    nanogui::Vector2i floor_size() const override { return nanogui::Vector2i(280, 300); }
    void refresh() override;
    void perform_layout(NVGcontext *ctx) override;

    /* Put the cursor in the Find box, with what the page has selected in
     * it when that is a short run of text on one line. */
    void focus_find();

    /* The row's find, its text around it and its page label, for the list */
    struct Row {
        pagemade::FoundText find;
        std::string head, tail;          // context before and after
        std::string page;                // "3", or "—" in the overset
    };
    const std::vector<Row> &rows() const { return m_rows; }

private:
    void find_all();
    void replace_all();
    void toggle(size_t row);             // Replace, or Revert
    void go_to(size_t row);
    /* Locate the finds again and rebuild their context and pages */
    void resolve();
    void update_status();
    pagemade::FindOptions options() const;

    ComposerView *m_view = nullptr;
    nanogui::TextBox *m_find = nullptr, *m_replace = nullptr;
    nanogui::CheckBox *m_case = nullptr, *m_word = nullptr;
    nanogui::Button *m_find_all = nullptr, *m_replace_all = nullptr;
    nanogui::DataGrid *m_grid = nullptr;
    nanogui::Label *m_status = nullptr;
    std::shared_ptr<class FindModel> m_model;
    std::vector<Row> m_rows;
    std::string m_needle;                // what the list was found with
    bool m_busy = false;                 // our own edit: refresh() leaves the list alone
};
