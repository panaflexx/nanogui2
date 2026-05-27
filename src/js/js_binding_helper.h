/*
 * js_binding_helper.h — Shared utilities for QuickJS-NG ↔ NanoGUI2 bindings.
 *
 * All helpers live in namespace nanogui::js.  Include this header in every
 * *_widget.cpp binding translation unit.
 */

#pragma once

#include <quickjs.h>
#include <nanogui/vector.h>

#include <cstdio>    // fprintf, stderr
#include <cstring>   // strlen
#include <utility>   // std::move

namespace nanogui::js {

// ---------------------------------------------------------------------------
// Per-type static JSClassID storage
// ---------------------------------------------------------------------------

/**
 * JSClassIDFor<T>::value holds the JSClassID that was assigned to the
 * binding for type T.  It is set to 0 until register_*_class() is called.
 *
 * Usage example:
 *   JSClassID id = JSClassIDFor<nanogui::Widget>::value;
 */
template <typename T>
struct JSClassIDFor {
    static JSClassID value;
};

template <typename T>
JSClassID JSClassIDFor<T>::value = 0;

// ---------------------------------------------------------------------------
// Typed opaque retrieval with subclass fallback
// ---------------------------------------------------------------------------

/**
 * Return the opaque pointer of JS object `this_val` as T*, using `class_id`
 * as the expected class.
 *
 * If the exact class ID does not match (e.g. `this_val` is a Window JS object
 * calling a getter that lives on Widget's prototype), we fall back to
 * JS_GetAnyOpaque.  The C++ object stored as opaque is always a subclass of
 * Widget, so the static_cast<T*> is safe when T is Widget (or a base of the
 * actual concrete type).
 *
 * Returns nullptr (and leaves a TypeError pending) on failure.
 */
template <typename T>
inline T* js_get_opaque(JSContext* ctx, JSValueConst this_val, JSClassID class_id) {
    // Fast path: exact class match — opaque is stored as a raw T* directly.
    void* ptr = JS_GetOpaque(this_val, class_id);
    if (ptr)
        return static_cast<T*>(ptr);

    // Slow path: the object is a subclass (Window, Screen, Button, Label …).
    // Their opaques are wrapper STRUCTS whose very first field is the widget
    // pointer (e.g. WindowOpaque::window, ButtonOpaque::button).
    // We must dereference the struct pointer to get that first field.
    //
    // Concretely:
    //   Widget opaque  = raw Widget*   → fast path above (class_id matches)
    //   Window opaque  = WindowOpaque* → slow path, *((Widget**)ptr) = window
    //   Screen opaque  = ScreenOpaque* → slow path, *((Widget**)ptr) = screen
    //   Button opaque  = ButtonOpaque* → slow path, *((Widget**)ptr) = button
    //   Label  opaque  = LabelOpaque*  → slow path, *((Widget**)ptr) = label
    JSClassID actual_id = 0;
    ptr = JS_GetAnyOpaque(this_val, &actual_id);
    if (ptr) {
        if (actual_id == class_id)
            return static_cast<T*>(ptr);          // same class: raw pointer
        else
            return *static_cast<T**>(ptr);         // subclass struct: dereference
    }

    JS_ThrowTypeError(ctx, "invalid receiver: expected a Widget instance");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Vector2i ↔ JS conversion
// ---------------------------------------------------------------------------

/**
 * Convert a nanogui::Vector2i to a plain JS object { x: int, y: int }.
 * The returned JSValue is owned by the caller.
 */
inline JSValue vec2i_to_js(JSContext* ctx, const nanogui::Vector2i& v) {
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_SetPropertyStr(ctx, obj, "x", JS_NewInt32(ctx, v.x()));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewInt32(ctx, v.y()));
    return obj;
}

/**
 * Parse a JS value into a nanogui::Vector2i.
 *
 * Accepts a JS object with numeric "x" and "y" properties.
 * Returns 0 on success, -1 on failure (a TypeError is pending in ctx).
 */
inline int js_to_vec2i(JSContext* ctx, JSValueConst val, nanogui::Vector2i& out) {
    JSValue jx = JS_GetPropertyStr(ctx, val, "x");
    JSValue jy = JS_GetPropertyStr(ctx, val, "y");

    int32_t x = 0, y = 0;
    int rc = 0;

    if (JS_IsException(jx) || JS_IsException(jy)) {
        rc = -1;
    } else if (JS_ToInt32(ctx, &x, jx) < 0 || JS_ToInt32(ctx, &y, jy) < 0) {
        rc = -1;
    } else {
        out = nanogui::Vector2i(x, y);
    }

    JS_FreeValue(ctx, jx);
    JS_FreeValue(ctx, jy);
    return rc;
}

// ---------------------------------------------------------------------------
// Error reporting helper
// ---------------------------------------------------------------------------

/**
 * Print the pending JS exception (including stack trace when available) to
 * stderr, then clear it.  Useful for debugging callbacks that cannot
 * propagate exceptions to JS code.
 */
inline void js_dump_error(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);

