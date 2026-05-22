/*
    src/screen.cpp -- Top-level widget and interface between NanoGUI and GLFW

    A significant redesign of this code was contributed by Christian Schueller.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/screen.h>
#include <chrono>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>
#include <nanogui/window.h>
#include <nanogui/popup.h>
#include <nanogui/menu.h>
#include <nanogui/popupbutton.h>
#include <nanogui/metal.h>
#include <nanogui/textbox.h>
#include <nanogui/textarea.h>
#include <nanogui/checkbox.h>
#include <nanogui/combobox.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanogui/cachedwidget.h>
#include <map>
#include <iostream>

#if defined(EMSCRIPTEN)
#  include <emscripten/emscripten.h>
#  include <emscripten/html5.h>
#endif

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  undef APIENTRY

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
# endif
#  include <windows.h>

#  define GLFW_EXPOSE_NATIVE_WGL
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#endif

/* Allow enforcing the GL2 implementation of NanoVG */

#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
#  if defined(NANOGUI_USE_OPENGL)
#    define NANOVG_GL3_IMPLEMENTATION
#  elif defined(NANOGUI_USE_GLES)
#    define NANOVG_GLES2_IMPLEMENTATION
#  endif
#  include <nanovg_gl.h>
#  include "opengl_check.h"
#elif defined(NANOGUI_USE_METAL)
#  include <nanovg_mtl.h>
#endif

#if defined(__APPLE__)
#  define GLFW_EXPOSE_NATIVE_COCOA 1
#  include <GLFW/glfw3native.h>
#endif

#if !defined(GL_RGBA_FLOAT_MODE)
#  define GL_RGBA_FLOAT_MODE 0x8820
#endif

NAMESPACE_BEGIN(nanogui)

std::map<GLFWwindow*, Screen*> __nanogui_screens;

#if defined(NANOGUI_GLAD)
static bool glad_initialized = false;
#endif

/* Calculate pixel ratio for hi-dpi devices. */
static float get_pixel_ratio(GLFWwindow* window) {
#if defined(EMSCRIPTEN)
    return emscripten_get_device_pixel_ratio();
#elif defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window);
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    /* The following function only exists on Windows 8.1+, but we don't want to make that a dependency */
    static HRESULT(WINAPI * GetDpiForMonitor_)(HMONITOR, UINT, UINT*, UINT*) = nullptr;
    static bool GetDpiForMonitor_tried = false;

    if (!GetDpiForMonitor_tried) {
        auto shcore = LoadLibrary(TEXT("shcore"));
        if (shcore)
            GetDpiForMonitor_ = (decltype(GetDpiForMonitor_))GetProcAddress(shcore, "GetDpiForMonitor");
        GetDpiForMonitor_tried = true;
    }

    if (GetDpiForMonitor_) {
        uint32_t dpi_x, dpi_y;
        if (GetDpiForMonitor_(monitor, 0 /* effective DPI */, &dpi_x, &dpi_y) == S_OK)
            return dpi_x / 96.0;
    }
    return 1.f;
#elif defined(__linux__)
    (void)window;

    float ratio = 1.0f;
    FILE* fp;
    /* Try to read the pixel ratio from KDEs config */
    auto currentDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (currentDesktop && currentDesktop == std::string("KDE")) {
        fp = popen("kreadconfig5 --group KScreen --key ScaleFactor", "r");
        if (!fp)
            return 1;

        if (fscanf(fp, "%f", &ratio) != 1)
            return 1;
    }
    else {
        /* Try to read the pixel ratio from GTK */
        fp = popen("gsettings get org.gnome.desktop.interface scaling-factor", "r");
        if (!fp)
            return 1;

        int ratioInt = 1;
        if (fscanf(fp, "uint32 %i", &ratioInt) != 1)
            return 1;
        ratio = ratioInt;
    }
    if (pclose(fp) != 0)
        return 1;
    return ratio >= 1 ? ratio : 1;
#else
    Vector2i fb_size, size;
    glfwGetFramebufferSize(window, &fb_size[0], &fb_size[1]);
    glfwGetWindowSize(window, &size[0], &size[1]);
    return (float)fb_size[0] / (float)size[0];
#endif
}

#if defined(EMSCRIPTEN)
static EM_BOOL nanogui_emscripten_resize_callback(int eventType, const EmscriptenUiEvent*, void*) {
    double ratio = emscripten_get_device_pixel_ratio();

    int w1, h1;
    emscripten_get_canvas_element_size("#canvas", &w1, &h1);

    double w2, h2;
    emscripten_get_element_css_size("#canvas", &w2, &h2);

    double w3 = w2 * ratio, h3 = h2 * ratio;

    if (w1 != (int)w3 || h1 != (int)h3)
        emscripten_set_canvas_element_size("#canvas", w3, h3);

    for (auto it : __nanogui_screens) {
        Screen* screen = it.second;
        screen->resize_event(Vector2i((int)w2, (int)h2));
        screen->redraw();
    }

    return true;
}
#endif

Screen::Screen()
    : WidgetCRTP<Screen>(nullptr), m_glfw_window(nullptr), m_nvg_context(nullptr),
    m_cursor(Cursor::Arrow), m_background(0.3f, 0.3f, 0.32f, 1.f),
    m_shutdown_glfw(false), m_fullscreen(false), m_depth_buffer(false),
    m_stencil_buffer(false), m_float_buffer(false), m_redraw(false) {
    DebugName = "Screen";
    memset(m_cursors, 0, sizeof(GLFWcursor*) * (size_t)Cursor::CursorCount);
#if defined(NANOGUI_USE_OPENGL)
    GLint n_stencil_bits = 0, n_depth_bits = 0;
    GLboolean float_mode;
    CHK(glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
        GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &n_depth_bits));
    CHK(glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
        GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &n_stencil_bits));
    CHK(glGetBooleanv(GL_RGBA_FLOAT_MODE, &float_mode));
    m_depth_buffer = n_depth_bits > 0;
    m_stencil_buffer = n_stencil_bits > 0;
    m_float_buffer = (bool)float_mode;
#endif
}

Screen::Screen(const Vector2i& size, const std::string& caption, bool resizable,
    bool fullscreen, bool depth_buffer, bool stencil_buffer,
    bool float_buffer, unsigned int gl_major, unsigned int gl_minor)
    : WidgetCRTP<Screen>(nullptr), m_glfw_window(nullptr), m_nvg_context(nullptr),
    m_cursor(Cursor::Arrow), m_background(0.3f, 0.3f, 0.32f, 1.f), m_caption(caption),
    m_shutdown_glfw(false), m_fullscreen(fullscreen), m_depth_buffer(depth_buffer),
    m_stencil_buffer(stencil_buffer), m_float_buffer(float_buffer), m_redraw(false) {
    memset(m_cursors, 0, sizeof(GLFWcursor*) * (int)Cursor::CursorCount);

#if defined(NANOGUI_USE_OPENGL)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);

    /* Request a forward compatible OpenGL gl_major.gl_minor core profile context.
       Default value is an OpenGL 3.3 core profile context. */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, gl_major);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, gl_minor);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#elif defined(NANOGUI_USE_GLES)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, NANOGUI_GLES_VERSION);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#elif defined(NANOGUI_USE_METAL)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_stencil_buffer = stencil_buffer = false;
