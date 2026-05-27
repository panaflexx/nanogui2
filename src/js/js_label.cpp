// js_label.cpp — QuickJS-NG bindings for nanogui::Label
//
// Ownership model
// ---------------
// Label objects are created as children of a parent Widget and are deleted
// when that parent is destroyed.  The JS object therefore does NOT own the
// underlying C++ Label:
//   - The finalizer zeroes the opaque pointer and frees the LabelOpaque
//     struct, but never calls `delete label`.
//   - wrap_label() provides a non-owning wrapper for existing Label pointers.
//
// Opaque layout
// -------------
// The JS opaque for every Label object is a heap-allocated LabelOpaque whose
// FIRST member is `nanogui::Label* label`.  This layout is intentional: the
// unwrap_widget_from_any() helper casts any non-Screen opaque to Widget** and
// dereferences it to recover the Widget*.  Since Label IS-A Widget (single
// inheritance, no MI offset) this yields the correct pointer for all widget
// types that use this struct-with-first-member convention (Window, Label,
// Button).
//
// Color representation
// --------------------
// nanogui::Color is represented in JS as a four-element array [r, g, b, a]
// where each component is a floating-point number in [0, 1].

#include <quickjs.h>

#include <nanogui/label.h>
#include <nanogui/widget.h>
#include <nanogui/screen.h>   // needed for Screen IS-A Widget upcast in unwrap_widget_from_any

#include "js_binding_helper.h"  // JSClassIDFor<T>, js_dump_error, js_mallocz
#include "js_widget.h"           // widget_proto
#include "js_screen.h"           // js_screen_class_id, ScreenOpaque
#include "js_window.h"           // WindowOpaque (first member = Window*)
#include "js_label.h"

#include <stdexcept>
#include <string>

#ifndef countof
#  define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

namespace nanogui::js {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

JSValue          label_proto       = JS_UNDEFINED;
static JSClassID js_label_class_id = 0;

// ---------------------------------------------------------------------------
// LabelOpaque — heap struct stored as the JS object's opaque pointer
// ---------------------------------------------------------------------------

// IMPORTANT: `label` must remain the first member.  unwrap_widget_from_any()
// casts the opaque void* to Widget** and dereferences it; that read hits the
// first struct member.  Since Label* IS-A Widget* (same address, single
// inheritance) the upcast produces the correct Widget*.
struct LabelOpaque {
    nanogui::Label* label;  // non-owning; NanoGUI widget tree owns the Label
};

// ---------------------------------------------------------------------------
// Color helpers: nanogui::Color ↔ JS [r, g, b, a] array
// ---------------------------------------------------------------------------

static JSValue color_to_js(JSContext* ctx, const nanogui::Color& c) {
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, c.r()));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, c.g()));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, c.b()));
    JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, c.a()));
    return arr;
}

// Returns 0 on success; sets a pending TypeError and returns -1 on failure.
static int js_to_color(JSContext* ctx, JSValueConst val, nanogui::Color& out) {
    JSValue elems[4];
    for (uint32_t i = 0; i < 4; ++i)
        elems[i] = JS_GetPropertyUint32(ctx, val, i);

    double c[4] = { 0.0, 0.0, 0.0, 1.0 };
    int rc = 0;
    for (int i = 0; i < 4; ++i) {
        if (JS_IsException(elems[i]) || JS_ToFloat64(ctx, &c[i], elems[i]) < 0) {
            rc = -1;
            break;
        }
    }
    for (int i = 0; i < 4; ++i)
        JS_FreeValue(ctx, elems[i]);

    if (rc < 0) {
        JS_ThrowTypeError(ctx, "color must be an array [r, g, b, a] of numbers");
        return -1;
    }
    out = nanogui::Color((float)c[0], (float)c[1], (float)c[2], (float)c[3]);
    return 0;
}

// ---------------------------------------------------------------------------
// unwrap_widget_from_any — extract a Widget* from any widget JS value
// ---------------------------------------------------------------------------

