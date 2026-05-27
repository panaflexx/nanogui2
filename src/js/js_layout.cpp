/*
 * js_layout.cpp — QuickJS-NG bindings for nanogui layout classes.
 *
 * Exposed JS surface
 * ------------------
 * new BoxLayout(orientation [, alignment [, margin [, spacing]]])
 *
 * Properties (getset):
 *   orientation  rw  — int (Orientation enum value)
 *   alignment    rw  — int (Alignment enum value)
 *   margin       rw  — int
 *   spacing      rw  — int
 *
 * Enum objects (plain JS objects, not classes):
 *   Orientation  — { Horizontal: 0, Vertical: 1 }
 *   Alignment    — { Minimum: 0, Middle: 1, Maximum: 2, Fill: 3 }
 *
 * Memory model
 * ------------
 * The JS object stores a raw nanogui::BoxLayout* as its opaque.
 *   - Constructor: inc_ref() — the JS object claims one reference.
 *   - Finalizer:   dec_ref() — releases the JS object's reference.
 * When the widget tree holds the layout via ref<Layout> the C++ object
 * remains alive even after the JS wrapper is GC-d.
 */

#include <quickjs.h>
#include <nanogui/layout.h>

#include "js_layout.h"
#include "js_binding_helper.h"

#include <stdexcept>

#ifndef countof
#  define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

namespace nanogui::js {

// ---------------------------------------------------------------------------
// Module-level state (definitions for the externs declared in js_layout.h)
// ---------------------------------------------------------------------------

JSClassID js_box_layout_class_id = 0;
JSValue   box_layout_proto       = JS_UNDEFINED;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Retrieve the BoxLayout* opaque, or nullptr + pending exception on failure.
static inline nanogui::BoxLayout* get_box_layout(JSContext*   ctx,
                                                   JSValueConst this_val) {
    auto* bl = static_cast<nanogui::BoxLayout*>(
        JS_GetOpaque2(ctx, this_val, js_box_layout_class_id));
    return bl;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

static void js_box_layout_finalizer(JSRuntime* /*rt*/, JSValueConst val) {
    auto* layout = static_cast<nanogui::BoxLayout*>(
        JS_GetOpaque(val, js_box_layout_class_id));
    if (layout)
        layout->dec_ref();
}

static JSClassDef js_box_layout_class_def = {
    /* class_name = */ "BoxLayout",
    /* finalizer  = */ js_box_layout_finalizer,
    /* gc_mark    = */ nullptr,
    /* call       = */ nullptr,
    /* exotic     = */ nullptr,
};

// ---------------------------------------------------------------------------
// Constructor:  new BoxLayout(orientation [, alignment [, margin [, spacing]]])
// ---------------------------------------------------------------------------

static JSValue js_box_layout_ctor(JSContext*    ctx,
                                   JSValueConst  new_target,
                                   int           argc,
                                   JSValueConst* argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "BoxLayout constructor requires at least an orientation argument");

    // orientation (required)
    int32_t orientation_int = 0;
    if (JS_ToInt32(ctx, &orientation_int, argv[0]) < 0)
        return JS_EXCEPTION;
    auto orientation = static_cast<nanogui::Orientation>(orientation_int);

    // alignment (optional, default: Middle)
    auto alignment = nanogui::Alignment::Middle;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        int32_t v = 0;
        if (JS_ToInt32(ctx, &v, argv[1]) < 0)
            return JS_EXCEPTION;
        alignment = static_cast<nanogui::Alignment>(v);
    }

    // margin (optional, default: 0)
    int32_t margin = 0;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32(ctx, &margin, argv[2]) < 0)
            return JS_EXCEPTION;
    }

    // spacing (optional, default: 0)
    int32_t spacing = 0;
    if (argc >= 4 && !JS_IsUndefined(argv[3])) {
        if (JS_ToInt32(ctx, &spacing, argv[3]) < 0)
            return JS_EXCEPTION;
    }

    // Create the C++ object and claim one reference for this JS wrapper
    nanogui::BoxLayout* bl = nullptr;
    try {
        bl = new nanogui::BoxLayout(orientation, alignment, margin, spacing);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "BoxLayout creation failed: %s", e.what());
    } catch (...) {
        return JS_ThrowInternalError(ctx, "BoxLayout creation failed");
    }
    bl->inc_ref();

    // Build the JS wrapper using the prototype from new.target (supports subclassing)
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        bl->dec_ref();
        return JS_EXCEPTION;
    }

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_box_layout_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        bl->dec_ref();
        return JS_EXCEPTION;
    }

    JS_SetOpaque(obj, bl);
    return obj;
}

// ---------------------------------------------------------------------------
// Property: orientation  (read-write, int / Orientation enum)
// ---------------------------------------------------------------------------

static JSValue js_box_layout_get_orientation(JSContext* ctx, JSValueConst this_val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    return JS_NewInt32(ctx, static_cast<int32_t>(bl->orientation()));
}

