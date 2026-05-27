/*
 * nanojs_cli.cpp — Standalone NanoJS script runner.
 *
 * Usage:
 *   nanojs <script.js>          # evaluate a JS or MJS file
 *   nanojs -e 'expr'            # evaluate an expression from the command line
 *   nanojs --help               # show usage
 *
 * Exit code 0 on success, 1 on JS exception or bad arguments.
 *
 * Module auto-detection (same rules as NanoJS::eval_file):
 *   - .mjs extension  → ES module mode (import/export)
 *   - file starts with "import "  → ES module mode
 *   - otherwise  → global script mode
 */

#include "nanojs.h"

#include <cstdio>
#include <cstring>

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <script.js|script.mjs>   evaluate a script file\n"
        "  %s -e 'expression'          evaluate an expression\n"
        "  %s --help                   show this message\n",
        prog, prog, prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    nanogui::NanoJS js;

    // The script label used in error messages.
    const char* label = (strcmp(argv[1], "-e") == 0) ? "<cmdline>" : argv[1];

    bool ok = false;

    if (strcmp(argv[1], "-e") == 0) {
        if (argc < 3) {
            fprintf(stderr, "nanojs: -e requires an expression argument\n");
            return 1;
        }
        ok = js.eval_string(argv[2], "<cmdline>");
    } else {
        ok = js.eval_file(argv[1]);
    }

    // Drain remaining microtasks / promise continuations.
    // eval_string already pumps for module mode, but additional promises
    // scheduled during eval (e.g. from async callbacks) need a second pass.
    bool pump_ok = js.pump();
    ok = ok && pump_ok;

    if (!ok) {
        // The specific error was already printed to stderr by dump_error().
        // Print a short summary line so the user knows which script failed.
        fprintf(stderr, "nanojs: script failed: %s\n", label);
        fflush(stderr);
    }

    return ok ? 0 : 1;
}