#else
#  error Did not select a graphics API!
#endif

    int color_bits = 8, depth_bits = 0, stencil_bits = 0;

    if (stencil_buffer && !depth_buffer)
        throw std::runtime_error(
            "Screen::Screen(): stencil_buffer = True requires depth_buffer = True");
    if (depth_buffer)
        depth_bits = 32;
    if (stencil_buffer) {
        depth_bits = 24;
        stencil_bits = 8;
    }
    if (m_float_buffer)
        color_bits = 16;

    glfwWindowHint(GLFW_RED_BITS, color_bits);
    glfwWindowHint(GLFW_GREEN_BITS, color_bits);
    glfwWindowHint(GLFW_BLUE_BITS, color_bits);
    glfwWindowHint(GLFW_ALPHA_BITS, color_bits);
    glfwWindowHint(GLFW_STENCIL_BITS, stencil_bits);
    glfwWindowHint(GLFW_DEPTH_BITS, depth_bits);

#if (defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_METAL)) && defined(GLFW_FLOATBUFFER)
    glfwWindowHint(GLFW_FLOATBUFFER, m_float_buffer ? GL_TRUE : GL_FALSE);
#else
    m_float_buffer = false;
#endif

    glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, resizable ? GL_TRUE : GL_FALSE);

    for (int i = 0; i < 2; ++i) {
        if (fullscreen) {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            m_glfw_window = glfwCreateWindow(mode->width, mode->height,
                caption.c_str(), monitor, nullptr);
        }
        else {
            m_glfw_window = glfwCreateWindow(size.x(), size.y(),
                caption.c_str(), nullptr, nullptr);
        }

        if (m_glfw_window == nullptr && m_float_buffer) {
            m_float_buffer = false;
#if defined(GLFW_FLOATBUFFER)
            glfwWindowHint(GLFW_FLOATBUFFER, GL_FALSE);
#endif
            fprintf(stderr, "Could not allocate floating point framebuffer, retrying without..\n");
        }
        else {
            break;
        }
    }

#if defined(NANOGUI_USE_OPENGL)
    if (m_float_buffer) {
        GLboolean float_mode;
        CHK(glGetBooleanv(GL_RGBA_FLOAT_MODE, &float_mode));
        if (!float_mode) {
            fprintf(stderr, "Could not allocate floating point framebuffer.\n");
            m_float_buffer = false;
        }
    }
#endif

    if (!m_glfw_window) {
        (void)gl_major; (void)gl_minor;
#if defined(NANOGUI_USE_OPENGL)
        throw std::runtime_error("Could not create an OpenGL " +
            std::to_string(gl_major) + "." +
            std::to_string(gl_minor) + " context!");
#elif defined(NANOGUI_USE_GLES)
        throw std::runtime_error("Could not create a GLES 2 context!");
#elif defined(NANOGUI_USE_METAL)
        throw std::runtime_error(
            "Could not create a GLFW window for rendering using Metal!");
#endif
    }

#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
    glfwMakeContextCurrent(m_glfw_window);
#endif

    glfwSetInputMode(m_glfw_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

#if defined(NANOGUI_GLAD)
    if (!glad_initialized) {
        glad_initialized = true;
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
            throw std::runtime_error("Could not initialize GLAD!");
        glGetError(); // pull and ignore unhandled errors like GL_INVALID_ENUM
    }
#endif

    glfwGetFramebufferSize(m_glfw_window, &m_fbsize[0], &m_fbsize[1]);

#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
    CHK(glViewport(0, 0, m_fbsize[0], m_fbsize[1]));
    CHK(glClearColor(m_background[0], m_background[1],
        m_background[2], m_background[3]));
    CHK(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
        GL_STENCIL_BUFFER_BIT));

    glfwSwapInterval(0);
    glfwSwapBuffers(m_glfw_window);
#endif

#if defined(__APPLE__)
    /* Poll for events once before starting a potentially
       lengthy loading process. This is needed to be
       classified as "interactive" by other software such
       as iTerm2 */

    glfwPollEvents();
#endif

    /* Propagate GLFW events to the appropriate Screen instance */
    glfwSetCursorPosCallback(m_glfw_window,
        [](GLFWwindow* w, double x, double y) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;
            Screen* s = it->second;
            if (!s->m_process_events)
                return;
            s->cursor_pos_callback_event(x, y);
        }
    );

    glfwSetMouseButtonCallback(m_glfw_window,
        [](GLFWwindow* w, int button, int action, int modifiers) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;
            Screen* s = it->second;
            if (!s->m_process_events)
                return;
            s->mouse_button_callback_event(button, action, modifiers);
        }
    );

    glfwSetKeyCallback(m_glfw_window,
        [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;
            Screen* s = it->second;
            if (!s->m_process_events)
                return;
            s->key_callback_event(key, scancode, action, mods);
        }
    );

    glfwSetCharCallback(m_glfw_window,
        [](GLFWwindow* w, unsigned int codepoint) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;
            Screen* s = it->second;
            if (!s->m_process_events)
                return;
            s->char_callback_event(codepoint);
        }
    );

    glfwSetDropCallback(m_glfw_window,
        [](GLFWwindow* w, int count, const char** filenames) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;
            Screen* s = it->second;
            if (!s->m_process_events)
                return;
            s->drop_callback_event(count, filenames);
        }
    );

    glfwSetScrollCallback(m_glfw_window,
        [](GLFWwindow* w, double x, double y) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;
            Screen* s = it->second;
            if (!s->m_process_events)
                return;
            s->scroll_callback_event(x, y);
        }
    );

    /* React to framebuffer size events -- includes window
       size events and also catches things like dragging
       a window from a Retina-capable screen to a normal
       screen on Mac OS X */
    glfwSetFramebufferSizeCallback(m_glfw_window,
        [](GLFWwindow* w, int width, int height) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;
            Screen* s = it->second;

            if (!s->m_process_events)
                return;

            s->resize_callback_event(width, height);
        }
    );

    // notify when the screen has lost focus (e.g. application switch)
    glfwSetWindowFocusCallback(m_glfw_window,
        [](GLFWwindow* w, int focused) {
            auto it = __nanogui_screens.find(w);
            if (it == __nanogui_screens.end())
                return;

            Screen* s = it->second;
            // focus_event: 0 when false, 1 when true
            s->focus_event(focused != 0);
        }
    );
    initialize(m_glfw_window, true);

    /* -------------------------------------------------------------- */
    /*  Register platform-specific pinch-to-zoom handlers            */
    /* -------------------------------------------------------------- */
