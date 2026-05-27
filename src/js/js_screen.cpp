// js_screen.cpp — QuickJS-NG bindings for nanogui::Screen
//
// Ownership model
// ---------------
//   Screen created via `new Screen(w, h, ...)` from JS:
//     ScreenOpaque::owned = true  → finalizer calls `delete screen`
//
//   Screen wrapped with wrap_screen() for an app-owned pointer:
//     ScreenOpaque::owned = false → finalizer frees the ScreenOpaque struct
//                                   only; the C++ object is untouched

#include <quickjs.h>

#include <nanogui/screen.h>
#include <nanogui/widget.h>
#include <nanogui/common.h>   // nanogui::init, shutdown, mainloop

#include "js_binding_helper.h"
#include "js_widget.h"
#include "js_screen.h"

#include <stdexcept>
#include <string>

#ifndef countof
#  define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

namespace nanogui::js {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

JSValue   screen_proto      = JS_UNDEFINED; // set in register_screen_class
JSClassID js_screen_class_id = 0;           // set in register_screen_class

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline ScreenOpaque* get_screen_opaque(JSContext* ctx,
                                               JSValueConst this_val) {
    return static_cast<ScreenOpaque*>(
        JS_GetOpaque2(ctx, this_val, js_screen_class_id));
}

// ---------------------------------------------------------------------------
// Finalizer
// ---------------------------------------------------------------------------

static void js_screen_finalizer(JSRuntime* rt, JSValueConst val) {
    ScreenOpaque* op = static_cast<ScreenOpaque*>(
        JS_GetOpaque(val, js_screen_class_id));
    if (!op)
        return;
    // Free JS callback values before freeing the struct
    JS_FreeValueRT(rt, op->on_resize);
    if (op->owned)
        delete op->screen;
    js_free_rt(rt, op);
}

static JSClassDef js_screen_class_def = {
    /* class_name = */ "Screen",
    /* finalizer  = */ js_screen_finalizer,
    /* gc_mark    = */ nullptr,
    /* call       = */ nullptr,
    /* exotic     = */ nullptr,
};

// ---------------------------------------------------------------------------
// Constructor:  new Screen(width, height [, caption [, resizable]])
// ---------------------------------------------------------------------------

static JSValue js_screen_ctor(JSContext*    ctx,
                               JSValueConst  new_target,
                               int           argc,
                               JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
                                 "Screen constructor requires at least "
                                 "width and height arguments");

    int32_t w = 0, h = 0;
    if (JS_ToInt32(ctx, &w, argv[0]) < 0)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[1]) < 0)
        return JS_EXCEPTION;

    // Optional caption (argv[2])
    const char* caption_cstr   = nullptr;
    bool        free_caption   = false;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        caption_cstr = JS_ToCString(ctx, argv[2]);
        if (!caption_cstr)
            return JS_EXCEPTION;
        free_caption = true;
    }
    const std::string caption(caption_cstr ? caption_cstr : "NanoJS");
    if (free_caption)
        JS_FreeCString(ctx, caption_cstr);

    // Optional resizable (argv[3])
    bool resizable = true;
    if (argc >= 4 && !JS_IsUndefined(argv[3])) {
        int r = JS_ToBool(ctx, argv[3]);
        if (r < 0)
            return JS_EXCEPTION;
        resizable = (r != 0);
    }

    // Create the C++ Screen — may throw on GL/GLFW errors
    nanogui::Screen* screen = nullptr;
    try {
        screen = new nanogui::Screen(nanogui::Vector2i(w, h),
                                     caption,
                                     resizable);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx,
                                     "Screen creation failed: %s", e.what());
    } catch (...) {
        return JS_ThrowInternalError(ctx, "Screen creation failed");
    }

    // Retrieve the prototype from new.target so subclassing works
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        delete screen;
        return JS_EXCEPTION;
    }

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_screen_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        delete screen;
        return JS_EXCEPTION;
    }

    // Allocate the opaque wrapper
    auto* op = static_cast<ScreenOpaque*>(
        js_mallocz(ctx, sizeof(ScreenOpaque)));
    if (!op) {
        delete screen;
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->screen    = screen;
    op->owned     = true;
    op->ctx       = ctx;
    op->on_resize = JS_UNDEFINED;
    JS_SetOpaque(obj, op);
    return obj;
}

// ---------------------------------------------------------------------------
// Property: caption  (read-write, string)
// ---------------------------------------------------------------------------

static JSValue js_screen_get_caption(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    return JS_NewString(ctx, op->screen->caption().c_str());
}

