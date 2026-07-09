/*
    src/wayland.cpp -- Wayland touchpad pinch-to-zoom

    Uses zwp_pointer_gestures_v1 / zwp_pointer_gesture_pinch_v1.

    IMPORTANT: Never call wl_display_roundtrip() on GLFW's display from here.
    GLFW owns the event loop; roundtrips re-dispatch and easily produce
    xdg_wm_base protocol errors. Globals arrive via normal dispatch
    (glfwPollEvents / WaitEvents).
*/

#include <nanogui/gestures.h>

#include <wayland-client.h>
#include "pointer-gestures-unstable-v1-client-protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using ::wl_display;
using ::wl_surface;
using ::wl_registry;
using ::wl_seat;
using ::wl_pointer;
using ::wl_fixed_t;

NAMESPACE_BEGIN(nanogui)

namespace {

GestureZoomCallback g_zoom_cb;

struct GesturesState {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    zwp_pointer_gestures_v1* gestures = nullptr;
    zwp_pointer_gesture_pinch_v1* pinch = nullptr;
    double last_scale = 1.0;
    double pos_x = 0.0;
    double pos_y = 0.0;
    bool enabled = false;
};

GesturesState g;

void pointer_enter(void*, wl_pointer*, uint32_t, wl_surface*,
                   wl_fixed_t sx, wl_fixed_t sy) {
    g.pos_x = wl_fixed_to_double(sx);
    g.pos_y = wl_fixed_to_double(sy);
}

void pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) { }

void pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    g.pos_x = wl_fixed_to_double(sx);
    g.pos_y = wl_fixed_to_double(sy);
}

void pointer_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) { }
void pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) { }
void pointer_frame(void*, wl_pointer*) { }
void pointer_axis_source(void*, wl_pointer*, uint32_t) { }
void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) { }
void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) { }

const wl_pointer_listener pointer_listener = {
    pointer_enter,
    pointer_leave,
    pointer_motion,
    pointer_button,
    pointer_axis,
    pointer_frame,
    pointer_axis_source,
    pointer_axis_stop,
    pointer_axis_discrete,
    nullptr,
    nullptr
};

void pinch_begin(void*, zwp_pointer_gesture_pinch_v1*,
                 uint32_t, uint32_t, wl_surface*, uint32_t) {
    g.last_scale = 1.0;
}

void pinch_update(void*, zwp_pointer_gesture_pinch_v1*,
                  uint32_t,
                  wl_fixed_t dx, wl_fixed_t dy,
                  wl_fixed_t fscale,
                  wl_fixed_t /*rotation*/) {
    if (!g_zoom_cb)
        return;

    g.pos_x += wl_fixed_to_double(dx);
    g.pos_y += wl_fixed_to_double(dy);

    double scale = wl_fixed_to_double(fscale);
    if (!(scale > 0.0) || !std::isfinite(scale))
        return;

    double factor = scale / (g.last_scale > 1e-9 ? g.last_scale : 1.0);
    g.last_scale = scale;

    if (!std::isfinite(factor) || factor <= 0.0)
        return;
    if (std::fabs(factor - 1.0) < 1e-6)
        return;
    if (factor < 0.5) factor = 0.5;
    if (factor > 2.0) factor = 2.0;

    g_zoom_cb(factor, (int)std::lround(g.pos_x), (int)std::lround(g.pos_y));
}

void pinch_end(void*, zwp_pointer_gesture_pinch_v1*,
               uint32_t, uint32_t, int32_t) {
    g.last_scale = 1.0;
}

const zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    pinch_begin,
    pinch_update,
    pinch_end
};

void ensure_pinch() {
    if (g.pinch || !g.gestures || !g.pointer)
        return;
    g.pinch = zwp_pointer_gestures_v1_get_pinch_gesture(g.gestures, g.pointer);
    if (g.pinch) {
        zwp_pointer_gesture_pinch_v1_add_listener(g.pinch, &pinch_listener, nullptr);
        std::fprintf(stderr, "nanogui: wayland: pinch-to-zoom gesture enabled\n");
    }
}

void seat_capabilities(void*, wl_seat* seat, uint32_t caps) {
    const bool has_pointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_pointer && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        if (g.pointer) {
            wl_pointer_add_listener(g.pointer, &pointer_listener, nullptr);
            ensure_pinch();
        }
    } else if (!has_pointer && g.pointer) {
        if (g.pinch) {
            zwp_pointer_gesture_pinch_v1_destroy(g.pinch);
            g.pinch = nullptr;
        }
        wl_pointer_destroy(g.pointer);
        g.pointer = nullptr;
    }
}

void seat_name(void*, wl_seat*, const char*) { }

const wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name
};

void registry_global(void*, wl_registry* registry,
                     uint32_t name, const char* interface, uint32_t version) {
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        if (!g.seat) {
            uint32_t v = version < 5 ? version : 5;
            g.seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, v);
            if (g.seat)
                wl_seat_add_listener(g.seat, &seat_listener, nullptr);
        }
    } else if (std::strcmp(interface, zwp_pointer_gestures_v1_interface.name) == 0) {
        if (!g.gestures) {
            uint32_t v = version < 1 ? 1 : (version > 3 ? 3 : version);
            g.gestures = (zwp_pointer_gestures_v1*)
                wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, v);
            ensure_pinch();
        }
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) { }

const wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

} // namespace

void set_wayland_zoom_callback(const GestureZoomCallback& cb) {
    g_zoom_cb = cb;
}

void enable_wayland_pinch_zoom(wl_display* display, wl_surface* surface) {
    (void)surface;
    if (!display || g.enabled)
        return;

    g.display = display;
    g.registry = wl_display_get_registry(display);
    if (!g.registry) {
        std::fprintf(stderr, "nanogui: wayland: failed to get registry\n");
        return;
    }
    wl_registry_add_listener(g.registry, &registry_listener, nullptr);
    g.enabled = true;
    // Do NOT roundtrip — wait for next glfwPollEvents/WaitEvents.
}

NAMESPACE_END(nanogui)
