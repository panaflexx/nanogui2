/*
 * pagemade/paragraph_panel.h — the Paragraph panel the ¶ button pops open,
 * laid out after VectorStyler's: the style menu, seven alignments, line
 * spacing and extra spacing, the four indents, and space before and after,
 * with Remove (delete the style) and Add (new style from the selection)
 * below.
 *
 * The panel edits the selected paragraphs through Hooks and shows them
 * through load(); it owns no document state. Its number fields spin
 * (spinbox.h). A field is blank when the paragraphs differ, and the style
 * menu reads "Name+" when the paragraph is formatted differently from its
 * style.
 */
#pragma once

#include "page.h"
#include "spinbox.h"

#include <nanogui/widget.h>

#include <functional>
#include <string>
#include <vector>

namespace nanogui { class Dropdown; class Button; }

/* The icons the panel draws, in the style of its reference: stacks of
 * text lines. */
enum class ParaIcon {
    AlignLeft, AlignCenter, AlignRight,
    JustifyLeft, JustifyCenter, JustifyRight, JustifyAll,
    LineSpacing, ExtraSpacing,
    LeftIndent, RightIndent, FirstIndent, LastIndent,
    SpaceBefore, SpaceAfter,
};
void draw_para_icon(NVGcontext *ctx, ParaIcon icon, float x, float y, float w, float h,
                    const nanogui::Color &color);

class ParagraphPanel : public nanogui::Widget {
public:
    struct Hooks {
        /* Change the selected paragraphs. `spin` is the field when this is
         * a spin step (a run of them is one undo step), else nullptr. */
        std::function<void(const std::function<void(pagemade::ParaStyle &)> &,
                           const void *spin)> apply;
        std::function<void(pagemade::Align)> align;
        std::function<void(const std::string &)> apply_style;
        std::function<void()> new_style;
        std::function<void(const std::string &)> delete_style;
    };

    ParagraphPanel(nanogui::Widget *parent, Hooks hooks);

    /* Show these paragraphs (none disables the panel) and the document's
     * styles. */
    void load(const std::vector<const pagemade::Paragraph *> &paras,
              const std::vector<pagemade::StyleDef> &styles);

    /* An opaque backing inside the popup's glass, so the page doesn't show
     * through the fields. */
    void draw(NVGcontext *ctx) override;

private:
    struct Field {
        FloatSpin *box = nullptr;
        float pagemade::ParaStyle::*member = nullptr;
        float scale = 1;                 // shown = member * scale
        float lo = 0, hi = 0;
    };
    class AlignButton;

    void add_field(nanogui::Widget *grid, ParaIcon icon, const char *tip,
                   float pagemade::ParaStyle::*member, float scale, float lo, float hi,
                   float step, const char *format, const char *units);

    Hooks m_hooks;
    nanogui::Dropdown *m_style = nullptr;
    std::vector<std::string> m_style_names;
    std::string m_current_style;         // the selection's style, when it has one
    std::vector<AlignButton *> m_align;
    std::vector<Field> m_fields;
    pagemade::ParaStyle m_first;         // the first selected paragraph's
    nanogui::Button *m_delete = nullptr, *m_new = nullptr;
    bool m_loading = false;
};
