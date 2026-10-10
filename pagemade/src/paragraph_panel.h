/*
 * pagemade/paragraph_panel.h — the Paragraph panel the ¶ button pops open,
 * laid out after VectorStyler's: the style menu, seven alignments, line
 * spacing and extra spacing, the four indents, space before and after, and
 * bullets and numbering, the drop cap (lines, size, characters), keep options (orphans, widows,
 * keep together, keep with next, start in the next text block),
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
    DropCap, DropSize, DropChars,
    Orphans, Widows, KeepTogether, KeepWithNext, StartNewFrame,
    List, ListStart,
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
    struct IntField {
        IntSpin *box = nullptr;
        int pagemade::ParaStyle::*member = nullptr;
        int lo = 0, hi = 0;
    };
    class IconButton;

    void add_field(nanogui::Widget *grid, ParaIcon icon, const char *tip,
                   float pagemade::ParaStyle::*member, float scale, float lo, float hi,
                   float step, const char *format, const char *units);
    void add_int_field(nanogui::Widget *grid, ParaIcon icon, const char *tip,
                       int pagemade::ParaStyle::*member, int lo, int hi, const char *units);

    Hooks m_hooks;
    nanogui::Dropdown *m_style = nullptr;
    nanogui::Dropdown *m_list = nullptr;
    std::vector<std::string> m_style_names;
    std::string m_current_style;         // the selection's style, when it has one
    std::vector<IconButton *> m_align;
    std::vector<IconButton *> m_flags;   // keep together, keep with next, new frame
    std::vector<Field> m_fields;
    std::vector<IntField> m_int_fields;
    pagemade::ParaStyle m_first;         // the first selected paragraph's
    nanogui::Button *m_delete = nullptr, *m_new = nullptr;
    bool m_loading = false;
};

/* The Character panel, popped open beside the Paragraph panel: the
 * character style menu, the text's color (a swatch) and tint, and Remove /
 * Add for styles. Like the Paragraph panel it edits through Hooks and
 * shows the selection through load(). */
class CharacterPanel : public nanogui::Widget {
public:
    struct Hooks {
        std::function<void(const std::string &)> apply_style;   // "" = none
        std::function<void()> new_style;
        std::function<void(const std::string &)> delete_style;
        std::function<void(uint32_t swatch)> swatch;
        /* `spin`: the field when this is a spin step, else nullptr. */
        std::function<void(float tint, const void *spin)> tint;
    };
    /* What the selection is set in. */
    struct State {
        bool enabled = false;
        std::string style;                 // character style name, "" = none
        bool mixed_style = false, overridden = false;
        uint32_t swatch = 0;               // 0: an RGB color, not a swatch
        bool mixed_color = false;
        float tint = 100.f;
        bool mixed_tint = false;
        pagemade::Color shown;             // the color the text draws in
    };

    CharacterPanel(nanogui::Widget *parent, Hooks hooks);
    void load(const State &state, const std::vector<pagemade::CharStyleDef> &styles,
              const std::vector<pagemade::Swatch> &swatches);
    void draw(NVGcontext *ctx) override;

private:
    class Chip;
    Hooks m_hooks;
    nanogui::Dropdown *m_style = nullptr, *m_color = nullptr;
    FloatSpin *m_tint = nullptr;
    Chip *m_chip = nullptr;
    nanogui::Button *m_delete = nullptr, *m_new = nullptr;
    std::vector<std::string> m_style_names;
    std::vector<std::string> m_color_names;
    std::vector<uint32_t> m_swatch_ids;
    std::string m_current;
    float m_first_tint = 100.f;
    bool m_loading = false;
};
