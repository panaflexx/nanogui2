/*
    nanogui/gestures.h -- Platform pinch / multitouch gesture hooks

    Platform backends (src/wayland.cpp, src/darwin.mm) register a callback
    that delivers a *multiplicative* zoom factor near 1.0 plus a position
    in window coordinates (top-left origin, logical pixels).
*/
#pragma once

#include <nanogui/common.h>
#include <functional>

#if defined(NANOGUI_WAYLAND)
extern "C" {
struct wl_display;
struct wl_surface;
}
#endif

NAMESPACE_BEGIN(nanogui)

/// Callback: (scale_factor, x, y). scale_factor is multiplicative (e.g. 1.02 = +2%).
using GestureZoomCallback = std::function<void(double scale_factor, int x, int y)>;

#if defined(NANOGUI_WAYLAND)
extern NANOGUI_EXPORT void set_wayland_zoom_callback(const GestureZoomCallback& cb);

/// Bind zwp_pointer_gestures_v1 on the given display / surface.
/// Safe no-op if the compositor does not advertise the protocol.
/// Never call wl_display_roundtrip() from here (races GLFW's event loop).
extern NANOGUI_EXPORT void enable_wayland_pinch_zoom(struct ::wl_display* display,
                                                     struct ::wl_surface* surface);
#endif

#if defined(__APPLE__)
extern NANOGUI_EXPORT void set_macos_zoom_callback(const GestureZoomCallback& cb);
extern NANOGUI_EXPORT void enable_macos_pinch_zoom(void* nswindow);
#endif

NAMESPACE_END(nanogui)
