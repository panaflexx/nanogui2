/*
 * js_widget.cpp — QuickJS-NG binding implementation for nanogui::Widget.
 *
 * Exposed JS surface
 * ------------------
 * Properties (getset):
 *   x, y             — position components (int), settable
 *   width, height    — size components (int), settable
 *   visible          — bool, settable
 *   enabled          — bool, settable
 *   tooltip          — string, settable
 *   id               — string, settable
 *   font_size        — int, settable
 *   child_count      — int, read-only
 *
 * Methods:
 *   request_focus()                — void
 *   child_at(index)                — Widget JS object or null
 *   remove_child(widgetObj)        — void
 *   perform_layout()               — void (uses widget's Screen for NVGcontext)
 *
 * Memory model
 * ------------
 * NanoGUI owns every Widget instance.  The JS object stores a raw (non-owning)
 * pointer as its opaque.  The finalizer only nulls the opaque; it never deletes
 * the underlying Widget.
 *
 * Subclass support
 * ----------------
 * js_get_opaque<Widget> falls back to JS_GetAnyOpaque so that Widget getters/
 * setters work correctly when called on subclass instances (e.g. a Window JS
 * object that inherits Widget's prototype).
 */

#include "js_widget.h"
#include "js_binding_helper.h"

#include <nanogui/widget.h>
#include <nanogui/screen.h>
#include <nanogui/layout.h>

#include <cstdint>   // int32_t
#include <cstring>   // (indirectly via helper)

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

namespace nanogui::js {

JSValue widget_proto = JS_UNDEFINED;

} // namespace nanogui::js

// Keep the rest in an anonymous namespace to avoid symbol clashes with future
// binding translation units, then pull the public symbols back in at the end.
namespace {

using nanogui::Widget;
using nanogui::Screen;
using nanogui::Vector2i;

// The runtime-assigned class ID for Widget JS objects.
static JSClassID js_widget_class_id = 0;

// ============================================================================
// Finalizer
// ============================================================================

/**
 * Called by the QuickJS GC when a Widget JS object is collected.
 *
 * NanoGUI owns the underlying C++ object, so we must NOT delete it.
 * We null the opaque pointer to guard against any stray JS references that
 * might attempt to use the object after the C++ side frees the Widget.
 */
static void js_widget_finalizer(JSRuntime* /*rt*/, JSValueConst val) {
    JS_SetOpaque(val, nullptr);
}

// ============================================================================
// Constructor (intentionally disabled)
// ============================================================================

/**
 * Widget cannot be instantiated directly from JS — it requires a parent C++
 * object and NanoGUI only creates widgets through its own factory patterns.
 * Subclasses (Window, Button, …) provide their own constructors.
 */
static JSValue js_widget_ctor(JSContext* ctx,
                              JSValueConst /*new_target*/,
                              int /*argc*/,
                              JSValueConst* /*argv*/) {
    return JS_ThrowTypeError(ctx,
        "Widget is a base class; construct Window, Button, etc. instead");
}

// ============================================================================
// Property getters and setters
// ============================================================================

// --- x / y (position components) -------------------------------------------

static JSValue js_widget_get_x(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewInt32(ctx, w->position().x());
}

static JSValue js_widget_set_x(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    w->set_position(Vector2i(v, w->position().y()));
    return JS_UNDEFINED;
}

static JSValue js_widget_get_y(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewInt32(ctx, w->position().y());
}

static JSValue js_widget_set_y(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    w->set_position(Vector2i(w->position().x(), v));
    return JS_UNDEFINED;
}

// --- width / height ---------------------------------------------------------

static JSValue js_widget_get_width(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewInt32(ctx, w->width());
}

static JSValue js_widget_set_width(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    w->set_width(v);
    return JS_UNDEFINED;
}

static JSValue js_widget_get_height(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewInt32(ctx, w->height());
}

static JSValue js_widget_set_height(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    w->set_height(v);
    return JS_UNDEFINED;
}

// --- visible ----------------------------------------------------------------

static JSValue js_widget_get_visible(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewBool(ctx, w->visible());
}

static JSValue js_widget_set_visible(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    int b = JS_ToBool(ctx, val);
    if (b < 0) return JS_EXCEPTION;
    w->set_visible(b != 0);
    return JS_UNDEFINED;
}

// --- enabled ----------------------------------------------------------------

static JSValue js_widget_get_enabled(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewBool(ctx, w->enabled());
}

static JSValue js_widget_set_enabled(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    int b = JS_ToBool(ctx, val);
    if (b < 0) return JS_EXCEPTION;
    w->set_enabled(b != 0);
    return JS_UNDEFINED;
}

// --- tooltip ----------------------------------------------------------------

static JSValue js_widget_get_tooltip(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewString(ctx, w->tooltip().c_str());
}

static JSValue js_widget_set_tooltip(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    w->set_tooltip(str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// --- id ---------------------------------------------------------------------

static JSValue js_widget_get_id(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewString(ctx, w->id().c_str());
}

static JSValue js_widget_set_id(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    const char* str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    w->set_id(str);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// --- font_size --------------------------------------------------------------

static JSValue js_widget_get_font_size(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewInt32(ctx, w->font_size());
}

static JSValue js_widget_set_font_size(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, val) < 0) return JS_EXCEPTION;
    w->set_font_size(v);
    return JS_UNDEFINED;
}

// --- child_count (read-only) ------------------------------------------------

static JSValue js_widget_get_child_count(JSContext* ctx, JSValueConst this_val) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    return JS_NewInt32(ctx, w->child_count());
}

// ============================================================================
// Methods
// ============================================================================

/**
 * widget.request_focus()
 * Requests keyboard focus for this widget.
 */
static JSValue js_widget_request_focus(JSContext* ctx,
                                        JSValueConst this_val,
                                        int /*argc*/,
                                        JSValueConst* /*argv*/) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    w->request_focus();
    return JS_UNDEFINED;
}