    if (!JS_IsNull(exc) && !JS_IsUndefined(exc)) {
        if (JS_IsError(exc)) {
            JSValue msg   = JS_GetPropertyStr(ctx, exc, "message");
            JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");

            const char* msg_str   = JS_ToCString(ctx, msg);
            const char* stack_str = JS_ToCString(ctx, stack);

            fprintf(stderr, "JS Error: %s\n%s\n",
                    msg_str   ? msg_str   : "(no message)",
                    stack_str ? stack_str : "");

            if (msg_str)   JS_FreeCString(ctx, msg_str);
            if (stack_str) JS_FreeCString(ctx, stack_str);

            JS_FreeValue(ctx, msg);
            JS_FreeValue(ctx, stack);
        } else {
            const char* str = JS_ToCString(ctx, exc);
            fprintf(stderr, "JS Exception: %s\n", str ? str : "(unknown)");
            if (str) JS_FreeCString(ctx, str);
        }
    }

    JS_FreeValue(ctx, exc);
}

// ---------------------------------------------------------------------------
// JS callback holder (RAII wrapper around a JSValue function reference)
// ---------------------------------------------------------------------------

/**
 * RAII owner for a JS function value that is called from C++ event handlers.
 *
 * Typical use:
 *   JSCallbackHolder cb(ctx, argv[0]);  // dups the reference
 *   ...
 *   if (cb.valid()) {
 *       JSValue ret = cb.call(0, nullptr);
 *       JS_FreeValue(ctx, ret);
 *   }
 */
struct JSCallbackHolder {
    JSContext* ctx  = nullptr;
    JSValue    func = JS_UNDEFINED;

    JSCallbackHolder() = default;

    JSCallbackHolder(JSContext* c, JSValueConst f)
        : ctx(c), func(JS_DupValue(c, f)) {}

    // Non-copyable
    JSCallbackHolder(const JSCallbackHolder&)            = delete;
    JSCallbackHolder& operator=(const JSCallbackHolder&) = delete;

    JSCallbackHolder(JSCallbackHolder&& o) noexcept
        : ctx(o.ctx), func(o.func) {
        o.ctx  = nullptr;
        o.func = JS_UNDEFINED;
    }

    JSCallbackHolder& operator=(JSCallbackHolder&& o) noexcept {
        if (this != &o) {
            release();
            ctx    = o.ctx;
            func   = o.func;
            o.ctx  = nullptr;
            o.func = JS_UNDEFINED;
        }
        return *this;
    }

    ~JSCallbackHolder() { release(); }

    /// True when a callable JS function is held.
    bool valid() const {
        return ctx != nullptr && JS_IsFunction(ctx, func);
    }

    /**
     * Invoke the held JS function.  argc/argv may be 0/nullptr.
     * Returns an owned JSValue (caller must JS_FreeValue).
     */
    JSValue call(int argc = 0, JSValueConst* argv = nullptr) const {
        if (!valid())
            return JS_UNDEFINED;
        return JS_Call(ctx, func, JS_UNDEFINED, argc, argv);
    }

    void release() {
        if (ctx) {
            JS_FreeValue(ctx, func);
            ctx  = nullptr;
            func = JS_UNDEFINED;
        }
    }
};

} // namespace nanogui::js
