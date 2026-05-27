/*
 * js_widget.h — QuickJS-NG binding declaration for nanogui::Widget.
 *
 * Widget is the C++ base class for all NanoGUI widgets.  Its JS class serves
 * as the prototype root: every other widget binding (Window, Button, …) sets
 * its prototype to `widget_proto` so that Widget's getters, setters and
 * methods are inherited.
 *
 * Usage (from a higher-level binding's register function):
 *
 *   #include "js_widget.h"
 *   ...
 *   // Inherit Widget's prototype:
 *   JSValue my_proto = JS_NewObjectProto(ctx, nanogui::js::widget_proto);
 */

#pragma once

#include <quickjs.h>

// Forward-declare so callers don't need to pull in all of widget.h just for
// the wrap_widget() declaration.
namespace nanogui { class Widget; }

namespace nanogui::js {

/**
 * The prototype JSValue for the Widget JS class.
 *
 * Set to JS_UNDEFINED until register_widget_class() is called.
 * Subclass bindings (js_window.cpp, js_button.cpp, …) must NOT free this
 * value; its lifetime matches the JSContext that was passed to
 * register_widget_class().
 */
extern JSValue widget_proto;

/**
 * Register the Widget JS class into the given QuickJS module.
 *
 * Must be called exactly once per JSContext, typically from the nanogui
 * module's JSModuleInitFunc.  After this call, `widget_proto` is live and
 * the "Widget" name is exported from `m`.
 *
 * @param ctx  The JS context that will own all created JS values.
 * @param rt   The JS runtime; required by JS_NewClassID / JS_NewClass.
 * @param m    The module definition that will export "Widget".
 */
void register_widget_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m);

/**
 * Wrap an existing nanogui::Widget* as a JS object.
 *
 * The returned JS object uses the Widget class and prototype; it holds a raw
 * (non-owning) pointer.  NanoGUI retains ownership of the C++ Widget and the
 * JS binding's finalizer does NOT delete it.
 *
 * Returns JS_NULL if `w` is nullptr.
 * Returns JS_EXCEPTION if JS object allocation fails.
 */
JSValue wrap_widget(JSContext* ctx, nanogui::Widget* w);

} // namespace nanogui::js
