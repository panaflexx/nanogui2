/*
 * pagemade/print_dialog.h — File > Print.
 *
 * Printer, copies, and which pages. This file does not know how a job
 * is spooled: the caller hands it the printer list and receives the
 * choice. `available` is false when this computer cannot print; the
 * dialog says why and leaves Print disabled.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace nanogui { class Screen; }

namespace pagemade { struct Printer; }

enum class PrintPages { All, Current, Range };

struct PrintChoice {
    std::string printer;                 // empty: the system default
    int copies = 1;
    PrintPages pages = PrintPages::All;
    int first_page = 1, last_page = 1;   // 1-based; Current uses both
};

void ask_print(nanogui::Screen *screen, const std::vector<pagemade::Printer> &printers,
               int page_count, int current_page, const PrintChoice &initial,
               bool available, const std::string &unavailable_reason,
               std::function<void(const PrintChoice &)> done);