static JSValue js_box_layout_set_orientation(JSContext*   ctx,
                                              JSValueConst this_val,
                                              JSValueConst val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    int32_t v = 0;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    bl->set_orientation(static_cast<nanogui::Orientation>(v));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: alignment  (read-write, int / Alignment enum)
// ---------------------------------------------------------------------------

static JSValue js_box_layout_get_alignment(JSContext* ctx, JSValueConst this_val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    return JS_NewInt32(ctx, static_cast<int32_t>(bl->alignment()));
}

static JSValue js_box_layout_set_alignment(JSContext*   ctx,
                                            JSValueConst this_val,
                                            JSValueConst val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    int32_t v = 0;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    bl->set_alignment(static_cast<nanogui::Alignment>(v));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: margin  (read-write, int)
// ---------------------------------------------------------------------------

static JSValue js_box_layout_get_margin(JSContext* ctx, JSValueConst this_val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    return JS_NewInt32(ctx, bl->margin());
}

static JSValue js_box_layout_set_margin(JSContext*   ctx,
                                         JSValueConst this_val,
                                         JSValueConst val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    int32_t v = 0;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    bl->set_margin(v);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: spacing  (read-write, int)
// ---------------------------------------------------------------------------

static JSValue js_box_layout_get_spacing(JSContext* ctx, JSValueConst this_val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    return JS_NewInt32(ctx, bl->spacing());
}

static JSValue js_box_layout_set_spacing(JSContext*   ctx,
                                          JSValueConst this_val,
                                          JSValueConst val) {
    auto* bl = get_box_layout(ctx, this_val);
    if (!bl) return JS_EXCEPTION;
    int32_t v = 0;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    bl->set_spacing(v);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Prototype function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_box_layout_proto_funcs[] = {
    JS_CGETSET_DEF("orientation",
                   js_box_layout_get_orientation,
                   js_box_layout_set_orientation),
    JS_CGETSET_DEF("alignment",
                   js_box_layout_get_alignment,
                   js_box_layout_set_alignment),
    JS_CGETSET_DEF("margin",
                   js_box_layout_get_margin,
                   js_box_layout_set_margin),
    JS_CGETSET_DEF("spacing",
                   js_box_layout_get_spacing,
                   js_box_layout_set_spacing),
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void register_layout_classes(JSContext* ctx, JSRuntime* rt, JSModuleDef* m) {
    // ------------------------------------------------------------------
    // 1. Allocate a stable class ID and register the class definition.
    // ------------------------------------------------------------------
    JS_NewClassID(rt, &js_box_layout_class_id);
    JS_NewClass(rt, js_box_layout_class_id, &js_box_layout_class_def);

    // ------------------------------------------------------------------
    // 2. Build the prototype and attach properties.
    // ------------------------------------------------------------------
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto,
                               js_box_layout_proto_funcs,
                               countof(js_box_layout_proto_funcs));

    JS_SetClassProto(ctx, js_box_layout_class_id, JS_DupValue(ctx, proto));
    box_layout_proto = proto; // module-lifetime reference; never freed here

    // ------------------------------------------------------------------
    // 3. Create and export the BoxLayout constructor.
    // ------------------------------------------------------------------
    JSValue ctor = JS_NewCFunction2(ctx,
                                    reinterpret_cast<JSCFunction*>(js_box_layout_ctor),
                                    "BoxLayout",
                                    1,
                                    JS_CFUNC_constructor,
                                    0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetModuleExport(ctx, m, "BoxLayout", ctor);

    // ------------------------------------------------------------------
    // 4. Export Orientation plain object: { Horizontal: 0, Vertical: 1 }
    // ------------------------------------------------------------------
    {
        JSValue orientation_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, orientation_obj, "Horizontal",
                          JS_NewInt32(ctx, static_cast<int32_t>(nanogui::Orientation::Horizontal)));
        JS_SetPropertyStr(ctx, orientation_obj, "Vertical",
                          JS_NewInt32(ctx, static_cast<int32_t>(nanogui::Orientation::Vertical)));
        JS_SetModuleExport(ctx, m, "Orientation", orientation_obj);
    }

    // ------------------------------------------------------------------
    // 5. Export Alignment plain object: { Minimum: 0, Middle: 1, Maximum: 2, Fill: 3 }
    // ------------------------------------------------------------------
    {
        JSValue alignment_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, alignment_obj, "Minimum",
                          JS_NewInt32(ctx, static_cast<int32_t>(nanogui::Alignment::Minimum)));
        JS_SetPropertyStr(ctx, alignment_obj, "Middle",
                          JS_NewInt32(ctx, static_cast<int32_t>(nanogui::Alignment::Middle)));
        JS_SetPropertyStr(ctx, alignment_obj, "Maximum",
                          JS_NewInt32(ctx, static_cast<int32_t>(nanogui::Alignment::Maximum)));
        JS_SetPropertyStr(ctx, alignment_obj, "Fill",
                          JS_NewInt32(ctx, static_cast<int32_t>(nanogui::Alignment::Fill)));
        JS_SetModuleExport(ctx, m, "Alignment", alignment_obj);
    }
}

} // namespace nanogui::js