static JSValue js_screen_set_caption(JSContext*   ctx,
                                      JSValueConst this_val,
                                      JSValueConst val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    op->screen->set_caption(str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: background  (read-write, JS array [r, g, b, a] as floats 0..1)
// ---------------------------------------------------------------------------

static JSValue js_screen_get_background(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    const nanogui::Color& c = op->screen->background();
    // Return as a 4-element JS array [r, g, b, a]
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, c.r()));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, c.g()));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, c.b()));
    JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, c.a()));
    return arr;
}

static JSValue js_screen_set_background(JSContext*   ctx,
                                         JSValueConst this_val,
                                         JSValueConst val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    // Accept a 4-element JS array [r, g, b, a] with values in 0..1
    double rgba[4] = {0.0, 0.0, 0.0, 1.0};
    for (uint32_t i = 0; i < 4; ++i) {
        JSValue el = JS_GetPropertyUint32(ctx, val, i);
        if (JS_IsException(el))
            return JS_EXCEPTION;
        int ok = JS_ToFloat64(ctx, &rgba[i], el);
        JS_FreeValue(ctx, el);
        if (ok < 0)
            return JS_ThrowTypeError(ctx,
                "background must be an [r, g, b, a] array of numbers");
    }
    op->screen->set_background(
        nanogui::Color(static_cast<float>(rgba[0]),
                       static_cast<float>(rgba[1]),
                       static_cast<float>(rgba[2]),
                       static_cast<float>(rgba[3])));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Property: pixel_ratio  (read-only, float)
// ---------------------------------------------------------------------------

static JSValue js_screen_get_pixel_ratio(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, static_cast<double>(op->screen->pixel_ratio()));
}

// ---------------------------------------------------------------------------
// Property: framebuffer_size  (read-only, [w, h])
// ---------------------------------------------------------------------------

static JSValue js_screen_get_framebuffer_size(JSContext*   ctx,
                                               JSValueConst this_val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    return vec2i_to_js(ctx, op->screen->framebuffer_size());
}

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

static JSValue js_screen_redraw(JSContext*    ctx,
                                 JSValueConst  this_val,
                                 int           /*argc*/,
                                 JSValueConst* /*argv*/) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    op->screen->redraw();
    return JS_UNDEFINED;
}

static JSValue js_screen_draw_all(JSContext*    ctx,
                                   JSValueConst  this_val,
                                   int           /*argc*/,
                                   JSValueConst* /*argv*/) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    op->screen->draw_all();
    return JS_UNDEFINED;
}

static JSValue js_screen_perform_layout(JSContext*    ctx,
                                         JSValueConst  this_val,
                                         int           /*argc*/,
                                         JSValueConst* /*argv*/) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    op->screen->perform_layout();
    return JS_UNDEFINED;
}

// set_visible on Screen is NOT virtual in Widget, so we need a Screen-specific
// override that calls glfwShowWindow / glfwHideWindow via Screen::set_visible.
static JSValue js_screen_set_visible(JSContext*   ctx,
                                      JSValueConst this_val,
                                      JSValueConst val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    int v = JS_ToBool(ctx, val);
    if (v < 0) return JS_EXCEPTION;
    op->screen->set_visible(v != 0);   // calls glfwShowWindow/HideWindow
    return JS_UNDEFINED;
}

static JSValue js_screen_get_visible(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    return JS_NewBool(ctx, op->screen->visible());
}

// ---------------------------------------------------------------------------
// Property: onResize  (read-write, function | null)
// ---------------------------------------------------------------------------
// getter — returns the currently registered callback (or undefined)
static JSValue js_screen_get_on_resize(JSContext* ctx, JSValueConst this_val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;
    return JS_DupValue(ctx, op->on_resize);
}

// setter — stores the callback and wires it to set_resize_callback
static JSValue js_screen_set_on_resize(JSContext*   ctx,
                                        JSValueConst this_val,
                                        JSValueConst val) {
    auto* op = get_screen_opaque(ctx, this_val);
    if (!op) return JS_EXCEPTION;

    // Release previous callback
    JS_FreeValue(ctx, op->on_resize);

    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        op->on_resize = JS_UNDEFINED;
        op->screen->set_resize_callback(nullptr);
        return JS_UNDEFINED;
    }

    if (!JS_IsFunction(ctx, val))
        return JS_ThrowTypeError(ctx, "onResize must be a function or null");

    op->on_resize = JS_DupValue(ctx, val);
    op->ctx       = ctx;

    // Wire the C++ callback; capture op by pointer (stable for Screen lifetime)
    op->screen->set_resize_callback([op](nanogui::Vector2i sz) {
        if (JS_IsUndefined(op->on_resize)) return;
        JSValue args[2] = {
            JS_NewInt32(op->ctx, sz.x()),
            JS_NewInt32(op->ctx, sz.y())
        };
        JSValue ret = JS_Call(op->ctx, op->on_resize, JS_UNDEFINED, 2, args);
        JS_FreeValue(op->ctx, args[0]);
        JS_FreeValue(op->ctx, args[1]);
        if (JS_IsException(ret))
            JS_GetException(op->ctx);   // clear, avoid stale exception
        else
            JS_FreeValue(op->ctx, ret);
    });
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Module-level free functions: init / mainloop / shutdown
// ---------------------------------------------------------------------------