#if defined(__APPLE__)
    // darwin.mm provides these
    extern void enable_macos_pinch_zoom(void*);
    extern void set_macos_zoom_callback(const std::function<void(double,int,int)>&);
	void* nswin = glfwGetCocoaWindow(m_glfw_window);
    enable_macos_pinch_zoom(nswin);
    set_macos_zoom_callback(
        [this](double mag, int x, int y) {
            this->zoom_callback_event(mag, Vector2i(x, y));
        });
#elif defined(WAYLAND)
    // wayland.cpp provides these
    extern void enable_wayland_pinch_zoom(struct wl_display*, struct wl_surface*);
    extern void set_wayland_zoom_callback(const std::function<void(double,int,int)>&);
    struct wl_display* dpy = glfwGetWaylandDisplay();
    struct wl_surface* srf = glfwGetWaylandWindow(m_glfw_window);
    if (dpy && srf) {
        enable_wayland_pinch_zoom(dpy, srf);
        set_wayland_zoom_callback(
            [this](double mag, int x, int y) {
                this->zoom_callback_event(mag, Vector2i(x, y));
            });
    }
#else
	printf("Pinch zoom not enabled\n");
#endif

#if defined(NANOGUI_USE_METAL)
    if (depth_buffer) {
        m_depth_stencil_texture = new Texture(
            stencil_buffer ? Texture::PixelFormat::DepthStencil
            : Texture::PixelFormat::Depth,
            Texture::ComponentFormat::Float32,
            framebuffer_size(),
            Texture::InterpolationMode::Bilinear,
            Texture::InterpolationMode::Bilinear,
            Texture::WrapMode::ClampToEdge,
            1,
            Texture::TextureFlags::RenderTarget
        );
    }
#endif
}

void Screen::initialize(GLFWwindow* window, bool shutdown_glfw) {
    m_glfw_window = window;
    m_shutdown_glfw = shutdown_glfw;
    glfwGetWindowSize(m_glfw_window, &m_size[0], &m_size[1]);
    glfwGetFramebufferSize(m_glfw_window, &m_fbsize[0], &m_fbsize[1]);

    m_pixel_ratio = get_pixel_ratio(window);

#if defined(EMSCRIPTEN)
    double w, h;
    emscripten_get_element_css_size("#canvas", &w, &h);
    double ratio = emscripten_get_device_pixel_ratio(),
        w2 = w * ratio, h2 = h * ratio;

    if (w != m_size[0] || h != m_size[1]) {
        /* The canvas element is configured as width/height: auto, expand to
           the available space instead of using the specified window resolution */
        nanogui_emscripten_resize_callback(0, nullptr, nullptr);
        emscripten_set_resize_callback(nullptr, nullptr, false,
            nanogui_emscripten_resize_callback);
    }
    else if (w != w2 || h != h2) {
        /* Configure for rendering on a high-DPI display */
        emscripten_set_canvas_element_size("#canvas", (int)w2, (int)h2);
        emscripten_set_element_css_size("#canvas", w, h);
    }
    m_fbsize = Vector2i((int)w2, (int)h2);
    m_size = Vector2i((int)w, (int)h);
#elif defined(_WIN32) || defined(__linux__)
    if (m_pixel_ratio != 1 && !m_fullscreen)
        glfwSetWindowSize(window, m_size.x() * m_pixel_ratio,
            m_size.y() * m_pixel_ratio);
#endif

#if defined(NANOGUI_GLAD)
    if (!glad_initialized) {
        glad_initialized = true;
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
            throw std::runtime_error("Could not initialize GLAD!");
        glGetError(); // pull and ignore unhandled errors like GL_INVALID_ENUM
    }
#endif

    int flags = NVG_ANTIALIAS;
    if (m_stencil_buffer)
        flags |= NVG_STENCIL_STROKES;
#if !defined(NDEBUG)
    flags |= NVG_DEBUG;
#endif

#if defined(NANOGUI_USE_OPENGL)
    m_nvg_context = nvgCreateGL3(flags);
#elif defined(NANOGUI_USE_GLES)
    m_nvg_context = nvgCreateGLES2(flags);
#elif defined(NANOGUI_USE_METAL)
    void* nswin = glfwGetCocoaWindow(window);
    metal_window_init(nswin, m_float_buffer);
    metal_window_set_size(nswin, m_fbsize);
    m_nvg_context = nvgCreateMTL(metal_layer(),
        metal_command_queue(),
        flags | NVG_TRIPLE_BUFFER);
#endif

    if (!m_nvg_context)
        throw std::runtime_error("Could not initialize NanoVG!");

    m_visible = glfwGetWindowAttrib(window, GLFW_VISIBLE) != 0;
    set_theme(new Theme(m_nvg_context));
    m_mouse_pos = Vector2i(0);
    m_mouse_state = m_modifiers = 0;
    m_drag_active = false;
    m_last_interaction = glfwGetTime();
    m_process_events = true;
    m_redraw = true;
    __nanogui_screens[m_glfw_window] = this;

    for (size_t i = 0; i < (size_t)Cursor::CursorCount; ++i)
        m_cursors[i] = glfwCreateStandardCursor(GLFW_ARROW_CURSOR + (int)i);

    /// Fixes retina display-related font rendering issue (#185)
    nvgBeginFrame(m_nvg_context, m_size[0], m_size[1], m_pixel_ratio);
    nvgEndFrame(m_nvg_context);
}

Screen::~Screen() {
    __nanogui_screens.erase(m_glfw_window);
    for (size_t i = 0; i < (size_t)Cursor::CursorCount; ++i) {
        if (m_cursors[i])
            glfwDestroyCursor(m_cursors[i]);
    }

    if (m_nvg_context) {
#if defined(NANOGUI_USE_OPENGL)
        nvgDeleteGL3(m_nvg_context);
#elif defined(NANOGUI_USE_GLES)
        nvgDeleteGLES2(m_nvg_context);
#elif defined(NANOGUI_USE_METAL)
        nvgDeleteMTL(m_nvg_context);
#endif
    }

    if (m_glfw_window && m_shutdown_glfw)
        glfwDestroyWindow(m_glfw_window);
}

void Screen::set_visible(bool visible) {
    if (m_visible != visible) {
        m_visible = visible;

        if (visible)
            glfwShowWindow(m_glfw_window);
        else
            glfwHideWindow(m_glfw_window);
    }
}

