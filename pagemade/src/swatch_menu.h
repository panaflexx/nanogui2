/*
 * pagemade/swatch_menu.h — the Colors ("C") dropdown and the swatch editor.
 *
 * The dropdown lists the open publication's swatches: a color chip, the
 * name, and the CMYK definition. Picking a row paints the selection (fill
 * for shapes, ink for text, border for pictures). Custom rows carry an
 * edit mark that opens the editor without choosing; built-ins show a lock
 * and open read-only. A "+" footer row adds a new swatch.
 *
 * The editor is a ModalDialog: name, a lock toggle top-right, a color
 * wheel, RGB or CMYK sliders (a mode menu switches), a hex readout and a
 * preview chip. CMYK is the truth; the sliders convert both ways. The
 * globe button copies the swatch into the global colors.conf library.
 * Custom swatches also offer Delete.
 */
#pragma once

#include "page.h"

#include <nanogui/menu.h>
#include <nanogui/screen.h>

#include <functional>
#include <string>
#include <vector>

namespace pagemade {

struct Cmyk;

struct SwatchMenuView {
    /* The publication's swatches, in table order. */
    std::vector<Swatch> swatches;
    /* The current paint, for the check mark; kNoPaint hides it. */
    SwatchId current = 0;
};

/* Rebuild the dropdown from the view. `pick` paints the selection with
 * the swatch id. `edit` opens the editor for it. The "+" row calls
 * `add`. The button face shows the current paint (chip plus name); the
 * menu keeps its layout when open. */
void refill_swatch_menu(nanogui::Dropdown *menu, const SwatchMenuView &view,
                        const std::function<void(SwatchId)> &pick,
                        const std::function<void(SwatchId)> &edit,
                        const std::function<void()> &add);

/* The Colors button: a ComboBox whose closed face is the current paint
 * (chip plus name). Create one per toolbar; refill_swatch_menu drives it. */
class ColorDropdown : public nanogui::Dropdown {
public:
    explicit ColorDropdown(nanogui::Widget *parent);
    /* Show this paint on the button; an empty name clears the chip. */
    void set_swatch(const std::string &name, const Color &color);
    void clear_swatch();

    nanogui::Vector2i preferred_size(NVGcontext *ctx) const override;
    void draw(NVGcontext *ctx) override;

private:
    Color m_chip{1, 1, 1, 1};
    bool m_has_chip = false;
};

struct SwatchEdit {
    std::string name;
    Cmyk cmyk;
    /* True when the swatch is already in the global library. */
    bool in_library = false;
};

/* The swatch editor dialog. `initial` is the swatch as it stands; `save`
 * gets the edited name and CMYK (Rename/recolor in one undo step at the
 * call site). `remove` deletes a custom swatch (null for built-ins, which
 * stay locked). `to_library` copies it into colors.conf. `locked` opens
 * read-only: built-ins are always locked. */
void ask_swatch(nanogui::Screen *screen, const SwatchEdit &initial, bool locked,
                std::function<void(const std::string &, const Cmyk &)> save,
                std::function<void()> remove,
                std::function<void(const std::string &, const Cmyk &)> to_library);

} // namespace pagemade