/**
 * widget.child_at(index) → Widget | null
 * Returns a wrapped JS Widget for the child at the given index.
 */
static JSValue js_widget_child_at(JSContext* ctx,
                                   JSValueConst this_val,
                                   int argc,
                                   JSValueConst* argv) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Widget.child_at: expected an index argument");

    int32_t idx;
    if (JS_ToInt32(ctx, &idx, argv[0]) < 0)
        return JS_EXCEPTION;

    int count = w->child_count();
    if (idx < 0 || idx >= count)
        return JS_ThrowRangeError(ctx,
            "Widget.child_at: index %d is out of range [0, %d)", idx, count);

    Widget* child = w->child_at(idx);
    if (!child)
        return JS_NULL;

    return nanogui::js::wrap_widget(ctx, child);
}

/**
 * widget.remove_child(childObj) → undefined
 * Unwraps the JS Widget object and removes it from this widget's children.
 * The child JS object's opaque must be non-null (i.e. the C++ widget must
 * still be alive at the time of this call).
 */
static JSValue js_widget_remove_child(JSContext* ctx,
                                       JSValueConst this_val,
                                       int argc,
                                       JSValueConst* argv) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Widget.remove_child: expected a widget argument");

    // Extract Widget* from the child JS object.
    // Widget opaques are raw Widget*; all subclass opaques (Window, Button …)
    // are wrapper structs whose first field is the widget pointer — use
    // js_get_opaque which handles the struct-dereference correctly.
    auto* child = nanogui::js::js_get_opaque<Widget>(ctx, argv[0], js_widget_class_id);
    if (!child)
        return JS_EXCEPTION; // TypeError already set by js_get_opaque

    w->remove_child(child);
    return JS_UNDEFINED;
}

/**
 * widget.perform_layout() → undefined
 * Triggers a layout pass on this widget's subtree using the NVGcontext
 * obtained from its parent Screen.
 */
static JSValue js_widget_perform_layout(JSContext* ctx,
                                         JSValueConst this_val,
                                         int /*argc*/,
                                         JSValueConst* /*argv*/) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;

    Screen* s = w->screen();
    if (!s)
        return JS_ThrowInternalError(ctx,
            "Widget.perform_layout: widget is not attached to a Screen");

    w->perform_layout(s->nvg_context());
    return JS_UNDEFINED;
}

/**
 * widget.set_layout(layoutObj | null | undefined) → undefined
 *
 * Assigns a layout manager to this widget.  Accepts any JS object whose
 * opaque pointer is a nanogui::Layout subclass (e.g. a BoxLayout JS object).
 * Pass null or undefined to clear the current layout.
 */
static JSValue js_widget_set_layout(JSContext*    ctx,
                                     JSValueConst  this_val,
                                     int           argc,
                                     JSValueConst* argv) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "set_layout requires a layout argument");

    // Accept null/undefined to clear the layout
    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        w->set_layout(nullptr);
        return JS_UNDEFINED;
    }

    // Extract Layout* from the opaque — use JS_GetAnyOpaque since we don't
    // want to hard-code the BoxLayout class ID here (future layout types work too).
    JSClassID actual_id = 0;
    void* ptr = JS_GetAnyOpaque(argv[0], &actual_id);
    if (!ptr)
        return JS_ThrowTypeError(ctx,
            "set_layout argument must be a Layout instance (e.g. BoxLayout)");

    w->set_layout(static_cast<nanogui::Layout*>(ptr));
    return JS_UNDEFINED;
}

