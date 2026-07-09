/*
    src/cachedwidget.cpp -- Implementation of CachedWidget

    Caches an NVG draw list (display list) of the widget's children for
    performance. Cache updates are deferred to BEFORE the next screen
    frame begins so they don't disrupt ancestor NanoVG state
    (transforms, scissors, etc.).

    Geometry is recorded in widget-local coordinates and submitted under
    nvgTranslate(m_pos) so parent clipping/transforms still work.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/cachedwidget.h>
#include <nanogui/screen.h>
#include <nanogui/opengl.h>
#include <GLFW/glfw3.h>

#include <unordered_set>
#include <vector>

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

int CachedWidget::cache_packet_count() const {
    return nvgDrawListSize(m_drawList);
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
    if (m_drawList) {
        // Context may already be gone during teardown; nvgDeleteDrawList
        // tolerates a null context.
        NVGcontext* ctx = nullptr;
        if (Screen* scr = screen())
            ctx = scr->nvg_context();
        nvgDeleteDrawList(ctx, m_drawList);
        m_drawList = nullptr;
    }
    m_cachedSize = Vector2i(0, 0);
    m_cachedPixelRatio = 0.0f;
}

void CachedWidget::draw(NVGcontext* ctx) {
    if (m_size.x() <= 0 || m_size.y() <= 0)
        return;

    float pxRatio = 1.0f;
    if (Screen* scr = screen())
        pxRatio = scr->pixel_ratio();

    // Detect size / DPI mismatch -> request a rebuild for the next frame.
    if (m_cachedSize != m_size || m_cachedPixelRatio != pxRatio ||
        nvgDrawListNeedsRebuild(ctx, m_drawList)) {
        m_cacheDirty = true;
        s_pending.insert(this);
        if (Screen* scr = screen())
            scr->redraw();
    }

    // Fast path: submit the retained local-space draw list under our position.
    if (!m_cacheDirty && m_drawList && nvgDrawListSize(m_drawList) > 0) {
        nvgSave(ctx);
        nvgTranslate(ctx, (float)m_pos.x(), (float)m_pos.y());
        nvgSubmitDrawList(ctx, m_drawList);
        nvgRestore(ctx);
        return;
    }

    // Fallback: cache is dirty (or not yet built). Draw children directly
    // through the current NVG state so this frame stays correct.
    nvgSave(ctx);
    nvgTranslate(ctx, (float)m_pos.x(), (float)m_pos.y());
    draw_cached(ctx);
    nvgRestore(ctx);
}

void CachedWidget::draw_cached(NVGcontext* ctx) {
    // Children are at this widget's local (0,0) coordinate space.
    // Their draw() implementations use m_pos relative to this parent, so we
    // must NOT pre-translate by CachedWidget::m_pos here.
    for (auto child : m_children) {
        if (child->visible())
            child->draw(ctx);
    }
}

void CachedWidget::update_cache(NVGcontext* ctx) {
    if (m_size.x() <= 0 || m_size.y() <= 0)
        return;
    if (!ctx)
        return;

    float pxRatio = 1.0f;
    if (Screen* scr = screen())
        pxRatio = scr->pixel_ratio();

    if (!m_drawList) {
        m_drawList = nvgCreateDrawList(ctx);
        if (!m_drawList)
            return;
    }

    // Record children in local widget space. nvgBeginFrame establishes
    // devicePixelRatio/fringe for tessellation; Cancel discards any
    // accidental per-frame IR from the recording side.
    nvgBeginFrame(ctx, (float)m_size.x(), (float)m_size.y(), pxRatio);
    nvgBeginDisplayList(ctx, m_drawList);
    draw_cached(ctx);
    nvgEndDisplayList(ctx);
    nvgCancelFrame(ctx);

    m_cachedSize = m_size;
    m_cachedPixelRatio = pxRatio;
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
