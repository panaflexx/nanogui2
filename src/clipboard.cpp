/*
    src/clipboard.cpp -- see clipboard.h.

    The X11 read uses the application window and its own property, not
    GLFW's helper window, so this does not take over GLFW's selection.
    SelectionNotify is taken with XCheckIfEvent, which leaves every other
    event queued. A SelectionRequest that arrives while we wait is left
    queued too: GLFW answers it when it next polls, and we do not answer
    it here.
*/

#include <nanogui/clipboard.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#  define GLFW_EXPOSE_NATIVE_X11
#  include <GLFW/glfw3native.h>
#  include <X11/Xatom.h>
#  include <X11/Xlib.h>
#  include <poll.h>
#endif

NAMESPACE_BEGIN(nanogui)

namespace {

#if defined(__linux__)

/* This file is inside namespace nanogui, where Window is the widget. */
using XWindow = ::Window;

struct SelWant {
    XWindow window;
    Atom selection;
    Atom target;
};

struct PropWant {
    XWindow window;
    Atom property;
};

extern "C" Bool selection_is(Display *, XEvent *event, XPointer arg) {
    const SelWant *want = reinterpret_cast<const SelWant *>(arg);
    return event->type == SelectionNotify && event->xselection.requestor == want->window &&
           event->xselection.selection == want->selection && event->xselection.target == want->target;
}

extern "C" Bool property_is(Display *, XEvent *event, XPointer arg) {
    const PropWant *want = reinterpret_cast<const PropWant *>(arg);
    return event->type == PropertyNotify && event->xproperty.window == want->window &&
           event->xproperty.atom == want->property && event->xproperty.state == PropertyNewValue;
}

/* Pull matching events that are already queued, then wait for the X
 * socket until one arrives or the deadline passes. */
bool wait_for(Display *dpy, Bool (*pred)(Display *, XEvent *, XPointer), XPointer arg,
              XEvent *event, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (XCheckIfEvent(dpy, event, pred, arg))
            return true;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0)
            return false;
        pollfd pfd;
        pfd.fd = ConnectionNumber(dpy);
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, (int) left);
        XPending(dpy);                   // move socket bytes into the queue
    }
}

std::string property_bytes(Display *dpy, XWindow window, Atom property, Atom incr, int timeout_ms) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, left = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, window, property, 0, LONG_MAX, True, AnyPropertyType, &type, &format,
                           &count, &left, &data) != Success)
        return {};
    if (type == incr) {
        if (data)
            XFree(data);
        std::string out;
        PropWant want{window, property};
        for (;;) {
            XEvent event;
            if (!wait_for(dpy, property_is, reinterpret_cast<XPointer>(&want), &event, timeout_ms))
                return {};
            data = nullptr;
            count = 0;
            if (XGetWindowProperty(dpy, window, property, 0, LONG_MAX, True, AnyPropertyType, &type,
                                   &format, &count, &left, &data) != Success)
                return {};
            if (count == 0) {
                if (data)
                    XFree(data);
                return out;
            }
            if (data && format == 8)
                out.append(reinterpret_cast<const char *>(data), count);
            if (data)
                XFree(data);
        }
    }
    std::string out;
    if (data && format == 8 && count)
        out.assign(reinterpret_cast<const char *>(data), count);
    if (data)
        XFree(data);
    return out;
}

/* GLFW's clipboard owner is a 1x1 InputOnly window. A round trip to it
 * cannot complete while we are inside a menu callback, because GLFW answers
 * SelectionRequest on its own poll. The UTF-8 string is read with
 * glfwGetClipboardString, which returns that owned string without a trip. */
bool owner_is_helper(Display *dpy, XWindow owner) {
    XWindowAttributes attr;
    std::memset(&attr, 0, sizeof attr);
    if (!XGetWindowAttributes(dpy, owner, &attr))
        return false;
    return attr.c_class == InputOnly && attr.width <= 1 && attr.height <= 1;
}

void ensure_property_mask(Display *dpy, XWindow window) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, window, &attr))
        return;
    if (attr.your_event_mask & PropertyChangeMask)
        return;
    XSelectInput(dpy, window, attr.your_event_mask | PropertyChangeMask);
}

std::string read_target(Display *dpy, XWindow window, Atom selection, Atom target, Atom property,
                        Atom incr, int timeout_ms) {
    XConvertSelection(dpy, selection, target, property, window, CurrentTime);
    XFlush(dpy);
    SelWant want{window, selection, target};
    XEvent event;
    if (!wait_for(dpy, selection_is, reinterpret_cast<XPointer>(&want), &event, timeout_ms))
        return {};
    if (event.xselection.property == None)
        return {};
    return property_bytes(dpy, window, event.xselection.property, incr, timeout_ms);
}

void read_x11(GLFWwindow *window, std::vector<std::pair<std::string, std::string>> &out) {
    Display *dpy = glfwGetX11Display();
    const XWindow xwin = (XWindow) glfwGetX11Window(window);
    if (!dpy || !xwin)
        return;
    const Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    const XWindow owner = XGetSelectionOwner(dpy, clipboard);
    if (owner == None)
        return;
    if (owner_is_helper(dpy, owner)) {
        /* A GLFW clipboard owner. Ours while GLFW still holds the formats
         * write_clipboard offered (it drops them when another client takes
         * the clipboard); otherwise the caller falls back to the string. */
        int count = 0;
        const GLFWclipboardformat *formats = glfwGetClipboardFormats(&count);
        for (int i = 0; i < count; ++i)
            out.emplace_back(formats[i].type,
                             std::string(static_cast<const char *>(formats[i].data), formats[i].size));
        return;
    }
    ensure_property_mask(dpy, xwin);
    const Atom property = XInternAtom(dpy, "NANOGUI_SELECTION", False);
    const Atom incr = XInternAtom(dpy, "INCR", False);
    const char *names[] = {"image/svg+xml", "image/svg", "text/xml", "text/plain", "UTF8_STRING"};
    const int budget_ms = 80;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    for (const char *name : names) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0)
            break;
        const Atom target = XInternAtom(dpy, name, False);
        std::string bytes = read_target(dpy, xwin, clipboard, target, property, incr, (int) left);
        if (!bytes.empty()) {
            out.emplace_back(name, std::move(bytes));
            return;                      // the first target that answers is enough
        }
    }
    const Atom string_atom = XA_STRING;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left > 0) {
        std::string bytes = read_target(dpy, xwin, clipboard, string_atom, property, incr, (int) left);
        if (!bytes.empty())
            out.emplace_back("STRING", std::move(bytes));
    }
}

#endif

} // namespace

std::vector<std::pair<std::string, std::string>> read_clipboard(GLFWwindow *window) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!window)
        return out;
#if defined(__linux__)
    read_x11(window, out);
#endif
    if (out.empty())
        if (const char *text = glfwGetClipboardString(window))
            if (*text)
                out.emplace_back("text/plain", std::string(text));
    return out;
}

void write_clipboard(GLFWwindow *window,
                     const std::vector<std::pair<std::string, std::string>> &formats) {
    std::vector<GLFWclipboardformat> list;
    list.reserve(formats.size());
    for (const auto &f : formats)
        list.push_back({f.first.c_str(), f.second.data(), f.second.size()});
    glfwSetClipboardFormats(window, list.data(), (int) list.size());
}

NAMESPACE_END(nanogui)
