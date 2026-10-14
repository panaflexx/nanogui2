/*
 * pagemade/pdf_dialog.h — the PDF settings dialog (File > Export PDF,
 * File > PDF Settings).
 *
 * A preset to start from (High quality print, Prodigi photo book, Press
 * PDF/X-4, Smallest file), then every setting: pages, picture resolution
 * and compression, color, PDF/X-4 and its output intent, bleed and crop
 * marks, the title and author, and the check before export. Changing a
 * setting makes the preset Custom. CMYK conversion is listed and greyed
 * until it exists.
 */
#pragma once

#include "page.h"

#include <functional>
#include <string>

namespace nanogui { class Screen; }

/* `page_count` bounds the page range; `bleed` is the Document Setup bleed,
 * shown beside the bleed switch. `ok_caption` names the button ("Export…"
 * or "OK"); `done` gets the settings when it is pressed. */
void ask_pdf_settings(nanogui::Screen *screen, const pagemade::PdfSettings &current,
                      int page_count, float bleed, const std::string &ok_caption,
                      std::function<void(const pagemade::PdfSettings &)> done);

/* The presets, by name, applied to `s` (pages and document info are kept). */
const std::vector<std::string> &pdf_preset_names();
void apply_pdf_preset(const std::string &name, pagemade::PdfSettings &s);
