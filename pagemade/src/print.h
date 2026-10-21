/*
 * pagemade/print.h — hand a finished PDF to the system spooler.
 *
 * The print dialog does not include this. It only collects a printer
 * name, a copy count and a page range. The app writes that range to a
 * PDF, then calls print_pdf. Linux and macOS talk to CUPS through lp
 * and lpstat. Windows reports that printing is not available yet.
 */
#pragma once

#include <string>
#include <vector>

namespace pagemade {

struct Printer {
    std::string name;          // spooler name; empty means the system default
    std::string label;         // what the dialog shows
    bool is_default = false;
};

struct PrintRequest {
    std::string printer;       // empty: the system default
    int copies = 1;
    std::string title;         // the job name the queue shows
};

/* False where there is no spooler to call. The dialog still opens. */
bool printing_available();
const char *printing_unavailable_reason();

/* Queues this computer knows. Empty when none are listed; printing can
 * still go to the system default. */
std::vector<Printer> list_printers();

/* Queue `path`. An empty string means the spooler accepted the job.
 * Anything else is the reason it did not. */
std::string print_pdf(const std::string &path, const PrintRequest &request);

} // namespace pagemade
