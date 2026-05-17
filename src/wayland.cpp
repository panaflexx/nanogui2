/*
    src/wayland.cpp -- Wayland native pinch-to-zoom gesture support
    using the zwp_pointer_gestures_v1 / zwp_pointer_gesture_pinch_v1 protocol.

    This file is compiled only on Linux Wayland builds.
*/

#include <nanogui/nanogui.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>

#include <functional>
#include <cstring>

NAMESPACE_BEGIN(nanogui)

/* ------------------------------------------------------------------ */
/*  Generated Wayland protocol interface (must be generated from     */
/*  pointer-gestures-unstable-v1.xml and included at build time)      */
/* ------------------------------------------------------------------ */

#include "pointer-gestures-unstable-v1-client-protocol.h"

static std::function<void(double, int, int)> g_waylandZoomCallback;
static struct zwp_pointer_gesture_pinch_v1 *g_pinch = nullptr;
static double g_last_scale = 1.0;

/* ------------------------------------------------------------------ */
/*  Pinch gesture listeners                                           */
/* ------------------------------------------------------------------ */

static void pinch_begin(void *data,
                        struct zwp_pointer_gesture_pinch_v1 *pinch,
                        uint32_t serial, uint32_t time,
                        struct wl_surface *surface, uint32_t fingers)
{
    g_last_scale = 1.0;
}

static void pinch_update(void *data,
                         struct zwp_pointer_gesture_pinch_v1 *pinch,
                         uint32_t time,
                         wl_fixed_t x, wl_fixed_t y,
                         wl_fixed_t dx, wl_fixed_t dy,
                         wl_fixed_t scale,
                         wl_fixed_t rotation)
{
    if (!g_waylandZoomCallback)
        return;

    double new_scale = wl_fixed_to_double(scale);
    double delta = new_scale - g_last_scale;
    g_last_scale = new_scale;

    // Wayland surface coordinates are already in logical units.
    // NanoGUI uses top-left origin, same as Wayland.
    int px = (int)wl_fixed_to_double(x);
    int py = (int)wl_fixed_to_double(y);

    g_waylandZoomCallback(delta, px, py);
}

static void pinch_end(void *data,
                      struct zwp_pointer_gesture_pinch_v1 *pinch,
                      uint32_t serial, uint32_t time,
                      uint32_t cancelled)
{
    // nothing special
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    .begin  = pinch_begin,
    .update = pinch_update,
    .end    = pinch_end
};

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void set_wayland_zoom_callback(const std::function<void(double, int, int)>& cb) {
    g_waylandZoomCallback = cb;
}

/**
 * Call this after you have a valid GLFW window and Wayland surface.
 * Typical usage in Screen constructor (Linux Wayland only):
 *
 *   struct wl_display *display = glfwGetWaylandDisplay();
 *   struct wl_surface *surface = glfwGetWaylandWindow(m_glfw_window);
 *   enable_wayland_pinch_zoom(display, surface);
 */
void enable_wayland_pinch_zoom(struct wl_display *display,
                               struct wl_surface *surface)
{
    if (!display || !surface)
        return;

    // You must have already bound zwp_pointer_gestures_v1 via the registry.
    // This example assumes you store the global in a singleton or similar.
    extern struct zwp_pointer_gestures_v1 *g_pointer_gestures; // provided by app

    if (!g_pointer_gestures)
        return;

    struct wl_seat *seat = glfwGetWaylandSeat(); // if GLFW exposes it, otherwise obtain manually

    if (g_pinch)
        zwp_pointer_gesture_pinch_v1_destroy(g_pinch);

    g_pinch = zwp_pointer_gestures_v1_get_pinch_gesture(g_pointer_gestures, seat);

    if (g_pinch)
        zwp_pointer_gesture_pinch_v1_add_listener(g_pinch, &pinch_listener, nullptr);
}

NAMESPACE_END(nanogui)
