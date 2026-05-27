// js_button.cpp — QuickJS-NG bindings for nanogui::Button
//
// Ownership model
// ---------------
// Button objects are created as children of a parent Widget and are deleted
// when that parent is destroyed.  The JS object therefore does NOT own the
// underlying C++ Button:
//   - The finalizer frees the JS onClick callback value and the ButtonOpaque
//     struct, but never calls `delete button`.
//   - wrap_button() provides a non-owning wrapper for existing Button pointers.
//
// Opaque layout
// -------------
// The JS opaque for every Button object is a heap-allocated ButtonOpaque.
// IMPORTANT: `button` must be the first member so that the Widget** deref
// trick in unwrap_widget_from_any() works correctly.  The same convention is
// used by WindowOpaque (js_window.h) and LabelOpaque (js_label.cpp).
//
// onClick callback lifetime
// -------------------------
// When JS sets `button.onClick = fn`, we:
//   1. JS_FreeValue the previously stored callback (if any).
//   2. JS_DupValue the new function and store it in op->on_click.
//   3. Install a C++ lambda as the NanoGUI callback that calls JS_Call.
//      The lambda captures `op` by raw pointer — this is safe because the
//      ButtonOpaque lives until the JS object is GC-collected, and the NanoGUI
//      callback is cleared (or the Button is destroyed) before that.
//   4. Setting onClick to null/undefined clears both the JS value and the C++
//      callback (via set_callback(nullptr)).
//
// Color representation
// --------------------
// nanogui::Color is represented in JS as a four-element array [r, g, b, a]
// where each component is a floating-point number in [0, 1].

#include <quickjs.h>

#include <nanogui/button.h>
#include <nanogui/widget.h>
#include <nanogui/screen.h>   // needed for Screen IS-A Widget upcast in unwrap_widget_from_any

#include "js_binding_helper.h"  // JSClassIDFor<T>, js_dump_error, js_mallocz
#include "js_widget.h"           // widget_proto
#include "js_screen.h"           // js_screen_class_id, ScreenOpaque
#include "js_window.h"           // WindowOpaque (first member = Window*)
#include "js_button.h"

#include <memory>
#include <stdexcept>
#include <string>

#ifndef countof
#  define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

namespace nanogui::js {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

JSValue          button_proto       = JS_UNDEFINED;
static JSClassID js_button_class_id = 0;

// ---------------------------------------------------------------------------
// ButtonOpaque — heap struct stored as the JS object's opaque pointer
// ---------------------------------------------------------------------------

// IMPORTANT: `button` must remain the first member.  unwrap_widget_from_any()
// casts the opaque void* to Widget** and dereferences it; that read hits the
// first struct member.  Since Button* IS-A Widget* (same address, single
// inheritance) the upcast produces the correct Widget*.
struct ButtonOpaque {
    nanogui::Button* button;     // non-owning; NanoGUI widget tree owns Button
    JSContext*       ctx      = nullptr;
    JSValue          on_click = JS_UNDEFINED;  // stored void() callback
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

static void js_button_finalizer(JSRuntime* rt, JSValueConst val) {
    auto* op = static_cast<ButtonOpaque*>(
        JS_GetOpaque(val, js_button_class_id));
    if (!op) return;
    // Free the stored JS callback value.
    JS_FreeValueRT(rt, op->on_click);
    // The C++ Button is tree-owned; do NOT delete it.
    js_free_rt(rt, op);
}

static JSClassDef js_button_class_def = {
    /* class_name = */ "Button",
    /* finalizer  = */ js_button_finalizer,
    /* gc_mark    = */ nullptr,
    /* call       = */ nullptr,
    /* exotic     = */ nullptr,
};

// ---------------------------------------------------------------------------
// Constructor:  new Button(parent [, caption [, icon]])
// ---------------------------------------------------------------------------

static JSValue js_button_ctor(JSContext*    ctx,
                               JSValueConst  new_target,
                               int           argc,
                               JSValueConst* argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "Button constructor requires at least a parent Widget argument");

    // argv[0] — parent widget (required)
    nanogui::Widget* parent = unwrap_widget_from_any(ctx, argv[0]);
    if (!parent) return JS_EXCEPTION;

    // argv[1] — caption string (optional, default "Button")
    std::string caption = "Button";
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        const char* c = JS_ToCString(ctx, argv[1]);
        if (!c) return JS_EXCEPTION;
        caption = c;
        JS_FreeCString(ctx, c);
    }

    // argv[2] — icon int (optional, default 0 = no icon)
    int icon = 0;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) {
        int32_t ic = 0;
        if (JS_ToInt32(ctx, &ic, argv[2]) < 0) return JS_EXCEPTION;
        icon = ic;
    }

    // Create the C++ Button — owned by the parent tree from this point on.
    nanogui::Button* button = nullptr;
    try {
        button = new nanogui::Button(parent, caption, icon);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "Button creation failed: %s", e.what());
    } catch (...) {
        return JS_ThrowInternalError(ctx, "Button creation failed");
    }

    // Retrieve prototype from new.target to support subclassing.
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) return JS_EXCEPTION;

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_button_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return JS_EXCEPTION;

    auto* op = static_cast<ButtonOpaque*>(js_mallocz(ctx, sizeof(ButtonOpaque)));
    if (!op) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->button   = button;
    op->ctx      = ctx;
    op->on_click = JS_UNDEFINED;
    JS_SetOpaque(obj, op);
    return obj;
}

// ---------------------------------------------------------------------------
// Internal helper: extract ButtonOpaque* and validate
// ---------------------------------------------------------------------------

