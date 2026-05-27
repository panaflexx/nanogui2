#pragma once
#include <nanogui/common.h>
#include <string>

struct JSRuntime;
struct JSContext;

namespace nanogui {

/**
 * NanoJS — owns a QuickJS runtime + context and integrates with NanoGUI's
 * render loop.
 *
 * Typical usage:
 *   NanoJS js;
 *   js.eval_file("app.mjs");       // boot script
 *   // inside render loop:
 *   js.pump();                     // drain microtasks / promise continuations
 */
class NANOGUI_EXPORT NanoJS {
public:
    NanoJS();
    ~NanoJS();

    // Non-copyable, non-movable
    NanoJS(const NanoJS&)            = delete;
    NanoJS& operator=(const NanoJS&) = delete;
    NanoJS(NanoJS&&)                 = delete;
    NanoJS& operator=(NanoJS&&)      = delete;

    /**
     * Evaluate a JS source string.
     * @param source     JavaScript source text.
     * @param filename   Used in stack traces / error messages.
     * @param as_module  When true, evaluates as an ES module
     *                   (enables import/export).
     * @return true on success, false if a JS exception was thrown
     *         (exception is printed to stderr; the runtime stays alive).
     */
    bool eval_string(const std::string& source,
                     const std::string& filename = "<string>",
                     bool as_module = false);

    /**
     * Load and evaluate a .js / .mjs file.
     * Module mode is selected automatically when:
     *   - the file extension is ".mjs", OR
     *   - the file content starts with "import " or contains "\nimport "
     * @return true on success.
     */
    bool eval_file(const std::string& path);

    /**
     * Drain all pending microtasks and promise continuations.
     * Call once per frame from NanoGUI's render loop.
     * Returns true if all jobs completed cleanly, false if any job threw.
     */
    bool pump();

    // Raw handles — for advanced use (e.g. binding registration).
    JSContext* context() const { return m_ctx; }
    JSRuntime* runtime() const { return m_rt; }

private:
    // Prints the pending JS exception from `ctx` (defaults to m_ctx) to
    // stderr and clears it. Handles Error objects, plain throws, and the
    // case where no exception is pending (no-op).
    void dump_error(JSContext* ctx = nullptr);

    // Registers print() / console.log / .warn / .error on the global object.
    void setup_console();

    JSRuntime* m_rt = nullptr;
    JSContext* m_ctx = nullptr;
};

} // namespace nanogui
