#pragma once

#include <quickjs.h>

// Forward-declare Screen so callers of this header need not pull in the full
// nanogui headers unless they actually use the Screen* member below.
namespace nanogui { class Screen; }

namespace nanogui::js {

// ---------------------------------------------------------------------------
// ScreenOpaque
//
// Stored as the QuickJS opaque pointer for every Screen JS object.
// Exposed in this header (rather than kept private to js_screen.cpp) so that
// js_window.cpp can cast a Screen JS value back to a Widget* without needing
// to know the full Screen class layout — it just takes `opaque->screen`.
// ---------------------------------------------------------------------------
struct ScreenOpaque {
    nanogui::Screen* screen;
    bool             owned;      ///< true  → JS-created; finalizer deletes screen
                                 ///< false → app-owned; finalizer does not delete

    // JS callback storage — JS_UNDEFINED when not set.
    // Freed with JS_FreeValueRT() in the finalizer.
    JSContext* ctx        = nullptr;   ///< context needed to invoke callbacks
    JSValue    on_resize  = JS_UNDEFINED; ///< function(width, height)
};

// The prototype JSValue placed on every Screen JS instance.
// Valid after register_screen_class() has been called; undefined before.
extern JSValue screen_proto;

// The QuickJS class ID for the Screen class.
// Exposed here (rather than kept as a file-static) so that js_window.cpp can
// call JS_GetOpaque(val, js_screen_class_id) when unwrapping a parent that
// might be a Screen rather than a plain Widget.
extern JSClassID js_screen_class_id;

/// Register the Screen JS class, add its prototype to widget_proto's chain,
/// and export the "Screen" constructor through the module `m`.
/// Must be called from the module-init callback, after register_widget_class().
void register_screen_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m);

/// Wrap an existing (app-owned) Screen* into a new JS object.
/// The JS object does NOT take ownership; the C++ Screen must outlive the
/// JS value.  Returns JS_EXCEPTION on allocation failure.
JSValue wrap_screen(JSContext* ctx, nanogui::Screen* s);

} // namespace nanogui::js