static inline ButtonOpaque* get_button_opaque(JSContext*   ctx,
                                               JSValueConst this_val) {
    return static_cast<ButtonOpaque*>(
        JS_GetOpaque2(ctx, this_val, js_button_class_id));
}

// ---------------------------------------------------------------------------
// Property: caption  (read-write, string)
// ---------------------------------------------------------------------------

static JSValue js_button_get_caption(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    return JS_NewString(ctx, op->button->caption().c_str());
}

static JSValue js_button_set_caption(JSContext*   ctx,
                                      JSValueConst this_val,
                                      JSValueConst val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    op->button->set_caption(str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: pushed  (read-write, bool)
// ---------------------------------------------------------------------------

static JSValue js_button_get_pushed(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    return JS_NewBool(ctx, op->button->pushed());
}

static JSValue js_button_set_pushed(JSContext*   ctx,
                                     JSValueConst this_val,
                                     JSValueConst val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    int v = JS_ToBool(ctx, val);
    if (v < 0) return JS_EXCEPTION;
    op->button->set_pushed(v != 0);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: background_color  (read-write, [r, g, b, a] array)
// ---------------------------------------------------------------------------

static JSValue js_button_get_background_color(JSContext* ctx,
                                               JSValueConst this_val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    return color_to_js(ctx, op->button->background_color());
}

static JSValue js_button_set_background_color(JSContext*   ctx,
                                               JSValueConst this_val,
                                               JSValueConst val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    nanogui::Color c;
    if (js_to_color(ctx, val, c) < 0) return JS_EXCEPTION;
    op->button->set_background_color(c);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: text_color  (read-write, [r, g, b, a] array)
// ---------------------------------------------------------------------------

static JSValue js_button_get_text_color(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    return color_to_js(ctx, op->button->text_color());
}

static JSValue js_button_set_text_color(JSContext*   ctx,
                                         JSValueConst this_val,
                                         JSValueConst val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;
    nanogui::Color c;
    if (js_to_color(ctx, val, c) < 0) return JS_EXCEPTION;
    op->button->set_text_color(c);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: onClick  (read-write, function | null)
// ---------------------------------------------------------------------------

static JSValue js_button_get_on_click(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    return JS_DupValue(ctx, op->on_click);
}

static JSValue js_button_set_on_click(JSContext*   ctx,
                                       JSValueConst this_val,
                                       JSValueConst val) {
    auto* op = get_button_opaque(ctx, this_val);
    if (!op || !op->button) return JS_EXCEPTION;

    // Release the previously held callback.
    JS_FreeValue(ctx, op->on_click);

    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        op->on_click = JS_UNDEFINED;
        op->button->set_callback(nullptr);
        return JS_UNDEFINED;
    }

    if (!JS_IsFunction(ctx, val))
        return JS_ThrowTypeError(ctx, "onClick must be a function or null");

    op->on_click = JS_DupValue(ctx, val);
    op->ctx      = ctx;

    // Install the C++ callback that bridges into JS.
    // The lambda captures `op` by raw pointer.  `op` is freed only when the
    // JS object is GC-collected, which happens after the Button is removed
    // from the widget tree — so the pointer is always valid during a callback.
    op->button->set_callback([op]() {
        JSValue ret = JS_Call(op->ctx, op->on_click,
                              JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(ret)) {
            // Cannot propagate the exception from a C++ callback; log it.
            js_dump_error(op->ctx);
        } else {
            JS_FreeValue(op->ctx, ret);
        }
    });

    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Prototype function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_button_proto_funcs[] = {
    JS_CGETSET_DEF("caption",
                   js_button_get_caption,
                   js_button_set_caption),
    JS_CGETSET_DEF("pushed",
                   js_button_get_pushed,
                   js_button_set_pushed),
    JS_CGETSET_DEF("background_color",
                   js_button_get_background_color,
                   js_button_set_background_color),
    JS_CGETSET_DEF("text_color",
                   js_button_get_text_color,
                   js_button_set_text_color),
    JS_CGETSET_DEF("onClick",
                   js_button_get_on_click,
                   js_button_set_on_click),
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void register_button_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m) {
    // Assign a stable, runtime-unique class ID.
    JS_NewClassID(rt, &js_button_class_id);
    JS_NewClass(rt, js_button_class_id, &js_button_class_def);

    // Prototype inherits from Widget prototype so Widget getters/methods are
    // available on Button JS objects.
    JSValue proto = JS_NewObjectProto(ctx, widget_proto);
    JS_SetPropertyFunctionList(ctx, proto,
                               js_button_proto_funcs,
                               countof(js_button_proto_funcs));
    JS_SetClassProto(ctx, js_button_class_id, JS_DupValue(ctx, proto));

    JSValue ctor = JS_NewCFunction2(
        ctx,
        reinterpret_cast<JSCFunction*>(js_button_ctor),
        "Button",
        1,                    // expected argument count (parent)
        JS_CFUNC_constructor,
        0);
    JS_SetConstructor(ctx, ctor, proto);

    button_proto = proto;  // transfer ownership; lives as long as the context
    JS_SetModuleExport(ctx, m, "Button", ctor);
}

JSValue wrap_button(JSContext* ctx, nanogui::Button* b) {
    if (!b) return JS_NULL;

    JSValue obj = JS_NewObjectProtoClass(ctx, button_proto, js_button_class_id);
    if (JS_IsException(obj)) return obj;

    auto* op = static_cast<ButtonOpaque*>(js_mallocz(ctx, sizeof(ButtonOpaque)));
    if (!op) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->button   = b;
    op->ctx      = ctx;
    op->on_click = JS_UNDEFINED;
    JS_SetOpaque(obj, op);
    return obj;
}

} // namespace nanogui::js