JSValue js_nanogui_init(JSContext* ctx, JSValueConst,
                         int /*argc*/, JSValueConst* /*argv*/) {
    try {
        nanogui::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "nanogui::init() failed: %s", e.what());
    }
    return JS_UNDEFINED;
}

JSValue js_nanogui_shutdown(JSContext*, JSValueConst,
                             int /*argc*/, JSValueConst* /*argv*/) {
    nanogui::shutdown();
    return JS_UNDEFINED;
}

JSValue js_nanogui_mainloop(JSContext* ctx, JSValueConst,
                             int argc, JSValueConst* argv) {
    double refresh = -1.0;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &refresh, argv[0]) < 0)
            return JS_EXCEPTION;
    }
    try {
        nanogui::mainloop(static_cast<float>(refresh));
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "nanogui::mainloop() failed: %s", e.what());
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Prototype function list
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_screen_proto_funcs[] = {
    // Override Widget's non-virtual visible so Screen::set_visible() runs
    JS_CGETSET_DEF("visible",
                   js_screen_get_visible,
                   js_screen_set_visible),
    JS_CGETSET_DEF("caption",
                   js_screen_get_caption,
                   js_screen_set_caption),
    JS_CGETSET_DEF("background",
                   js_screen_get_background,
                   js_screen_set_background),
    JS_CGETSET_DEF("pixel_ratio",
                   js_screen_get_pixel_ratio,
                   nullptr),                  // read-only
    JS_CGETSET_DEF("framebuffer_size",
                   js_screen_get_framebuffer_size,
                   nullptr),                  // read-only
    JS_CFUNC_DEF("redraw",          0, js_screen_redraw),
    JS_CFUNC_DEF("draw_all",        0, js_screen_draw_all),
    JS_CFUNC_DEF("perform_layout",  0, js_screen_perform_layout),
    JS_CGETSET_DEF("onResize",
                   js_screen_get_on_resize,
                   js_screen_set_on_resize),
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void register_screen_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m) {
    // Allocate a runtime-unique class ID (idempotent once non-zero)
    JS_NewClassID(rt, &js_screen_class_id);
    JS_NewClass(rt, js_screen_class_id, &js_screen_class_def);

    // Build a prototype that inherits from the Widget prototype, so all Widget
    // methods are available on Screen instances from JS.
    JSValue proto = JS_NewObjectProto(ctx, widget_proto);
    JS_SetPropertyFunctionList(ctx, proto,
                               js_screen_proto_funcs,
                               countof(js_screen_proto_funcs));

    // Register the prototype with the class (JS_SetClassProto takes ownership
    // of one reference, so we dup before storing our own copy).
    JS_SetClassProto(ctx, js_screen_class_id, JS_DupValue(ctx, proto));

    // Create the constructor function and link prototype ↔ constructor
    JSValue ctor = JS_NewCFunction2(ctx,
                                    reinterpret_cast<JSCFunction*>(js_screen_ctor),
                                    "Screen",
                                    2,                  // expected arg count
                                    JS_CFUNC_constructor,
                                    0);
    JS_SetConstructor(ctx, ctor, proto);

    // Save our reference to the prototype for use by wrap_screen() and by
    // derived-class builders.
    screen_proto = proto; // transfer ownership; proto no longer separately freed

    // Export under "Screen" through the module definition
    JS_SetModuleExport(ctx, m, "Screen", ctor);
}

JSValue wrap_screen(JSContext* ctx, nanogui::Screen* s) {
    JSValue obj = JS_NewObjectProtoClass(ctx, screen_proto, js_screen_class_id);
    if (JS_IsException(obj))
        return obj;

    auto* op = static_cast<ScreenOpaque*>(js_mallocz(ctx, sizeof(ScreenOpaque)));
    if (!op) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    op->screen    = s;
    op->owned     = false;
    op->ctx       = ctx;
    op->on_resize = JS_UNDEFINED;
    JS_SetOpaque(obj, op);
    return obj;
}

} // namespace nanogui::js
