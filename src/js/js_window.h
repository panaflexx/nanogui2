#pragma once

#include <quickjs.h>

namespace nanogui { class Window; }

namespace nanogui::js {

// ---------------------------------------------------------------------------
// WindowOpaque
//
// Stored as the QuickJS opaque pointer for every Window JS object.
// Holds the raw (non-owning) Window pointer and optional JS callbacks.
// ---------------------------------------------------------------------------
struct WindowOpaque {
    nanogui::Window* window;

    // JS callback storage — JS_UNDEFINED when not set.
    JSContext* ctx        = nullptr;
    JSValue    on_dispose = JS_UNDEFINED; ///< function() — called just before dispose()
};

// The prototype JSValue placed on every Window JS instance.
// Valid after register_window_class() has been called; undefined before.
extern JSValue window_proto;

/// Register the Window JS class, chain its prototype from widget_proto, and
/// export the "Window" constructor through the module `m`.
/// Must be called from the module-init callback, after register_widget_class().
void register_window_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m);

/// Wrap an existing (NanoGUI-tree-owned) Window* into a new JS object.
/// The JS object does NOT take ownership.  The C++ Window is owned by its
/// parent widget and will be destroyed when the parent tree is torn down.
/// Returns JS_EXCEPTION on allocation failure.
JSValue wrap_window(JSContext* ctx, nanogui::Window* w);

} // namespace nanogui::js
