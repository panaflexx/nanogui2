#include "nanojs.h"

#include <quickjs.h>
#include "../js/js_module.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace nanogui {

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

NanoJS::NanoJS() {
    m_rt = JS_NewRuntime();
    if (!m_rt) {
        fprintf(stderr, "[NanoJS] Failed to create JSRuntime\n");
        return;
    }

    // Reasonable memory ceiling — prevents runaway scripts from OOM-killing
    // the host process.  64 MiB is plenty for UI scripting.
    JS_SetMemoryLimit(m_rt, 64u * 1024u * 1024u);

    m_ctx = JS_NewContext(m_rt);
    if (!m_ctx) {
        fprintf(stderr, "[NanoJS] Failed to create JSContext\n");
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
        return;
    }

    // Expose the 'nanogui' ES module so scripts can do:
    //   import { Widget, Button } from 'nanogui';
    nanogui::js::register_nanogui_module(m_rt, m_ctx);

    // Register print() and console.{log,warn,error} on the global object.
    // These write to stdout/stderr with a trailing newline, joining args
    // with spaces — enough for test scripts and basic debugging.
    setup_console();
}

NanoJS::~NanoJS() {
    if (m_ctx) {
        JS_FreeContext(m_ctx);
        m_ctx = nullptr;
    }
    if (m_rt) {
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
    }
}

// Forward declaration — defined later in this file.
static void print_exception(JSContext* ctx, JSValue exc);

// ---------------------------------------------------------------------------
// Console builtins
// ---------------------------------------------------------------------------

static JSValue js_print_impl(JSContext* ctx, JSValueConst,
                              int argc, JSValueConst* argv, FILE* out) {
    for (int i = 0; i < argc; ++i) {
        if (i) fputc(' ', out);
        const char* s = JS_ToCString(ctx, argv[i]);
        if (!s) return JS_EXCEPTION;
        fputs(s, out);
        JS_FreeCString(ctx, s);
    }
    fputc('\n', out);
    return JS_UNDEFINED;
}

static JSValue js_print(JSContext* ctx, JSValueConst tv, int argc, JSValueConst* argv)
    { return js_print_impl(ctx, tv, argc, argv, stdout); }
static JSValue js_console_log(JSContext* ctx, JSValueConst tv, int argc, JSValueConst* argv)
    { return js_print_impl(ctx, tv, argc, argv, stdout); }
static JSValue js_console_warn(JSContext* ctx, JSValueConst tv, int argc, JSValueConst* argv)
    { return js_print_impl(ctx, tv, argc, argv, stderr); }
static JSValue js_console_error(JSContext* ctx, JSValueConst tv, int argc, JSValueConst* argv)
    { return js_print_impl(ctx, tv, argc, argv, stderr); }

