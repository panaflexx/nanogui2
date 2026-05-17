/*
    src/cachedwidget.cpp -- Implementation of CachedWidget

    Renders an offscreen NVG/FBO cache of the widget's children for
    performance. Cache updates are deferred to BEFORE the next screen
    frame begins so they don't disrupt ancestor NanoVG state
    (transforms, scissors, etc.).

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/cachedwidget.h>
#include <nanogui/screen.h>
#include <nanogui/opengl.h>
#include <GLFW/glfw3.h>

// Bring in NanoVG FBO helpers. The implementation lives in nanovg.c
// (compiled with C linkage), so wrap in extern "C".
#if defined(NANOGUI_USE_OPENGL)
#  define NANOVG_GL3
#elif defined(NANOGUI_USE_GLES)
#  define NANOVG_GLES2
#endif
extern "C" {
#include <nanovg_gl_utils.h>
}

#include <unordered_set>

NAMESPACE_BEGIN(nanogui)

/* ------------------------------------------------------------------ */
/*  Pending cache update registry                                     */
/* ------------------------------------------------------------------ */

namespace {
    // Set of CachedWidgets whose cache needs to be (re)built before the
    // next screen frame begins. Processed in process_pending_updates().
    std::unordered_set<CachedWidget*> s_pending;
}

/* ------------------------------------------------------------------ */
/*  Construction / destruction                                        */
/* ------------------------------------------------------------------ */

CachedWidget::CachedWidget(Widget* parent) : Widget(parent) {
    DebugName = m_parent ? (m_parent->DebugName + ",Cached") : "CachedWidget";
    m_cacheDirty = true;
    s_pending.insert(this);
}

CachedWidget::~CachedWidget() {
    s_pending.erase(this);
    delete_cache();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void CachedWidget::cache_dirty() {
    m_cacheDirty = true;
    m_redrawDelayMs = 0;
    s_pending.insert(this);
    if (Screen* scr = screen())
        scr->redraw();
}

void CachedWidget::cache_redraw(int ms_time) {
    m_redrawDelayMs = ms_time;
    m_cacheDirty = true;
    m_lastRedrawRequest = glfwGetTime();
    s_pending.insert(this);

    if (Screen* scr = screen())
        scr->redraw();
}

void CachedWidget::perform_layout(NVGcontext* ctx) {
    Vector2i oldSize = m_size;
    Widget::perform_layout(ctx);
    if (m_size != oldSize) {
        cache_dirty();
    }
}

/* ------------------------------------------------------------------ */
/*  Event overrides -- invalidate cache when something might change   */
/* ------------------------------------------------------------------ */

bool CachedWidget::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    cache_dirty();
    return Widget::mouse_button_event(p, button, down, modifiers);
}

bool CachedWidget::mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    cache_dirty();
    return Widget::mouse_drag_event(p, rel, button, modifiers);
}

bool CachedWidget::mouse_enter_event(const Vector2i& p, bool enter) {
    cache_dirty();
    return Widget::mouse_enter_event(p, enter);
}

bool CachedWidget::scroll_event(const Vector2i& p, const Vector2f& rel) {
    cache_dirty();
    return Widget::scroll_event(p, rel);
}

bool CachedWidget::focus_event(bool focused) {
    cache_dirty();
    return Widget::focus_event(focused);
}

bool CachedWidget::keyboard_event(int key, int scancode, int action, int mods) {
    cache_dirty();
    return Widget::keyboard_event(key, scancode, action, mods);
}

bool CachedWidget::keyboard_character_event(unsigned int codepoint) {
    cache_dirty();
    return Widget::keyboard_character_event(codepoint);
}

/* ------------------------------------------------------------------ */
/*  Cache management                                                  */
/* ------------------------------------------------------------------ */

void CachedWidget::delete_cache() {
    if (m_fbo) {
        nvgluDeleteFramebuffer(static_cast<NVGLUframebuffer*>(m_fbo));
        m_fbo = nullptr;
    }
    // nvgluDeleteFramebuffer also frees the associated image
    m_cacheImage = -1;
}