// Handles Widget, Screen, and any other widget type whose opaque is a struct
// with a Widget* subtype pointer as its first member (Window, Label, Button).
static nanogui::Widget* unwrap_widget_from_any(JSContext*   ctx,
                                                JSValueConst val) {
    // 1. Plain Widget — opaque is a raw nanogui::Widget* (set by js_widget.cpp).
    void* ptr = JS_GetOpaque(val, JSClassIDFor<nanogui::Widget>::value);
    if (ptr) return static_cast<nanogui::Widget*>(ptr);

    // 2. Screen — opaque is ScreenOpaque*; the .screen member is-a Widget*.
    ptr = JS_GetOpaque(val, js_screen_class_id);
    if (ptr) return static_cast<ScreenOpaque*>(ptr)->screen;

    // 3. Window, Label, Button (and future widget subclasses) — opaque is a
    //    struct whose first member is a Widget* subtype.  Reading the first
    //    pointer-sized word as Widget* is safe because:
    //      - All three classes use single inheritance from Widget (no MI).
    //      - Widget* and Label*/Window*/Button* share the same address.
    JSClassID any_id = 0;
    ptr = JS_GetAnyOpaque(val, &any_id);
    if (ptr) return *static_cast<nanogui::Widget**>(ptr);

    JS_ThrowTypeError(ctx,
        "argument must be a Widget, Screen, Window, Label, or Button");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

static void js_label_finalizer(JSRuntime* rt, JSValueConst val) {
    auto* op = static_cast<LabelOpaque*>(
        JS_GetOpaque(val, js_label_class_id));
    if (!op) return;
    // The C++ Label is tree-owned; do NOT delete it.
    op->label = nullptr;
    js_free_rt(rt, op);
}

static JSClassDef js_label_class_def = {
    /* class_name = */ "Label",
    /* finalizer  = */ js_label_finalizer,
    /* gc_mark    = */ nullptr,
    /* call       = */ nullptr,
    /* exotic     = */ nullptr,
};

// ---------------------------------------------------------------------------
// Constructor:  new Label(parent, caption [, font [, font_size]])
// ---------------------------------------------------------------------------

static JSValue js_label_ctor(JSContext*    ctx,
                              JSValueConst  new_target,
                              int           argc,
                              JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "Label constructor requires at least a parent Widget and a caption");

    // argv[0] — parent widget (required)
    nanogui::Widget* parent = unwrap_widget_from_any(ctx, argv[0]);
    if (!parent) return JS_EXCEPTION;

    // argv[1] — caption string (required)
    const char* cap_cstr = JS_ToCString(ctx, argv[1]);
    if (!cap_cstr) return JS_EXCEPTION;
    std::string caption(cap_cstr);
    JS_FreeCString(ctx, cap_cstr);

    // argv[2] — font string (optional, default "sans")
    std::string font = "sans";
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        const char* f = JS_ToCString(ctx, argv[2]);
        if (!f) return JS_EXCEPTION;
        font = f;
        JS_FreeCString(ctx, f);
    }

    // argv[3] — font_size int (optional, default -1 → theme default)
    int font_size = -1;
    if (argc >= 4 && !JS_IsUndefined(argv[3])) {
        int32_t fs = 0;
        if (JS_ToInt32(ctx, &fs, argv[3]) < 0) return JS_EXCEPTION;
        font_size = fs;
    }

    // Create the C++ Label — owned by the parent tree from this point on.
    nanogui::Label* label = nullptr;
    try {
        label = new nanogui::Label(parent, caption, font, font_size);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "Label creation failed: %s", e.what());
    } catch (...) {
        return JS_ThrowInternalError(ctx, "Label creation failed");
    }

    // Retrieve prototype from new.target to support subclassing.
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) return JS_EXCEPTION;

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_label_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return JS_EXCEPTION;

    auto* op = static_cast<LabelOpaque*>(js_mallocz(ctx, sizeof(LabelOpaque)));
    if (!op) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->label = label;
    JS_SetOpaque(obj, op);
    return obj;
}

