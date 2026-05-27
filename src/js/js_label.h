/*
 * js_label.h — QuickJS-NG binding declaration for nanogui::Label.
 *
 * Label is a plain text widget that is owned by its parent Widget tree.
 * The JS binding does NOT own the underlying C++ object; the NanoGUI widget
 * tree manages its lifetime.
 *
 * Usage from other binding translation units:
 *
 *   #include "js_label.h"
 *   JSValue label_obj = nanogui::js::wrap_label(ctx, my_label_ptr);
 */

#pragma once

#include <quickjs.h>

namespace nanogui { class Label; }

namespace nanogui::js {

/**
 * The prototype JSValue for the Label JS class.
 * Valid after register_label_class() has been called; JS_UNDEFINED before.
 * Its prototype chain runs: Label.prototype → widget_proto.
 */
extern JSValue label_proto;

/**
 * Register the Label JS class into the given QuickJS module.
 *
 * Must be called from the nanogui module's JSModuleInitFunc, after
 * register_widget_class() (so that widget_proto is already live).
 *
 * @param ctx  The JS context that will own all created JS values.
 * @param rt   The JS runtime; needed by JS_NewClassID / JS_NewClass.
 * @param m    The module definition that will export "Label".
 */
void register_label_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m);

/**
 * Wrap an existing nanogui::Label* as a non-owning JS object.
 *
 * The JS object holds a raw pointer; the C++ Label is owned by its parent
 * widget tree and must outlive the JS value.
 *
 * Returns JS_NULL  if l is nullptr.
 * Returns JS_EXCEPTION if JS object allocation fails.
 */
JSValue wrap_label(JSContext* ctx, nanogui::Label* l);

} // namespace nanogui::js
