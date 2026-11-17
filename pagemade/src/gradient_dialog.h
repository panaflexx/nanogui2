/*
 * pagemade/gradient_dialog.h — Fill > Gradient: a shape's gradient fill.
 *
 * The dialog sets the name, the kind (Linear, Radial), a linear ramp's angle,
 * and two to four color points: each a swatch at a tint, a place along the
 * ramp and an opacity. A strip across the top shows the ramp over a
 * checkerboard, so the opacities show. Named gradients live in the fill
 * menus beside the colors.
 */
#pragma once

#include "page.h"

#include <functional>

namespace nanogui {
class Screen;
}

namespace pagemade {

/* `name` empty fills the name in from the colors ("Red Blue") until it
 * is typed over. `remove`, when set, adds a Delete button (an existing
 * named gradient). `done` gets the name and gradient on OK. */
void ask_gradient(nanogui::Screen *screen, const std::vector<Swatch> &swatches,
                  const Gradient &initial, const std::string &name,
                  std::function<void(const std::string &, const Gradient &)> done,
                  std::function<void()> remove = {});

} // namespace pagemade
