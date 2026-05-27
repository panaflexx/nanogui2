// js_module.cpp — "nanogui" QuickJS-NG module registration
//
// Usage (in the host application, before evaluating JS):
//
//   JSRuntime* rt  = JS_NewRuntime();
//   JSContext* ctx = JS_NewContext(rt);
//   nanogui::js::register_nanogui_module(rt, ctx);
//   // Now JS code may: import { Screen, Window, Widget } from 'nanogui';
//
// Initialisation sequence inside the module init callback:
//
//   1. register_widget_class  — establishes widget_proto (base for all others)
//   2. register_screen_class  — prototype inherits from widget_proto
//   3. register_window_class  — prototype inherits from widget_proto
//
// Each register_*_class() call exports its constructor through the module
// definition `m` using JS_SetModuleExport().

#include <quickjs.h>

#include "js_widget.h"   // register_widget_class
#include "js_screen.h"   // register_screen_class
#include "js_window.h"   // register_window_class
#include "js_label.h"    // register_label_class
#include "js_button.h"   // register_button_class
#include "js_layout.h"   // register_layout_classes
#include "js_module.h"

namespace nanogui::js {

// ---------------------------------------------------------------------------
// Module init callback
// ---------------------------------------------------------------------------
// Called by the QuickJS engine when the "nanogui" module is first instantiated
// (i.e. the first time JS code executes `import ... from 'nanogui'`).

// Forward declarations for free functions defined in js_screen.cpp
// (must match the nanogui::js namespace they were defined in)
extern JSValue js_nanogui_init    (JSContext*, JSValueConst, int, JSValueConst*);
extern JSValue js_nanogui_shutdown(JSContext*, JSValueConst, int, JSValueConst*);
extern JSValue js_nanogui_mainloop(JSContext*, JSValueConst, int, JSValueConst*);

static int js_nanogui_module_init(JSContext* ctx, JSModuleDef* m) {
    JSRuntime* rt = JS_GetRuntime(ctx);

    // Order matters: widget_proto must exist before screen/window try to
    // inherit from it.
    register_widget_class(ctx, rt, m);
    register_screen_class(ctx, rt, m);
    register_window_class(ctx, rt, m);
    register_label_class(ctx, rt, m);
    register_button_class(ctx, rt, m);
    register_layout_classes(ctx, rt, m);

    // Free functions
    JS_SetModuleExport(ctx, m, "init",
        JS_NewCFunction(ctx, js_nanogui_init,     "init",     0));
    JS_SetModuleExport(ctx, m, "shutdown",
        JS_NewCFunction(ctx, js_nanogui_shutdown, "shutdown", 0));
    JS_SetModuleExport(ctx, m, "mainloop",
        JS_NewCFunction(ctx, js_nanogui_mainloop, "mainloop", 1));

    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void register_nanogui_module(JSRuntime* /*rt*/, JSContext* ctx) {
    // Create the "nanogui" module definition and store the init callback.
    // JS_NewCModule returns nullptr on OOM; callers should handle that by
    // checking for exceptions after calling JS_Eval.
    JSModuleDef* m = JS_NewCModule(ctx, "nanogui", js_nanogui_module_init);
    if (!m)
        return;

    // Pre-declare every name that the init callback will export via
    // JS_SetModuleExport().  These declarations must be made before the module
    // is instantiated (i.e. before any import resolves to it).
    JS_AddModuleExport(ctx, m, "Widget");
    JS_AddModuleExport(ctx, m, "Screen");
    JS_AddModuleExport(ctx, m, "Window");
    JS_AddModuleExport(ctx, m, "Label");
    JS_AddModuleExport(ctx, m, "Button");
    JS_AddModuleExport(ctx, m, "BoxLayout");
    JS_AddModuleExport(ctx, m, "Orientation");
    JS_AddModuleExport(ctx, m, "Alignment");
    JS_AddModuleExport(ctx, m, "init");
    JS_AddModuleExport(ctx, m, "shutdown");
    JS_AddModuleExport(ctx, m, "mainloop");
}

} // namespace nanogui::js