void Screen::set_caption(const std::string& caption) {
    if (caption != m_caption) {
        glfwSetWindowTitle(m_glfw_window, caption.c_str());
        m_caption = caption;
    }
}

void Screen::set_size(const Vector2i& size) {
    Widget::set_size(size);

#if defined(_WIN32) || defined(__linux__) || defined(EMSCRIPTEN)
    glfwSetWindowSize(m_glfw_window, size.x() * m_pixel_ratio,
        size.y() * m_pixel_ratio);
#else
    glfwSetWindowSize(m_glfw_window, size.x(), size.y());
#endif
}

void Screen::clear() {
#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
    CHK(glClearColor(m_background[0], m_background[1], m_background[2], m_background[3]));
    CHK(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
#elif defined(NANOGUI_USE_METAL)
    mnvgClearWithColor(m_nvg_context, m_background);
#endif
}

void Screen::draw_setup() {
#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
    glfwMakeContextCurrent(m_glfw_window);
#elif defined(NANOGUI_USE_METAL)
    void* nswin = glfwGetCocoaWindow(m_glfw_window);
    metal_window_set_size(nswin, m_fbsize);
    m_metal_drawable = metal_window_next_drawable(nswin);
    m_metal_texture = metal_drawable_texture(m_metal_drawable);
    mnvgSetColorTexture(m_nvg_context, m_metal_texture);
#endif

#if !defined(EMSCRIPTEN)
    glfwGetFramebufferSize(m_glfw_window, &m_fbsize[0], &m_fbsize[1]);
    glfwGetWindowSize(m_glfw_window, &m_size[0], &m_size[1]);
#else
    emscripten_get_canvas_element_size("#canvas", &m_size[0], &m_size[1]);
    m_fbsize = m_size;
#endif

#if defined(_WIN32) || defined(__linux__) || defined(EMSCRIPTEN)
    m_fbsize = m_size;
    m_size = Vector2i(Vector2f(m_size) / m_pixel_ratio);
#else
    /* Recompute pixel ratio on OSX */
    if (m_size[0])
        m_pixel_ratio = (float)m_fbsize[0] / (float)m_size[0];
#endif

#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
    CHK(glViewport(0, 0, m_fbsize[0], m_fbsize[1]));
#endif
}

void Screen::draw_teardown() {
#if defined(NANOGUI_USE_OPENGL) || defined(NANOGUI_USE_GLES)
    glfwSwapBuffers(m_glfw_window);
#elif defined(NANOGUI_USE_METAL)
    mnvgSetColorTexture(m_nvg_context, nullptr);
    metal_present_and_release_drawable(m_metal_drawable);
    m_metal_texture = nullptr;
    m_metal_drawable = nullptr;
#endif
}

void Screen::draw_all() {
    if (m_redraw) {
        m_redraw = false;

#if defined(_DEBUG) || !defined(NDEBUG)
        auto _t0 = std::chrono::high_resolution_clock::now();
#endif
        draw_setup();
        draw_contents();
        draw_widgets();
        draw_teardown();
#if defined(_DEBUG) || !defined(NDEBUG)
        auto _t1 = std::chrono::high_resolution_clock::now();
        m_last_redraw_ms = std::chrono::duration<float, std::milli>(_t1 - _t0).count();
        m_redraw_accum_ms += m_last_redraw_ms;
        m_redraw_count++;
        m_redraw_min_ms = std::min(m_redraw_min_ms, m_last_redraw_ms);
        m_redraw_max_ms = std::max(m_redraw_max_ms, m_last_redraw_ms);
#endif
    }
}

void Screen::draw_contents() {
    clear();
}

void Screen::nvg_flush() {
    NVGparams* params = nvgInternalParams(m_nvg_context);
    params->renderFlush(params->userPtr);
    params->renderViewport(params->userPtr, m_size[0], m_size[1], m_pixel_ratio);
}

void Screen::draw_widgets() {
    // Update any CachedWidget FBOs BEFORE we begin the screen frame so
    // they don't disturb ancestor NanoVG transform/scissor state.
    CachedWidget::process_pending_updates(m_nvg_context);

    nvgBeginFrame(m_nvg_context, m_size[0], m_size[1], m_pixel_ratio);

    draw(m_nvg_context);

	// Draw light cyan border around focused widget
    if (!m_focus_path.empty() && m_focus_path.front() && m_focus_path.back()->visible()) {
        Widget* focused_widget = m_focus_path.front();
        Vector2i pos = focused_widget->absolute_position();
        Vector2i size = focused_widget->size();

        // Don't draw focus on windows
        if (focused_widget->enabled() && dynamic_cast<Window*>(focused_widget) == nullptr) {
            // Check if the focused widget is inside a (Zoom)ScrollPanel
            Widget* parent = focused_widget->parent();
            ScrollPanel* scroll_panel = nullptr;
            ZoomScrollPanel* zoom_panel = nullptr;
            while (parent && !scroll_panel && !zoom_panel) {
                zoom_panel = dynamic_cast<ZoomScrollPanel*>(parent);
                if (!zoom_panel)
                    scroll_panel = dynamic_cast<ScrollPanel*>(parent);
                parent = parent->parent();
            }

            // For ZoomScrollPanel: re-project pos/size through the zoom transform
            // (absolute_position() walks logical positions and doesn't know about zoom).
            if (zoom_panel) {
                Vector2i panel_abs = zoom_panel->absolute_position();
                Vector2i rel = pos - panel_abs;                  // logical offset within child
                double z = zoom_panel->zoom();
                auto pan = zoom_panel->pan_offset();
                pos.x() = panel_abs.x() + (int)std::lround(pan.x() + rel.x() * z);
                pos.y() = panel_abs.y() + (int)std::lround(pan.y() + rel.y() * z);
                size.x() = (int)std::lround(size.x() * z);
                size.y() = (int)std::lround(size.y() * z);
            }

            if (zoom_panel || scroll_panel) {
                Widget* clip_panel = zoom_panel
                    ? static_cast<Widget*>(zoom_panel)
                    : static_cast<Widget*>(scroll_panel);
                // Get the scroll panel's absolute position and size
                Vector2i panel_pos = clip_panel->absolute_position();
                Vector2i panel_size = clip_panel->size();

                // Compute the visible bounds of the focused widget within the scroll panel
                int x_min = std::max(pos.x(), panel_pos.x());
                int y_min = std::max(pos.y(), panel_pos.y());
                int x_max = std::min(pos.x() + size.x(), panel_pos.x() + panel_size.x());
                int y_max = std::min(pos.y() + size.y(), panel_pos.y() + panel_size.y());

                // Adjust position and size for the focus border
                pos.x() = x_min - 2;
                pos.y() = y_min - 2;
                size.x() = x_max - x_min + 4;
                size.y() = y_max - y_min + 4;

                // Only draw if the adjusted size is positive
                if (size.x() > 0 && size.y() > 0) {
                    nvgBeginPath(m_nvg_context);
                    nvgRect(m_nvg_context, pos.x(), pos.y(), size.x(), size.y());
                    nvgStrokeColor(m_nvg_context, nvgRGBA(0, 255, 255, 128)); // Light cyan
                    nvgStrokeWidth(m_nvg_context, 2.0f);
                    nvgStroke(m_nvg_context);
                }
            } else {
                // Draw the border as usual if not in a scroll panel
                nvgBeginPath(m_nvg_context);
                nvgRect(m_nvg_context, pos.x() - 2, pos.y() - 2, size.x() + 4, size.y() + 4);
                nvgStrokeColor(m_nvg_context, nvgRGBA(0, 255, 255, 128)); // Light cyan
                nvgStrokeWidth(m_nvg_context, 2.0f);
                nvgStroke(m_nvg_context);
            }
        }
    }
		


    double elapsed = glfwGetTime() - m_last_interaction;

    if (elapsed > 0.5f) {
        /* Draw tooltips */
        const Widget* widget = find_widget(m_mouse_pos);
        if (widget && !widget->tooltip().empty()) {
            int tooltip_width = 150;

            float bounds[4];
            nvgFontFace(m_nvg_context, "sans");
            nvgFontSize(m_nvg_context, 15.0f);
            nvgTextAlign(m_nvg_context, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgTextLineHeight(m_nvg_context, 1.1f);
            Vector2i pos = widget->absolute_position() +
                Vector2i(widget->width() / 2, widget->height() + 10);

            nvgTextBounds(m_nvg_context, pos.x(), pos.y(),
                widget->tooltip().c_str(), nullptr, bounds);

            int h = (bounds[2] - bounds[0]) / 2;
            if (h > tooltip_width / 2) {
                nvgTextAlign(m_nvg_context, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
                nvgTextBoxBounds(m_nvg_context, pos.x(), pos.y(), tooltip_width,
                    widget->tooltip().c_str(), nullptr, bounds);

                h = (bounds[2] - bounds[0]) / 2;
            }
            int shift = 0;

            if (pos.x() - h - 8 < 0) {
                /* Keep tooltips on screen */
                shift = pos.x() - h - 8;
                pos.x() -= shift;
                bounds[0] -= shift;
                bounds[2] -= shift;
            }

            nvgGlobalAlpha(m_nvg_context,
                std::min(1.0, 2 * (elapsed - 0.5f)) * 0.8);

            nvgBeginPath(m_nvg_context);
            nvgFillColor(m_nvg_context, Color(0, 255));
            nvgRoundedRect(m_nvg_context, bounds[0] - 4 - h, bounds[1] - 4,
                (int)(bounds[2] - bounds[0]) + 8,
                (int)(bounds[3] - bounds[1]) + 8, 3);

            int px = (int)((bounds[2] + bounds[0]) / 2) - h + shift;
            nvgMoveTo(m_nvg_context, px, bounds[1] - 10);
            nvgLineTo(m_nvg_context, px + 7, bounds[1] + 1);
            nvgLineTo(m_nvg_context, px - 7, bounds[1] + 1);
            nvgFill(m_nvg_context);

            nvgFillColor(m_nvg_context, Color(255, 255));
            nvgFontBlur(m_nvg_context, 0.0f);
            nvgTextBox(m_nvg_context, pos.x() - h, pos.y(), tooltip_width,
                widget->tooltip().c_str(), nullptr);
        }
    }

    nvgEndFrame(m_nvg_context);
}

/*bool Screen::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (m_focus_path.size() > 0) {
        for (int Cnt = m_focus_path.size() - 2; Cnt >= 0; Cnt--)// Don't use iterators because we might change focus during key event
            if (m_focus_path[Cnt]->focused() && m_focus_path[Cnt]->keyboard_event(key, scancode, action, modifiers))
                return true;
    }

    return false;
}*/

// Helper function to collect focusable widgets (TextBox, CheckBox, ComboBox, Dropdown)
void Screen::collect_focusable_widgets(Widget* root, std::vector<Widget*>& focusable, Widget* stop_at) {
    if (!root || !root->visible() || !root->enabled()) return;

    // Check if the widget is focusable
    if (dynamic_cast<TextBox*>(root) || dynamic_cast<CheckBox*>(root) ||
        dynamic_cast<ComboBox*>(root) || dynamic_cast<Dropdown*>(root) ||
		dynamic_cast<TextArea*>(root) || dynamic_cast<Button*>(root)) {
        focusable.push_back(root);
    }

    // Stop recursion if we hit the stop_at widget (e.g., a different window)
    if (root == stop_at) return;

    // Recurse through children
    for (Widget* child : root->children()) {
        collect_focusable_widgets(child, focusable, stop_at);
    }
}

bool Screen::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (m_focus_path.size() > 0) {
#if defined(_DEBUG) || !defined(NDEBUG)
        if (key == GLFW_KEY_D && (modifiers & GLFW_MOD_CONTROL) && (modifiers & GLFW_MOD_SHIFT) && action == GLFW_PRESS) {
            printf("=== Window draw times ===\n");
            for (Widget* w : m_children) {
                if (auto* win = dynamic_cast<Window*>(w)) {
                    printf("  %s: %.3f ms\n", win->title().c_str(), win->m_last_drawtime_ms);
                }
            }
            double now = glfwGetTime();
            double wall_secs = (m_redraw_epoch > 0.0) ? (now - m_redraw_epoch) : 0.0;
            printf("=== Screen redraw stats (%d frames in %.1f s) ===\n", m_redraw_count, wall_secs);
            if (m_redraw_count > 0 && wall_secs > 0.0) {
                float avg = (float)(m_redraw_accum_ms / m_redraw_count);
                float fps = (float)(m_redraw_count / wall_secs); // wall-clock fps
                printf("  last=%.2f ms  avg=%.2f ms  min=%.2f ms  max=%.2f ms  ~%.1f fps\n",
                       m_last_redraw_ms, avg, m_redraw_min_ms, m_redraw_max_ms, fps);
            }
            // Reset accumulators; record wall time so next dump can compute fps.
            m_redraw_accum_ms = 0.0;
            m_redraw_count    = 0;
            m_redraw_min_ms   = 1e9f;
            m_redraw_max_ms   = 0.0f;
            m_redraw_epoch    = now;
            return true;
        }
#endif
        // First, try to handle the key event in the current focus path
        for (int Cnt = m_focus_path.size() - 2; Cnt >= 0; Cnt--) {
            if (m_focus_path[Cnt]->focused() && m_focus_path[Cnt]->keyboard_event(key, scancode, action, modifiers))
                return true;
        }

        // Handle Tab key for focus navigation
        if (key == GLFW_KEY_TAB && action == GLFW_PRESS) {
            // Find the current window or popup in the focus path
            Widget* current_container = nullptr;
            for (auto it = m_focus_path.rbegin(); it != m_focus_path.rend(); ++it) {
                if (dynamic_cast<Window*>(*it) || dynamic_cast<Popup*>(*it)) {
                    current_container = *it;
                    break;
                }
            }

            // If no container found, use the screen itself
            if (!current_container) {
                current_container = this;
            }

            // Collect all focusable widgets in the current container
            std::vector<Widget*> focusable_widgets;
            collect_focusable_widgets(current_container, focusable_widgets, nullptr);
            //printf("Found %lu focusable widgets in container: %s\n", focusable_widgets.size(), current_container->id().c_str());

            // If no focusable widgets, return false
            if (focusable_widgets.empty()) {
                //printf("No focusable widgets found\n");
                return false;
            }

            // Find the next or previous focusable widget based on Shift modifier
            Widget* current_focused = m_focus_path.empty() ? nullptr : m_focus_path.front();
            Widget* next_focused = nullptr;
            bool is_shift_tab = modifiers & GLFW_MOD_SHIFT;

            if (!current_focused) {
                // No current focus, select the first (Tab) or last (Shift+Tab) focusable widget
                next_focused = is_shift_tab ? focusable_widgets.back() : focusable_widgets.front();
                //printf("No current focus, selecting %s widget: %s\n", is_shift_tab ? "last" : "first", next_focused->id().c_str());
            } else {
                //printf("Current focused widget: %s\n", current_focused->id().c_str());
                // Find the current focused widget in the list
                auto it = std::find(focusable_widgets.begin(), focusable_widgets.end(), current_focused);
                if (it != focusable_widgets.end()) {
                    if (is_shift_tab) {
                        // Shift+Tab: Move to the previous widget, wrapping to the last if at the beginning
                        if (it != focusable_widgets.begin()) {
                            next_focused = *(it - 1);
                            //printf("Found previous widget: %s\n", next_focused->id().c_str());
                        } else {
                            next_focused = focusable_widgets.back();
                            //printf("At beginning, wrapping to last widget: %s\n", next_focused->id().c_str());
                        }
                    } else {
                        // Tab: Move to the next widget, wrapping to the first if at the end
                        if (it + 1 != focusable_widgets.end()) {
                            next_focused = *(it + 1);
                            //printf("Found next widget: %s\n", next_focused->id().c_str());
                        } else {
                            next_focused = focusable_widgets.front();
                            //printf("At end, wrapping to first widget: %s\n", next_focused->id().c_str());
                        }
                    }
                } else {
                    // Current focused widget not found (e.g., not focusable), select first (Tab) or last (Shift+Tab)
                    next_focused = is_shift_tab ? focusable_widgets.back() : focusable_widgets.front();
                    //printf("Current widget not focusable, selecting %s widget: %s\n", is_shift_tab ? "last" : "first", next_focused->id().c_str());
                }
            }

            // Update focus to the next or previous widget
            if (next_focused) {
                update_focus(next_focused);
                m_redraw = true;
                //printf("Focus set to: %s\n", next_focused->id().c_str());
                return true;
            }
        }
    }

    return false;
}

bool Screen::keyboard_character_event(unsigned int codepoint) {
    if (m_focus_path.size() > 0) {
        for (int Cnt = m_focus_path.size() - 2; Cnt >= 0; Cnt--)// Don't use iterators because we might change focus during key event
            if (m_focus_path[Cnt]->focused() && m_focus_path[Cnt]->keyboard_character_event(codepoint))
                return true;
    }
    return false;
}

bool Screen::resize_event(const Vector2i& size) {
    if (m_resize_callback)
        m_resize_callback(size);
    m_redraw = true;
    draw_all();
    return true;
}

void Screen::redraw() {
    if (!m_redraw) {
        m_redraw = true;
#if !defined(EMSCRIPTEN)
        glfwPostEmptyEvent();
#endif
    }
}

void Screen::cursor_pos_callback_event(double x, double y) {
    Vector2i p((int)x, (int)y);

#if defined(_WIN32) || defined(__linux__) || defined(EMSCRIPTEN)
    p = Vector2i(Vector2f(p) / m_pixel_ratio);
#endif

    m_last_interaction = glfwGetTime();
    try {
        p -= Vector2i(1, 2);

        bool ret = false;
        if (!m_drag_active) {
            Widget* widget = find_widget(p);
            if (widget != nullptr && widget->cursor() != m_cursor) {
                m_cursor = widget->cursor();
                glfwSetCursor(m_glfw_window, m_cursors[(int)m_cursor]);
            }
        } else {
			ret = m_drag_widget->mouse_drag_event(
				p - m_drag_widget->parent()->absolute_position(), p - m_mouse_pos,
				m_mouse_state, m_modifiers);
        }

        if (!ret)
            ret = mouse_motion_event(p, p - m_mouse_pos, m_mouse_state, m_modifiers);

        m_mouse_pos = p;
        m_redraw |= ret;
    }
    catch (const std::exception& e) {
        std::cerr << "Screen::cursor_pos_callback_event: Caught exception in event handler: " << e.what() << std::endl;
    }
}

// Works great - but doesn't handle popups 
void Screen::mouse_button_callback_event(int button, int action, int modifiers) {
    m_modifiers = modifiers;
    m_last_interaction = glfwGetTime();

#if defined(__APPLE__)
    if (button == GLFW_MOUSE_BUTTON_1 && modifiers == GLFW_MOD_CONTROL)
        button = GLFW_MOUSE_BUTTON_2;
#endif

    try {
        if (m_focus_path.size() > 1) {
            const Window* window =
                dynamic_cast<Window*>(m_focus_path[m_focus_path.size() - 2]);
            if (window && window->modal()) {
                if (!window->contains(m_mouse_pos) && !m_drag_active)
                    return;
            }
        }

        if (action == GLFW_PRESS)
            m_mouse_state |= 1 << button;
        else
            m_mouse_state &= ~(1 << button);

        auto drop_widget = find_widget(m_mouse_pos);
        if (m_drag_active && action == GLFW_RELEASE && drop_widget != m_drag_widget) {
            // Check if m_drag_widget is still valid
            if (m_drag_widget && (m_drag_widget->parent() != nullptr || m_drag_widget == this)) {
                m_redraw |= m_drag_widget->mouse_button_event(
                    m_mouse_pos - m_drag_widget->parent()->absolute_position(), button,
                    false, m_modifiers);
            } else {
                m_drag_active = false;
                m_drag_widget = nullptr;
            }
        }

        if (drop_widget != nullptr && drop_widget->cursor() != m_cursor) {
            m_cursor = drop_widget->cursor();
            glfwSetCursor(m_glfw_window, m_cursors[(int)m_cursor]);
        }

        bool btn12 = button == GLFW_MOUSE_BUTTON_1 || button == GLFW_MOUSE_BUTTON_2;

        if (!m_drag_active && action == GLFW_PRESS && btn12) {
            m_drag_widget = find_widget(m_mouse_pos);
            if (m_drag_widget == this)
                m_drag_widget = nullptr;
            m_drag_active = m_drag_widget != nullptr;
            if (!m_drag_active)
                update_focus(nullptr);
        } else if (m_drag_active && action == GLFW_RELEASE && btn12) {
            m_drag_active = false;
            m_drag_widget = nullptr;
        }
        m_redraw |= mouse_button_event(m_mouse_pos, button, action == GLFW_PRESS, m_modifiers);
        if ((!m_redraw || m_close_popups) && m_popup_visible.size() != 0) {
            int Size = m_popup_visible.size();
            for (int Cnt = 0; Cnt < Size; Cnt++) {
                m_popup_visible.pop_front();
            }
            m_redraw = true;
            m_close_popups = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "Screen::mouse_button_callback_event: Caught exception in event handler: " << e.what() << std::endl;
    }
}

/* // Broken, causes crash during popup cloing
void Screen::mouse_button_callback_event(int button, int action, int modifiers) {
    m_modifiers = modifiers;
    m_last_interaction = glfwGetTime();

#if defined(__APPLE__)
    if (button == GLFW_MOUSE_BUTTON_1 && modifiers == GLFW_MOD_CONTROL)
        button = GLFW_MOUSE_BUTTON_2;
#endif

    try {
        if (m_focus_path.size() > 1) {
            const Window* window =
                dynamic_cast<Window*>(m_focus_path[m_focus_path.size() - 2]);
            if (window && window->modal()) {
                if (!window->contains(m_mouse_pos) && !m_drag_active)
                    return;
            }
        }

        if (action == GLFW_PRESS)
            m_mouse_state |= 1 << button;
        else
            m_mouse_state &= ~(1 << button);

        auto drop_widget = find_widget(m_mouse_pos);
        if (m_drag_active && action == GLFW_RELEASE && drop_widget != m_drag_widget) {
            // Check if m_drag_widget is still valid
            if (m_drag_widget && (m_drag_widget->parent() != nullptr || m_drag_widget == this)) {
                m_redraw |= m_drag_widget->mouse_button_event(
                    m_mouse_pos - m_drag_widget->parent()->absolute_position(), button,
                    false, m_modifiers);
            } else {
                m_drag_active = false;
                m_drag_widget = nullptr;
            }
        }

        if (drop_widget != nullptr && drop_widget->cursor() != m_cursor) {
            m_cursor = drop_widget->cursor();
            glfwSetCursor(m_glfw_window, m_cursors[(int)m_cursor]);
        }

        bool btn12 = button == GLFW_MOUSE_BUTTON_1 || button == GLFW_MOUSE_BUTTON_2;

        // Check if the click is outside all visible popups
        bool click_outside_popup = false;
        if (action == GLFW_PRESS && btn12 && !m_drag_active && !m_popup_visible.empty()) {
            click_outside_popup = true;
			int cnt=0;
            for (const auto& popup : m_popup_visible) {
				printf("Check popup %d - %s\n", cnt++, popup->id().c_str() );
                if (popup->contains(m_mouse_pos)) {
                    click_outside_popup = false;
                    break;
                }
            }
        }

        // Handle popup closure if clicking outside
        if (click_outside_popup) {
			printf("clicke_outsize_popup\n");
            // Defer popup closure to avoid modifying widget hierarchy during event handling
            async([this] {
				printf("Close all popups...count=%ld\n", m_popup_visible.size());
                // Create a copy of m_popup_visible to avoid iterator invalidation
                //std::list<Widget*> popups_to_close = m_popup_visible;
				//std::list< PopupButton*> popups_to_close = m_popup_visible;
				int cnt=0;
                for (auto popup : m_popup_visible) {
					printf("Got popup %d - %s\n", cnt++, popup->id().c_str() );

                    if (PopupMenu* popup_menu = dynamic_cast<PopupMenu*>(popup)) {
						printf("It's a PopupMenu\n");
                        // Update parent Dropdown or MenuBar
                        if (Dropdown* dropdown = dynamic_cast<Dropdown*>(popup_menu->parent())) {
                            dropdown->set_pushed(false); // Updates m_popup_visible via PopupButton
                        }
                        popup_menu->set_visible(false);
                        popup_menu->set_highlighted_index(-1);
                        popup_menu->set_selected_index(-1);
                    } else if (PopupButton* popup_button = dynamic_cast<PopupButton*>(popup)) {
						printf("It's a PopupButton\n");
						popup_button->set_pushed(false); // Updates m_popup_visible
                    } else {
						printf("It's something else...\n");
					}
                }
                m_redraw = true;
				printf("DONE Close all popups...\n");
            });
        }

        if (!m_drag_active && action == GLFW_PRESS && btn12) {
            m_drag_widget = find_widget(m_mouse_pos);
            if (m_drag_widget == this)
                m_drag_widget = nullptr;
            m_drag_active = m_drag_widget != nullptr;
            if (!m_drag_active)
                update_focus(nullptr);
        } else if (m_drag_active && action == GLFW_RELEASE && btn12) {
            m_drag_active = false;
            m_drag_widget = nullptr;
        }
        m_redraw |= mouse_button_event(m_mouse_pos, button, action == GLFW_PRESS, m_modifiers);
        if ((!m_redraw || m_close_popups) && m_popup_visible.size() != 0) {
            int Size = m_popup_visible.size();
            for (int Cnt = 0; Cnt < Size; Cnt++) {
                m_popup_visible.pop_front();
            }
            m_redraw = true;
            m_close_popups = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "Screen::mouse_button_callback_event: Caught exception in event handler: " << e.what() << std::endl;
    }
}
*/

void Screen::key_callback_event(int key, int scancode, int action, int mods) {
    m_last_interaction = glfwGetTime();
    try {
        m_redraw |= keyboard_event(key, scancode, action, mods);
    }
    catch (const std::exception& e) {
        std::cerr << "Screen::key_callback_event: Caught exception in event handler: " << e.what() << std::endl;
    }
}

void Screen::char_callback_event(unsigned int codepoint) {
    m_last_interaction = glfwGetTime();
    try {
        m_redraw |= keyboard_character_event(codepoint);
    }
    catch (const std::exception& e) {
        std::cerr << "Screen::char_callback_event: Caught exception in event handler: " << e.what() << std::endl;
    }
}

void Screen::drop_callback_event(int count, const char** filenames) {
    std::vector<std::string> arg(count);
    for (int i = 0; i < count; ++i)
        arg[i] = filenames[i];
    m_redraw |= drop_event(arg);
}

void Screen::scroll_callback_event(double x, double y) {
    m_last_interaction = glfwGetTime();
    try {
        if (m_focus_path.size() > 1) {
            const Window* window =
                dynamic_cast<Window*>(m_focus_path[m_focus_path.size() - 2]);
            if (window && window->modal()) {
                if (!window->contains(m_mouse_pos))
                    return;
            }
        }
        m_redraw |= scroll_event(m_mouse_pos, Vector2f(x, y));
    }
    catch (const std::exception& e) {
        std::cerr << "Caught exception in event handler: " << e.what() << std::endl;
    }
}

void Screen::resize_callback_event(int, int) {
#if defined(EMSCRIPTEN)
    return;
#endif
    Vector2i fb_size, size;
    glfwGetFramebufferSize(m_glfw_window, &fb_size[0], &fb_size[1]);
    glfwGetWindowSize(m_glfw_window, &size[0], &size[1]);
    if (fb_size == Vector2i(0, 0) || size == Vector2i(0, 0))
        return;
    m_fbsize = fb_size; m_size = size;

#if defined(_WIN32) || defined(__linux__) || defined(EMSCRIPTEN)
    m_size = Vector2i(Vector2f(m_size) / m_pixel_ratio);
#endif

    m_last_interaction = glfwGetTime();

#if defined(NANOGUI_USE_METAL)
    if (m_depth_stencil_texture)
        m_depth_stencil_texture->resize(fb_size);
#endif

    try {
        resize_event(m_size);
    }
    catch (const std::exception& e) {
        std::cerr << "Caught exception in event handler: " << e.what() << std::endl;
    }
    redraw();
}

/*
void Screen::update_focus(Widget* widget) {
    // First, notify all previously focused widgets that they're losing focus
    for (auto w : m_focus_path) {
        if (w != nullptr && w->focused())
            w->focus_event(false);
    }
    
    // Now clear the path and build the new one
    m_focus_path.clear();
    Widget* window = nullptr;
    while (widget) {
        m_focus_path.push_back(widget);
        if (dynamic_cast<Window*>(widget))
            window = widget;
        widget = widget->parent();
    }
    
    // Set focus to the new widgets
    for (auto it = m_focus_path.rbegin(); it != m_focus_path.rend(); ++it)
        (*it)->focus_event(true);
        
    if (window)
        move_window_to_front((Window*)window);
}
*/

/*
void Screen::update_focus(Widget* widget) {
    m_focus_path.clear();
    Widget* window = nullptr;
    while (widget) {
        m_focus_path.push_back(widget);
        if (dynamic_cast<Window*>(widget))
            window = widget;
        widget = widget->parent();
    }
    for (auto it = m_focus_path.rbegin(); it != m_focus_path.rend(); ++it)
        (*it)->focus_event(true);
    if (window)
        move_window_to_front((Window*)window);
}
*/

void Screen::update_focus(Widget* widget) {
	// Clear focus
	if(!widget) {
		m_focus_path.clear();
		return;
	}

    for (auto w : m_focus_path) {
        if (w == nullptr || !w->focused())
            continue;
        w->focus_event(false);
    }
    m_focus_path.clear();
    Widget* window = nullptr;
    while (widget) {
        m_focus_path.push_back(widget);
        if (dynamic_cast<Window*>(widget))
            window = widget;
        widget = widget->parent();
    }
    for (auto it = m_focus_path.rbegin(); it != m_focus_path.rend(); ++it)
        (*it)->focus_event(true);

    if (window)
        move_window_to_front((Window*)window);
}

bool Screen::zoom_callback_event(double magnification, const Vector2i& pos) {
    // First try the focused widget / path
    if (!m_focus_path.empty()) {
        for (auto it = m_focus_path.rbegin(); it != m_focus_path.rend(); ++it) {
            if ((*it)->zoom_event(magnification, pos))
                return true;
        }
    }
    // Fall back to normal widget traversal
    return Widget::zoom_event(magnification, pos);
}

void Screen::dispose_window(Window* window) {
    if (std::find(m_focus_path.begin(), m_focus_path.end(), window) != m_focus_path.end())
        m_focus_path.clear();
    if (m_drag_widget == window)
        m_drag_widget = nullptr;
    remove_child(window);
}

void Screen::center_window(Window* window) {
    if (window->size() == 0) {
        window->set_size(window->preferred_size(m_nvg_context));
        window->perform_layout(m_nvg_context);
    }
    window->set_position((m_size - window->size()) / 2);
}

void Screen::move_window_to_front(Window* window) {
    m_children.erase(std::remove(m_children.begin(), m_children.end(), window), m_children.end());
    m_children.push_back(window);
    /* Brute force topological sort (no problem for a few windows..) */
    bool changed = false;
    do {
        size_t base_index = 0;
        for (size_t index = 0; index < m_children.size(); ++index)
            if (m_children[index] == window)
                base_index = index;
        changed = false;
        for (size_t index = 0; index < m_children.size(); ++index) {
            Popup* pw = dynamic_cast<Popup*>(m_children[index]);
            if (pw && pw->parent_window() == window && index < base_index) {
                move_window_to_front(pw);
                changed = true;
                break;
            }
        }
    } while (changed);
}

void Screen::register_animation(Widget* w) {
    m_active_animations.insert(w);
}

void Screen::unregister_animation(Widget* w) {
    m_active_animations.erase(w);
}

bool Screen::animation_in_progress() const {
    return !m_active_animations.empty();
}

bool Screen::tooltip_fade_in_progress() const {
    double elapsed = glfwGetTime() - m_last_interaction;
    if (elapsed < 0.25f || elapsed > 1.25f)
        return false;
    /* Temporarily increase the frame rate to fade in the tooltip */
    const Widget* widget = find_widget(m_mouse_pos);
    return widget && !widget->tooltip().empty();
}

Texture::PixelFormat Screen::pixel_format() const {
#if defined(NANOGUI_USE_METAL)
    if (!m_float_buffer)
        return Texture::PixelFormat::BGRA;
#endif
    return Texture::PixelFormat::RGBA;
}

Texture::ComponentFormat Screen::component_format() const {
    if (m_float_buffer)
        return Texture::ComponentFormat::Float16;
    else
        return Texture::ComponentFormat::UInt8;
}

#if defined(NANOGUI_USE_METAL)
void* Screen::metal_layer() const {
    return metal_window_layer(glfwGetCocoaWindow(m_glfw_window));
}
#endif

NAMESPACE_END(nanogui)
