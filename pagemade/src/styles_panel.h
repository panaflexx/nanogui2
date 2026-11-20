/*
 * pagemade/styles_panel.h — the Styles panel: the publication's paragraph
 * and character styles in one place.
 *
 * The list shows one row per style: its name, a preview set in it by the
 * composer, how many paragraphs (or runs) use it, and Edit, Merge and
 * Remove. ▸ marks the style at the caret; ⚠ marks a near-duplicate name
 * ("Body text 2" beside "Body text"), the usual leftover of an import.
 * Below, Add Style (from the selected paragraph, or Normal) and Add Based
 * On (a copy of the highlighted style), and Apply.
 *
 * Edit opens the style in the same panel: name, next style (paragraph
 * styles), a preview at its real size, and its attributes on two pages,
 * Type (the Type Specs fields) and Paragraph (the Paragraph panel's), or
 * for a character style Type and Sets (which fields the style sets, and
 * its color). OK writes it back as one undo step; paragraphs set in the
 * style follow unless they override it. Cancel leaves it as it was.
 *
 * The panel docks like the others and stays usable in the story editor.
 */
#pragma once

#include "tool_tabs.h"

#include "page.h"

#include <memory>
#include <string>

class ComposerView;
namespace pagemade { class FontLibrary; class FontMenuModel; }

class StylesPanel : public ToolPanel {
public:
    StylesPanel(nanogui::Widget *parent, ComposerView *view, pagemade::FontLibrary &fonts,
                pagemade::FontMenuModel &fonts_menu);
    ~StylesPanel() override;

    std::string title() const override { return "Styles"; }
    nanogui::Vector2i floor_size() const override { return nanogui::Vector2i(280, 380); }
    bool story_safe() const override { return true; }   // edits the text's styles
    void refresh() override;
    void perform_layout(NVGcontext *ctx) override;

    /* Show the paragraph styles (false) or the character styles. */
    void show_kind(bool character);

private:
    struct State;
    std::unique_ptr<State> m_state;
};
