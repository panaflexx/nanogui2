/*
 * js_layout.h — QuickJS-NG bindings for nanogui layout classes.
 *
 * Currently exposed:
 *   BoxLayout  — nanogui::BoxLayout constructor + orientation/alignment/margin/spacing
 *   Orientation — { Horizontal: 0, Vertical: 1 }
 *   Alignment   — { Minimum: 0, Middle: 1, Maximum: 2, Fill: 3 }
 *
 * Memory model
 * ------------
 * BoxLayout extends nanogui::Object and is reference-counted.  The JS binding
 * holds one ref (inc_ref on construction, dec_ref in the finalizer).  When a
 * Widget stores the layout via set_layout() the widget tree holds an additional
 * ref through ref<Layout>, so the C++ object survives even after the JS value
 * is GC-d.
 */

#pragma once

#include <quickjs.h>

namespace nanogui::js {

/// Prototype object for JS BoxLayout instances.  Set by register_layout_classes().
extern JSValue    box_layout_proto;

/// Runtime-unique class ID for JS BoxLayout objects.  Set by register_layout_classes().
extern JSClassID  js_box_layout_class_id;

/**
 * Register BoxLayout, Orientation, and Alignment with the QuickJS runtime and
 * add them as exports to the "nanogui" module definition @p m.
 *
 * Must be called from the module init callback, after register_widget_class().
 */
void register_layout_classes(JSContext* ctx, JSRuntime* rt, JSModuleDef* m);

} // namespace nanogui::js
