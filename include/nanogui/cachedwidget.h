/*
    nanogui/cachedwidget.h -- A widget that caches its rendering result
    as an NVG texture for performance.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/widget.h>

NAMESPACE_BEGIN(nanogui)

/**
 * \class CachedWidget cachedwidget.h nanogui/cachedwidget.h
 *
 * A widget that caches its drawing result as an NVG texture.
 * The cache is rebuilt:
 *   - On size change (automatic).
 *   - On any mouse/keyboard/scroll/focus event passing through the widget.
 *   - When perform_layout() runs.
 *   - On explicit cache_dirty() / cache_redraw(ms) calls.
 *
 * The FBO update happens BEFORE the screen frame begins, so it never
 * disturbs the NanoVG transform/scissor state of ancestor widgets.
 */
class NANOGUI_EXPORT CachedWidget : public Widget {
public:
    CachedWidget(Widget* parent);
    ~CachedWidget() override;

    /// Mark the cache as dirty. It will be rebuilt before the next frame.
    void cache_dirty();

    /**
     * Request a cache redraw with an optional debounce time (in milliseconds).
     * If ms_time > 0, the cache will only be rebuilt after the specified delay.
     */
    void cache_redraw(int ms_time = 0);

    /// Returns whether the cache is currently dirty.
    bool cache_is_dirty() const { return m_cacheDirty; }

    /**
     * Called once per screen frame BEFORE nvgBeginFrame to flush any pending
     * cache updates. Invoked from Screen::draw_widgets().
     */
    static void process_pending_updates(NVGcontext* ctx);

    /* ------------ Widget overrides ----------------- */
    virtual void draw(NVGcontext* ctx) override;
    virtual void perform_layout(NVGcontext* ctx) override;

    virtual bool mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) override;
    virtual bool mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) override;
    virtual bool mouse_enter_event(const Vector2i& p, bool enter) override;
    virtual bool scroll_event(const Vector2i& p, const Vector2f& rel) override;
    virtual bool focus_event(bool focused) override;
    virtual bool keyboard_event(int key, int scancode, int action, int mods) override;
    virtual bool keyboard_character_event(unsigned int codepoint) override;

protected:
    /// Called when the cache needs to be regenerated. Override to customize.
    virtual void draw_cached(NVGcontext* ctx);

    void update_cache(NVGcontext* ctx);
    void delete_cache();

protected:
    void*  m_fbo = nullptr;          ///< NVGLUframebuffer*, hidden behind void*
    int    m_cacheImage = -1;
    bool   m_cacheDirty = true;
    int    m_redrawDelayMs = 0;
    double m_lastRedrawRequest = 0.0;

    Vector2i m_cachedSize{0, 0};
};

NAMESPACE_END(nanogui)
