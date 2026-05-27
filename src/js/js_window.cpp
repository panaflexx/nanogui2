// js_window.cpp — QuickJS-NG bindings for nanogui::Window
//
// Ownership model
// ---------------
// Window objects are created as children of a parent Widget (typically a
// Screen).  Their lifetime is managed by the NanoGUI widget tree; when the
// parent is destroyed it deletes all its children, including Windows.
//
// Therefore the JS object does NOT own the underlying C++ Window:
//   - The finalizer frees no C++ memory; it only cleans up the raw Window*
//     stored in the JS opaque slot.
//   - dispose() and center() delegate directly to the C++ Window methods.
//
// Parent unwrapping
// -----------------
// The JS constructor accepts any Widget or Screen as the first argument.
// Because Screen has a different QuickJS class from Widget (different class
// IDs), we try both class IDs to extract the underlying Widget*:
//
//   1. Try JSClassIDFor<nanogui::Widget>::value  (set by JSClassBuilder<Widget>
//      in js_widget.cpp; assumes the opaque is a raw nanogui::Widget*).
//   2. If that returns null, try js_screen_class_id and cast the ScreenOpaque
//      to its contained nanogui::Screen* (which is-a Widget* in C++).

#include <quickjs.h>

#include <nanogui/window.h>
#include <nanogui/widget.h>
#include <nanogui/screen.h>   // needed for Screen IS-A Widget upcast in unwrap_parent_widget

#include "js_binding_helper.h"   // JSClassIDFor<T>, js_get_opaque<T>
#include "js_widget.h"            // widget_proto
#include "js_screen.h"            // js_screen_class_id, ScreenOpaque
#include "js_window.h"

#include <stdexcept>
#include <string>

#ifndef countof
#  define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

namespace nanogui::js {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

JSValue   window_proto      = JS_UNDEFINED; // set in register_window_class
static JSClassID js_window_class_id = 0;    // private to this file

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Retrieve the WindowOpaque from a JS Window object.
static inline WindowOpaque* get_window_opaque(JSContext*   ctx,
                                               JSValueConst this_val) {
    return static_cast<WindowOpaque*>(
        JS_GetOpaque2(ctx, this_val, js_window_class_id));
}

// Convenience: get the raw Window* (returns nullptr + exception on failure).
static inline nanogui::Window* get_window_ptr(JSContext*   ctx,
                                               JSValueConst this_val) {
    auto* op = get_window_opaque(ctx, this_val);
    return op ? op->window : nullptr;
}

// Extract a Widget* from a JS value that is either a Widget or a Screen.
// Returns nullptr (with a pending JS TypeError) if neither class matches.
static nanogui::Widget* unwrap_parent_widget(JSContext*   ctx,
                                              JSValueConst parent_js) {
    // 1. Try plain Widget (opaque is stored as nanogui::Widget* directly by
    //    JSClassBuilder<nanogui::Widget> in js_widget.cpp).
    auto* w = static_cast<nanogui::Widget*>(
        JS_GetOpaque(parent_js,
                     JSClassIDFor<nanogui::Widget>::value));
    if (w)
        return w;

    // 2. Try Screen — the opaque is a ScreenOpaque* whose `.screen` member
    //    is a nanogui::Screen*, which is-a nanogui::Widget*.
    auto* sop = static_cast<ScreenOpaque*>(
        JS_GetOpaque(parent_js, js_screen_class_id));
    if (sop)
        return sop->screen;   // Screen IS-A Widget

    // Nothing matched — raise a descriptive TypeError
    JS_ThrowTypeError(ctx, "Window parent must be a Widget or Screen instance");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

static void js_window_finalizer(JSRuntime* rt, JSValueConst val) {
    auto* op = static_cast<WindowOpaque*>(JS_GetOpaque(val, js_window_class_id));
    if (!op) return;
    // Free JS callbacks
    JS_FreeValueRT(rt, op->on_dispose);
    // Window is tree-owned — do NOT delete op->window
    js_free_rt(rt, op);
}

static JSClassDef js_window_class_def = {
    /* class_name = */ "Window",
    /* finalizer  = */ js_window_finalizer,
    /* gc_mark    = */ nullptr,
    /* call       = */ nullptr,
    /* exotic     = */ nullptr,
};

// ---------------------------------------------------------------------------
// Constructor:  new Window(parent [, title [, resizable]])
// ---------------------------------------------------------------------------

static JSValue js_window_ctor(JSContext*    ctx,
                               JSValueConst  new_target,
                               int           argc,
                               JSValueConst* argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
                                 "Window constructor requires a parent "
                                 "Widget or Screen argument");

    // Unwrap parent
    nanogui::Widget* parent = unwrap_parent_widget(ctx, argv[0]);
    if (!parent)
        return JS_EXCEPTION; // TypeError already set

    // Optional title (argv[1])
    const char* title_cstr = nullptr;
    bool        free_title = false;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        title_cstr = JS_ToCString(ctx, argv[1]);
        if (!title_cstr)
            return JS_EXCEPTION;
        free_title = true;
    }
    const std::string title(title_cstr ? title_cstr : "Untitled");
    if (free_title)
        JS_FreeCString(ctx, title_cstr);