// ============================================================================
// Helper: define a getset property on an object without designated inits
// ============================================================================

/**
 * Register a JS getter/setter pair on `proto` for the property `name`.
 *
 * We build the JSCFunctionType union explicitly to avoid C++ restrictions on
 * nested designated initializers (the JS_CGETSET_DEF macro uses C99
 * designated-init syntax that isn't portable to C++17).
 *
 * `setter` may be nullptr to create a read-only property (the setter slot is
 * passed as JS_UNDEFINED to JS_DefinePropertyGetSet).
 */
static void def_getset(
    JSContext*  ctx,
    JSValue     proto,
    const char* name,
    JSValue (*getter)(JSContext*, JSValueConst),
    JSValue (*setter)(JSContext*, JSValueConst, JSValueConst))
{
    JSAtom atom = JS_NewAtom(ctx, name);

    // Build the getter JSValue using the JSCFunctionType union to safely
    // reinterpret the narrower function pointer as JSCFunction*.
    JSCFunctionType gft;
    gft.getter = getter;
    JSValue g = JS_NewCFunction2(ctx, gft.generic, name, 0, JS_CFUNC_getter, 0);

    JSValue s;
    if (setter) {
        JSCFunctionType sft;
        sft.setter = setter;
        s = JS_NewCFunction2(ctx, sft.generic, name, 1, JS_CFUNC_setter, 0);
    } else {
        s = JS_UNDEFINED;
    }

    // JS_DefinePropertyGetSet takes ownership of g and s.
    JS_DefinePropertyGetSet(ctx, proto, atom, g, s,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

// ---------------------------------------------------------------------------
// Vector-argument helpers: set_size, set_fixed_size, set_position
// ---------------------------------------------------------------------------
// All three follow the same pattern: accept (width, height) or (x, y) as
// two separate integer arguments — mirrors the most common C++ call site.

static JSValue js_widget_set_size(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "set_size(width, height) requires 2 arguments");
    int32_t width = 0, height = 0;
    if (JS_ToInt32(ctx, &width,  argv[0]) < 0) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &height, argv[1]) < 0) return JS_EXCEPTION;
    w->set_size(Vector2i(width, height));
    return JS_UNDEFINED;
}

static JSValue js_widget_set_fixed_size(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "set_fixed_size(width, height) requires 2 arguments");
    int32_t width = 0, height = 0;
    if (JS_ToInt32(ctx, &width,  argv[0]) < 0) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &height, argv[1]) < 0) return JS_EXCEPTION;
    w->set_fixed_size(Vector2i(width, height));
    return JS_UNDEFINED;
}

static JSValue js_widget_set_min_size(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "set_min_size(width, height) requires 2 arguments");
    int32_t width = 0, height = 0;
    if (JS_ToInt32(ctx, &width,  argv[0]) < 0) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &height, argv[1]) < 0) return JS_EXCEPTION;
    w->set_min_size(Vector2i(width, height));
    return JS_UNDEFINED;
}

static JSValue js_widget_set_position(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* w = nanogui::js::js_get_opaque<Widget>(ctx, this_val, js_widget_class_id);
    if (!w) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "set_position(x, y) requires 2 arguments");
    int32_t x = 0, y = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) < 0) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1]) < 0) return JS_EXCEPTION;
    w->set_position(Vector2i(x, y));
    return JS_UNDEFINED;
}

} // anonymous namespace

// ============================================================================
// Public API — nanogui::js
// ============================================================================

