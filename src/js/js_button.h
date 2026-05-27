/*
 * js_button.h — QuickJS-NG binding declaration for nanogui::Button.
 *
 * Button is owned by its parent Widget tree.  The JS binding does NOT own the
 * underlying C++ object; the NanoGUI widget tree manages its lifetime.
 *
 * The JS Button object stores a small ButtonOpaque heap allocation that holds
 * both the raw Button pointer and the JS callback for onClick.  The finalizer
 * frees the JS callback value and the opaque struct itself, but never deletes
 * the C++ Button.
 *
 * Usage from other binding translation units:
 *
 *   #include "js_button.h"
 *   JSValue btn_obj = nanogui::js::wrap_button(ctx, my_button_ptr);
 */

#pragma once

#include <quickjs.h>

namespace nanogui { class Button; }

namespace nanogui::js {

/**
 * The prototype JSValue for the Button JS class.
 * Valid after register_button_class() has been called; JS_UNDEFINED before.
 * Its prototype chain runs: Button.prototype → widget_proto.
 */
extern JSValue button_proto;

/**
 * Register the Button JS class into the given QuickJS module.
 *
 * Must be called from the nanogui module's JSModuleInitFunc, after
 * register_widget_class() (so that widget_proto is already live).
 *
 * @param ctx  The JS context that will own all created JS values.
 * @param rt   The JS runtime; needed by JS_NewClassID / JS_NewClass.
 * @param m    The module definition that will export "Button".
 */
void register_button_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m);

/**
 * Wrap an existing nanogui::Button* as a non-owning JS object.
 *
 * The JS object holds a raw pointer; the C++ Button is owned by its parent
 * widget tree and must outlive the JS value.  The onClick callback slot is
 * initialised to JS_UNDEFINED.
 *
 * Returns JS_NULL  if b is nullptr.
 * Returns JS_EXCEPTION if JS object allocation fails.
 */
JSValue wrap_button(JSContext* ctx, nanogui::Button* b);

} // namespace nanogui::js