    // Optional resizable (argv[2])
    bool resizable = false;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) {
        int r = JS_ToBool(ctx, argv[2]);
        if (r < 0)
            return JS_EXCEPTION;
        resizable = (r != 0);
    }

    // Create C++ Window — owned by the parent tree from this point on
    nanogui::Window* window = nullptr;
    try {
        window = new nanogui::Window(parent, title, resizable);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx,
                                     "Window creation failed: %s", e.what());
    } catch (...) {
        return JS_ThrowInternalError(ctx, "Window creation failed");
    }

    // Retrieve prototype from new.target for correct subclassing
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        return JS_EXCEPTION; // window is now owned by parent; no leak

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_window_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return JS_EXCEPTION;

    auto* op = static_cast<WindowOpaque*>(js_mallocz(ctx, sizeof(WindowOpaque)));
    if (!op) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->window     = window;
    op->ctx        = ctx;
    op->on_dispose = JS_UNDEFINED;
    JS_SetOpaque(obj, op);
    return obj;
}

// ---------------------------------------------------------------------------
// Property: title  (read-write, string)
// ---------------------------------------------------------------------------

static JSValue js_window_get_title(JSContext* ctx, JSValueConst this_val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    return JS_NewString(ctx, w->title().c_str());
}

static JSValue js_window_set_title(JSContext*   ctx,
                                    JSValueConst this_val,
                                    JSValueConst val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    w->set_title(str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: modal  (read-write, bool)
// ---------------------------------------------------------------------------

static JSValue js_window_get_modal(JSContext* ctx, JSValueConst this_val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    return JS_NewBool(ctx, w->modal());
}

static JSValue js_window_set_modal(JSContext*   ctx,
                                    JSValueConst this_val,
                                    JSValueConst val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    int v = JS_ToBool(ctx, val);
    if (v < 0) return JS_EXCEPTION;
    w->set_modal(v != 0);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: resizable  (read-write, bool)
// ---------------------------------------------------------------------------

static JSValue js_window_get_resizable(JSContext* ctx, JSValueConst this_val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    return JS_NewBool(ctx, w->resizable());
}

static JSValue js_window_set_resizable(JSContext*   ctx,
                                        JSValueConst this_val,
                                        JSValueConst val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    int v = JS_ToBool(ctx, val);
    if (v < 0) return JS_EXCEPTION;
    w->set_resizable(v != 0);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: can_move  (read-write, bool)
// ---------------------------------------------------------------------------

static JSValue js_window_get_can_move(JSContext* ctx, JSValueConst this_val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    return JS_NewBool(ctx, w->can_move());
}

static JSValue js_window_set_can_move(JSContext*   ctx,
                                       JSValueConst this_val,
                                       JSValueConst val) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    int v = JS_ToBool(ctx, val);
    if (v < 0) return JS_EXCEPTION;
    w->set_can_move(v != 0);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

static JSValue js_window_center(JSContext*    ctx,
                                 JSValueConst  this_val,
                                 int           /*argc*/,
                                 JSValueConst* /*argv*/) {
    auto* w = get_window_ptr(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    w->center();
    return JS_UNDEFINED;
}

static JSValue js_window_dispose(JSContext*    ctx,
                                  JSValueConst  this_val,
                                  int           /*argc*/,
                                  JSValueConst* /*argv*/) {
    auto* op = get_window_opaque(ctx, this_val);
    if (!op || !op->window) return JS_EXCEPTION;

    // Fire onDispose callback before the C++ object is destroyed
    if (!JS_IsUndefined(op->on_dispose)) {
        JSValue ret = JS_Call(ctx, op->on_dispose, this_val, 0, nullptr);
        if (JS_IsException(ret))
            JS_GetException(ctx);   // clear; can't propagate from dispose()
        else
            JS_FreeValue(ctx, ret);
    }

    op->window->dispose();
    op->window = nullptr;   // stale pointer guard
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: onDispose  (read-write, function | null)
// ---------------------------------------------------------------------------

static JSValue js_window_get_on_dispose(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_window_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    return JS_DupValue(ctx, op->on_dispose);
}

static JSValue js_window_set_on_dispose(JSContext*   ctx,
                                         JSValueConst this_val,
                                         JSValueConst val) {
    auto* op = get_window_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;

    JS_FreeValue(ctx, op->on_dispose);

    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        op->on_dispose = JS_UNDEFINED;
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(ctx, val))
        return JS_ThrowTypeError(ctx, "onDispose must be a function or null");

    op->on_dispose = JS_DupValue(ctx, val);
    op->ctx        = ctx;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Prototype function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_window_proto_funcs[] = {
    JS_CGETSET_DEF("title",
                   js_window_get_title,
                   js_window_set_title),
    JS_CGETSET_DEF("modal",
                   js_window_get_modal,
                   js_window_set_modal),
    JS_CGETSET_DEF("resizable",
                   js_window_get_resizable,
                   js_window_set_resizable),
    JS_CGETSET_DEF("can_move",
                   js_window_get_can_move,
                   js_window_set_can_move),
    JS_CFUNC_DEF("center",  0, js_window_center),
    JS_CFUNC_DEF("dispose", 0, js_window_dispose),
    JS_CGETSET_DEF("onDispose",
                   js_window_get_on_dispose,
                   js_window_set_on_dispose),
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void register_window_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m) {
    JS_NewClassID(rt, &js_window_class_id);
    JS_NewClass(rt, js_window_class_id, &js_window_class_def);

    // Prototype inherits from Widget prototype
    JSValue proto = JS_NewObjectProto(ctx, widget_proto);
    JS_SetPropertyFunctionList(ctx, proto,
                               js_window_proto_funcs,
                               countof(js_window_proto_funcs));

    JS_SetClassProto(ctx, js_window_class_id, JS_DupValue(ctx, proto));

    JSValue ctor = JS_NewCFunction2(ctx,
                                    reinterpret_cast<JSCFunction*>(js_window_ctor),
                                    "Window",
                                    1,                   // expected arg count
                                    JS_CFUNC_constructor,
                                    0);
    JS_SetConstructor(ctx, ctor, proto);

    window_proto = proto; // transfer ownership

    JS_SetModuleExport(ctx, m, "Window", ctor);
}

JSValue wrap_window(JSContext* ctx, nanogui::Window* w) {
    JSValue obj = JS_NewObjectProtoClass(ctx, window_proto, js_window_class_id);
    if (JS_IsException(obj))
        return obj;
    auto* op = static_cast<WindowOpaque*>(js_mallocz(ctx, sizeof(WindowOpaque)));
    if (!op) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->window     = w;
    op->ctx        = ctx;
    op->on_dispose = JS_UNDEFINED;
    JS_SetOpaque(obj, op);
    return obj;
}

} // namespace nanogui::js