namespace nanogui::js {

JSValue wrap_widget(JSContext* ctx, nanogui::Widget* w) {
    if (!w)
        return JS_NULL;

    JSValue obj = JS_NewObjectProtoClass(ctx, widget_proto, js_widget_class_id);
    if (JS_IsException(obj))
        return obj;

    JS_SetOpaque(obj, w);
    return obj;
}

void register_widget_class(JSContext* ctx, JSRuntime* rt, JSModuleDef* m) {
    // ------------------------------------------------------------------
    // 1. Allocate a stable, runtime-unique class ID for Widget.
    // ------------------------------------------------------------------
    JS_NewClassID(rt, &js_widget_class_id);
    JSClassIDFor<Widget>::value = js_widget_class_id;

    // ------------------------------------------------------------------
    // 2. Register the class definition with the runtime.
    //    JSClassDef is a plain aggregate; we use positional initialisation
    //    to stay C++17-compatible (no designated initializers needed here
    //    because the struct fields are in declaration order).
    // ------------------------------------------------------------------
    static const JSClassDef widget_class_def = {
        /* class_name = */ "Widget",
        /* finalizer  = */ js_widget_finalizer,
        /* gc_mark    = */ nullptr,
        /* call       = */ nullptr,
        /* exotic     = */ nullptr,
    };
    JS_NewClass(rt, js_widget_class_id, &widget_class_def);

    // ------------------------------------------------------------------
    // 3. Build the prototype object.
    // ------------------------------------------------------------------
    JSValue proto = JS_NewObject(ctx);

    // Getset properties — mirrors the C++ Widget API
    def_getset(ctx, proto, "x",           js_widget_get_x,           js_widget_set_x);
    def_getset(ctx, proto, "y",           js_widget_get_y,           js_widget_set_y);
    def_getset(ctx, proto, "width",       js_widget_get_width,       js_widget_set_width);
    def_getset(ctx, proto, "height",      js_widget_get_height,      js_widget_set_height);
    def_getset(ctx, proto, "visible",     js_widget_get_visible,     js_widget_set_visible);
    def_getset(ctx, proto, "enabled",     js_widget_get_enabled,     js_widget_set_enabled);
    def_getset(ctx, proto, "tooltip",     js_widget_get_tooltip,     js_widget_set_tooltip);
    def_getset(ctx, proto, "id",          js_widget_get_id,          js_widget_set_id);
    def_getset(ctx, proto, "font_size",   js_widget_get_font_size,   js_widget_set_font_size);
    def_getset(ctx, proto, "child_count", js_widget_get_child_count, nullptr); // read-only

    // Methods — JS_SetPropertyStr takes ownership of the JSValue
    JS_SetPropertyStr(ctx, proto, "request_focus",
        JS_NewCFunction(ctx, js_widget_request_focus,  "request_focus",  0));
    JS_SetPropertyStr(ctx, proto, "child_at",
        JS_NewCFunction(ctx, js_widget_child_at,       "child_at",       1));
    JS_SetPropertyStr(ctx, proto, "remove_child",
        JS_NewCFunction(ctx, js_widget_remove_child,   "remove_child",   1));
    JS_SetPropertyStr(ctx, proto, "perform_layout",
        JS_NewCFunction(ctx, js_widget_perform_layout, "perform_layout", 0));
    JS_SetPropertyStr(ctx, proto, "set_layout",
        JS_NewCFunction(ctx, js_widget_set_layout,    "set_layout",    1));
    JS_SetPropertyStr(ctx, proto, "set_size",
        JS_NewCFunction(ctx, js_widget_set_size,      "set_size",      2));
    JS_SetPropertyStr(ctx, proto, "set_fixed_size",
        JS_NewCFunction(ctx, js_widget_set_fixed_size,"set_fixed_size",2));
    JS_SetPropertyStr(ctx, proto, "set_min_size",
        JS_NewCFunction(ctx, js_widget_set_min_size,  "set_min_size",  2));
    JS_SetPropertyStr(ctx, proto, "set_position",
        JS_NewCFunction(ctx, js_widget_set_position,  "set_position",  2));

    // ------------------------------------------------------------------
    // 4. Attach the prototype to the class.
    //    JS_SetClassProto consumes one reference; we keep our own copy in
    //    widget_proto so subclass bindings can inherit from it.
    // ------------------------------------------------------------------
    JS_SetClassProto(ctx, js_widget_class_id, JS_DupValue(ctx, proto));
    widget_proto = proto; // module-lifetime reference; never freed here

    // ------------------------------------------------------------------
    // 5. Create the constructor function.
    //    JS_SetConstructor sets ctor.prototype = proto and
    //    proto.constructor = ctor (both as non-enumerable props).
    //    JS_SetModuleExport then takes ownership of `ctor`.
    // ------------------------------------------------------------------
    JSValue ctor = JS_NewCFunction2(ctx, js_widget_ctor, "Widget",
                                    0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);

    // ------------------------------------------------------------------
    // 6. Fill the module export slot.
    //    JS_AddModuleExport (the slot pre-declaration) was already called
    //    by register_nanogui_module() before the init callback ran.
    //    We only need JS_SetModuleExport here to hand over the constructor.
    // ------------------------------------------------------------------
    JS_SetModuleExport(ctx, m, "Widget", ctor);
}

} // namespace nanogui::js
