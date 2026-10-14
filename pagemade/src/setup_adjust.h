/*
 * pagemade/setup_adjust.h — changing Document Setup under a finished layout.
 *
 * A new page size, margins, columns or facing pages can leave the items
 * where they are, scale the whole layout to the new page, or adjust it:
 *
 *   Adjust  Each page's edges, margins and column edges map to the new
 *           ones, and positions between them stretch in proportion. A
 *           block that filled a column fills the new column; one on the
 *           right margin stays on it. Text reflows at its own size, and a
 *           picture keeps its proportions (its width follows the layout).
 *           A group moves and stretches as one. Ruler guides follow.
 *   Scale   Everything, type included, scales by the smaller of the width
 *           and height ratios and is centered on the new page: the same
 *           look at a new size.
 *   Leave   Only the setup changes.
 */
#pragma once

#include "page.h"

namespace pagemade {

enum class SetupAdjust { Adjust, Scale, Leave };

void change_setup(PageDoc &doc, const PageSetup &to, SetupAdjust how);

/* Type and paragraph measurements times `s` (sizes, leading, indents,
 * spacing, tabs): what Scale does to every story and style. */
void scale_char_style(CharStyle &cs, float s);
void scale_para_style(ParaStyle &ps, float s);

} // namespace pagemade