void CachedWidget::draw(NVGcontext* ctx) {
    if (m_size.x() <= 0 || m_size.y() <= 0)
        return;

    // Detect size mismatch -> request a rebuild for the next frame.
    if (m_cachedSize != m_size) {
        m_cacheDirty = true;
        s_pending.insert(this);
        if (Screen* scr = screen())
            scr->redraw();
    }

    // If the cache is ready, use the fast path.
    if (!m_cacheDirty && m_cacheImage != -1) {
        NVGpaint paint = nvgImagePattern(ctx,
            0, 0,
            (float)m_size.x(), (float)m_size.y(),
            0.0f, m_cacheImage, 1.0f);

        nvgSave(ctx);
        nvgTranslate(ctx, m_pos.x(), m_pos.y());
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, m_size.x(), m_size.y());
        nvgFillPaint(ctx, paint);
        nvgFill(ctx);
        nvgRestore(ctx);
        return;
    }

    // Fallback path: cache is dirty (or not yet built). Draw children
    // directly through the current NVG state so the visuals remain correct
    // for this frame. The cache will be rebuilt before the next frame.
    nvgSave(ctx);
    nvgTranslate(ctx, m_pos.x(), m_pos.y());
    draw_cached(ctx);
    nvgRestore(ctx);
}

void CachedWidget::draw_cached(NVGcontext* ctx) {
    // Children are at this widget's local (0,0) coordinate space.
    for (auto child : m_children) {
        if (child->visible())
            child->draw(ctx);
    }
}

void CachedWidget::update_cache(NVGcontext* ctx) {
    if (m_size.x() <= 0 || m_size.y() <= 0)
        return;

    int w = m_size.x();
    int h = m_size.y();

    // (Re)create the FBO if needed.
    if (m_fbo == nullptr || m_cachedSize != m_size) {
        if (m_fbo) {
            nvgluDeleteFramebuffer(static_cast<NVGLUframebuffer*>(m_fbo));
            m_fbo = nullptr;
            m_cacheImage = -1;
        }

        NVGLUframebuffer* fb = nvgluCreateFramebuffer(
            ctx, w, h,
            NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY | NVG_IMAGE_FLIPY
        );


        if (!fb)
            return;

        m_fbo = fb;
        m_cacheImage = fb->image;
        m_cachedSize = m_size;
    }

    NVGLUframebuffer* fb = static_cast<NVGLUframebuffer*>(m_fbo);

    // Save the current default framebuffer and viewport so we can restore.
    GLint defaultFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &defaultFBO);
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);

    // Bind FBO and render the children into it. We are OUTSIDE any
    // active screen NVG frame at this point (we are called BEFORE the
    // screen's nvgBeginFrame), so we can freely begin/end a frame here
    // without disturbing ancestor state.
    nvgluBindFramebuffer(fb);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(ctx, (float)w, (float)h, 1.0f);
    draw_cached(ctx);
    nvgEndFrame(ctx);

    // Restore default FBO + viewport.
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFBO);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
}

/* ------------------------------------------------------------------ */
/*  Pre-frame processing of pending updates                           */
/* ------------------------------------------------------------------ */

void CachedWidget::process_pending_updates(NVGcontext* ctx) {
    if (s_pending.empty())
        return;

    double now = glfwGetTime();

    // Take a snapshot since update_cache may indirectly insert/erase
    // entries (e.g. via perform_layout -> cache_dirty).
    std::vector<CachedWidget*> ready;
    ready.reserve(s_pending.size());

    for (auto it = s_pending.begin(); it != s_pending.end(); ) {
        CachedWidget* w = *it;

        // Honour debounce/delay timing
        if (w->m_redrawDelayMs > 0 &&
            (now - w->m_lastRedrawRequest) * 1000.0 < (double)w->m_redrawDelayMs)
        {
            // Not yet — leave it pending and request another frame.
            if (Screen* scr = w->screen())
                scr->redraw();
            ++it;
            continue;
        }

        ready.push_back(w);
        it = s_pending.erase(it);
    }

    for (CachedWidget* w : ready) {
        if (w->m_size.x() <= 0 || w->m_size.y() <= 0)
            continue;
        w->update_cache(ctx);
        w->m_cacheDirty = false;
        w->m_redrawDelayMs = 0;
    }
}

NAMESPACE_END(nanogui)