void NanoJS::setup_console() {
    JSValue global = JS_GetGlobalObject(m_ctx);

    JS_SetPropertyStr(m_ctx, global, "print",
        JS_NewCFunction(m_ctx, js_print, "print", 1));

    JSValue console = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, console, "log",
        JS_NewCFunction(m_ctx, js_console_log,   "log",   1));
    JS_SetPropertyStr(m_ctx, console, "warn",
        JS_NewCFunction(m_ctx, js_console_warn,  "warn",  1));
    JS_SetPropertyStr(m_ctx, console, "error",
        JS_NewCFunction(m_ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(m_ctx, global, "console", console);

    JS_FreeValue(m_ctx, global);
}

// ---------------------------------------------------------------------------
// eval_string
// ---------------------------------------------------------------------------

bool NanoJS::eval_string(const std::string& source,
                         const std::string& filename,
                         bool as_module) {
    if (!m_ctx)
        return false;

    int flags = as_module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;

    JSValue result = JS_Eval(m_ctx,
                             source.c_str(),
                             source.size(),
                             filename.c_str(),
                             flags);

    bool had_exception = JS_IsException(result);
    JS_FreeValue(m_ctx, result);

    if (had_exception) {
        // Synchronous error flagged in the return value (syntax, link, global
        // runtime errors).
        dump_error(m_ctx);
        return false;
    }

    // Defensive check: QuickJS's module evaluator sometimes stores an
    // exception in the context without setting the JS_EXCEPTION tag on the
    // return value (observed for ReferenceErrors in module bodies).  Peek
    // at the context's current exception without clearing it by temporarily
    // retrieving it.
    {
        JSValue exc = JS_GetException(m_ctx);
        bool pending = !JS_IsNull(exc) && !JS_IsUndefined(exc);
        if (pending) {
            print_exception(m_ctx, exc);
            JS_FreeValue(m_ctx, exc);
            return false;
        }
        JS_FreeValue(m_ctx, exc);
    }

    // Drain any pending jobs (module body deferred execution, promises, etc.)
    // and propagate failures.
    if (!pump()) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// eval_file
// ---------------------------------------------------------------------------

bool NanoJS::eval_file(const std::string& path) {
    if (!m_ctx)
        return false;

    // Read the whole file into a string.
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[NanoJS] Cannot open file: %s\n", path.c_str());
        return false;
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    if (file.fail() && !file.eof()) {
        fprintf(stderr, "[NanoJS] Error reading file: %s\n", path.c_str());
        return false;
    }
    std::string content = oss.str();

    // Decide whether to treat the file as an ES module.
    //
    // Rule 1: .mjs extension → always a module.
    bool is_module = (path.size() >= 4 &&
                      path.compare(path.size() - 4, 4, ".mjs") == 0);

    // Rule 2: content starts with "import " (possibly after a UTF-8 BOM).
    if (!is_module) {
        // Skip optional UTF-8 BOM (EF BB BF).
        const char* src = content.c_str();
        if (content.size() >= 3 &&
            static_cast<unsigned char>(src[0]) == 0xEF &&
            static_cast<unsigned char>(src[1]) == 0xBB &&
            static_cast<unsigned char>(src[2]) == 0xBF) {
            src += 3;
        }
        if (content.compare(src - content.c_str(), 7, "import ") == 0)
            is_module = true;
    }

    // Rule 3: content contains a newline-prefixed "import " (module somewhere
    // inside the file).
    if (!is_module) {
        is_module = (content.find("\nimport ") != std::string::npos);
    }

    return eval_string(content, path, is_module);
}

// ---------------------------------------------------------------------------
// pump — drain pending microtasks / promise continuations
// ---------------------------------------------------------------------------

bool NanoJS::pump() {
    if (!m_rt)
        return true;

    JSContext* ctx = nullptr;
    int ret;
    while ((ret = JS_ExecutePendingJob(m_rt, &ctx)) > 0) {
        // A job ran OK; keep draining.
    }
    if (ret < 0) {
        // A job threw — report from whichever context it ran in.
        dump_error(ctx ? ctx : m_ctx);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// dump_error — print any pending exception from a context to stderr
// ---------------------------------------------------------------------------

// Static helper: prints `exc` (already retrieved) to stderr.
// Works for Error objects, plain strings, and anything else.
static void print_exception(JSContext* ctx, JSValue exc) {
    if (JS_IsError(exc)) {
        // Standard Error object — print message then stack trace.
        JSValue msg   = JS_GetPropertyStr(ctx, exc, "message");
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");

        const char* msg_str   = JS_ToCString(ctx, msg);
        const char* stack_str = JS_ToCString(ctx, stack);

        fprintf(stderr, "Uncaught %s\n",
                msg_str ? msg_str : "(unknown error)");
        if (stack_str && stack_str[0])
            fprintf(stderr, "%s\n", stack_str);

        JS_FreeCString(ctx, msg_str);
        JS_FreeCString(ctx, stack_str);
        JS_FreeValue(ctx, msg);
        JS_FreeValue(ctx, stack);
    } else {
        // Non-Error throw (plain string, number, object, …)
        const char* s = JS_ToCString(ctx, exc);
        fprintf(stderr, "Uncaught (non-Error): %s\n",
                s ? s : "(unrepresentable value)");
        if (s) JS_FreeCString(ctx, s);
    }
    fflush(stderr);
}

void NanoJS::dump_error(JSContext* ctx) {
    if (!ctx) ctx = m_ctx;
    if (!ctx) return;

    JSValue exc = JS_GetException(ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
        JS_FreeValue(ctx, exc);
        return;   // nothing pending
    }
    print_exception(ctx, exc);
    JS_FreeValue(ctx, exc);
}

} // namespace nanogui
