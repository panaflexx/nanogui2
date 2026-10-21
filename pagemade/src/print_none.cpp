/*
 * pagemade/print_none.cpp — printing where no spooler is wired up.
 */
#include "print.h"

namespace pagemade {

bool printing_available() { return false; }

const char *printing_unavailable_reason() {
    return "Printing isn't available on Windows yet. Use Export PDF.";
}

std::vector<Printer> list_printers() { return {}; }

std::string print_pdf(const std::string &, const PrintRequest &) {
    return printing_unavailable_reason();
}

} // namespace pagemade