// ---------------------------------------------------------------------------
// Internal helper: extract LabelOpaque* and validate
// ---------------------------------------------------------------------------

static inline LabelOpaque* get_label_opaque(JSContext*   ctx,
                                             JSValueConst this_val) {
    return static_cast<LabelOpaque*>(
        JS_GetOpaque2(ctx, this_val, js_label_class_id));
}

// ---------------------------------------------------------------------------
// Property: caption  (read-write, string)
// ---------------------------------------------------------------------------

static JSValue js_label_get_caption(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_label_opaque(ctx, this_val);
    if (!op || !op->label) return JS_EXCEPTION;
    return JS_NewString(ctx, op->label->caption().c_str());
}

static JSValue js_label_set_caption(JSContext*   ctx,
                                     JSValueConst this_val,
                                     JSValueConst val) {
    auto* op = get_label_opaque(ctx, this_val);
    if (!op || !op->label) return JS_EXCEPTION;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    op->label->set_caption(str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: font  (read-write, string)
// ---------------------------------------------------------------------------

static JSValue js_label_get_font(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_label_opaque(ctx, this_val);
    if (!op || !op->label) return JS_EXCEPTION;
    return JS_NewString(ctx, op->label->font().c_str());
}

static JSValue js_label_set_font(JSContext*   ctx,
                                  JSValueConst this_val,
                                  JSValueConst val) {
    auto* op = get_label_opaque(ctx, this_val);
    if (!op || !op->label) return JS_EXCEPTION;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    op->label->set_font(str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: color  (read-write, [r, g, b, a] array)
// ---------------------------------------------------------------------------

static JSValue js_label_get_color(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_label_opaque(ctx, this_val);
    if (!op || !op->label) return JS_EXCEPTION;
    return color_to_js(ctx, op->label->color());
}

static JSValue js_label_set_color(JSContext*   ctx,
                                   JSValueConst this_val,
                                   JSValueConst val) {
    auto* op = get_label_opaque(ctx, this_val);
    if (!op || !op->label) return JS_EXCEPTION;
    nanogui::Color c;
    if (js_to_color(ctx, val, c) < 0) return JS_EXCEPTION;
    op->label->set_color(c);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Prototype function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_label_proto_funcs[] = {
    JS_CGETSET_DEF("caption", js_label_get_caption, js_label_set_caption),
    JS_CGETSET_DEF("font",    js_label_get_font,    js_label_set_font),
    JS_CGETSET_DEF("color",   js_label_get_color,   js_label_set_color),
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void register_label_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m) {
    // Assign a stable, runtime-unique class ID.
    JS_NewClassID(rt, &js_label_class_id);
    JS_NewClass(rt, js_label_class_id, &js_label_class_def);

    // Prototype inherits from Widget prototype so Widget getters/methods are
    // available on Label JS objects.
    JSValue proto = JS_NewObjectProto(ctx, widget_proto);
    JS_SetPropertyFunctionList(ctx, proto,
                               js_label_proto_funcs,
                               countof(js_label_proto_funcs));
    JS_SetClassProto(ctx, js_label_class_id, JS_DupValue(ctx, proto));

    JSValue ctor = JS_NewCFunction2(
        ctx,
        reinterpret_cast<JSCFunction*>(js_label_ctor),
        "Label",
        2,                    // expected argument count (parent + caption)
        JS_CFUNC_constructor,
        0);
    JS_SetConstructor(ctx, ctor, proto);

    label_proto = proto;  // transfer ownership; lives as long as the context
    JS_SetModuleExport(ctx, m, "Label", ctor);
}

JSValue wrap_label(JSContext* ctx, nanogui::Label* l) {
    if (!l) return JS_NULL;

    JSValue obj = JS_NewObjectProtoClass(ctx, label_proto, js_label_class_id);
    if (JS_IsException(obj)) return obj;

    auto* op = static_cast<LabelOpaque*>(js_mallocz(ctx, sizeof(LabelOpaque)));
    if (!op) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->label = l;
    JS_SetOpaque(obj, op);
    return obj;
}

} // namespace nanogui::js
