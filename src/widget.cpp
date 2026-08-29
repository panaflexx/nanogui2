/*
    src/widget.cpp -- Base class of all widgets

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include "nanogui/common.h"
#include "nanogui/vector.h"
#include <exception>
#include <nanogui/widget.h>
#include <nanogui/layout.h>
#include <nanogui/theme.h>
#include <nanogui/window.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <stdexcept>

#define _USE_MATH_DEFINES
#include <cmath>
#include <GLFW/glfw3.h>
#include <unordered_set>
#include <vector>


/* Uncomment the following definition to draw red bounding
   boxes around widgets (useful for debugging drawing code) */

//    #define NANOGUI_SHOW_WIDGET_BOUNDS 1

NAMESPACE_BEGIN(nanogui)

namespace {
    std::unordered_set<Widget*> s_cache_pending;
}

Widget::Widget(Widget* parent)
    : m_parent(nullptr), m_theme(nullptr), m_layout(nullptr),
    m_pos(0), m_size(0), m_min_size(0), m_max_size(0), m_visible(true), m_enabled(true),
    m_focused(false), m_mouse_focus(false), m_tooltip(""), m_font_size(-1.f),
    m_icon_extra_scale(1.f), m_cursor(Cursor::Arrow),
    m_animation_type(AnimationType::None), m_animation_start(-1.0), m_animation_duration(0.5) {
    if (parent)
    {
        DebugName = parent->DebugName;
        parent->add_child(this);
        m_min_size = Vector2i(0, 0);
        // Leave m_max_size at (0,0) = unlimited. Capturing parent->size() here
        // permanently capped children to the parent's size at construction time,
        // so after a RootWindow/Screen grew, flex Stretch could not expand them
        // (and left empty strips that looked like status bars).
        m_max_size = Vector2i(0, 0);
    }
}

Widget::~Widget() {
    if (m_id.length())
        NANOGUI_TRACE("~Widget id=%s debugname=%s",
                      this->m_id.c_str(), this->DebugName.c_str());
    // If this widget is NOT itself the Screen but is attached to one,
    // notify the screen so it can scrub stale pointers (focus path,
    // drag widget, animation registry, etc.). Without this, a dynamically
    // destroyed widget (e.g. a temporary cell editor) leaves a dangling
    // pointer in Screen::m_focus_path and the next focus update will
    // dereference freed memory.
    bool is_screen = dynamic_cast<Screen*>(this) != nullptr;
    // Drop from the pending cache registry and free any retained list.
    s_cache_pending.erase(this);
    delete_draw_cache();

    if (!is_screen) {
        Screen* sc = this->screen();
        if (sc)
            sc->notify_widget_destroyed(this);
    }

    if (std::uncaught_exceptions() > 0) {
        /* If a widget constructor throws an exception, it is immediately
           dealloated but may still be referenced by a parent. Be conservative
           and don't decrease the reference count of children while dispatching
           exceptions. */
        return;
    }
    for (auto child : m_children) {
        if (child) {
            child->m_parent = nullptr; // avoid dangling parent in pending cleanup
            child->dec_ref();
        }
    }
}

void Widget::set_theme(Theme* theme) {
    if (m_theme.get() == theme)
        return;
    m_theme = theme;
    // Palette/metrics may have changed (or theme was nullptr → real theme).
    // Stale display lists would keep the old look until something else dirties.
    if (m_cached)
        cache_dirty();
    for (auto child : m_children)
        child->set_theme(theme);
}

int Widget::font_size() const {
    return (m_font_size < 0 && m_theme) ? m_theme->m_standard_font_size : m_font_size;
}

Vector2i Widget::preferred_size(NVGcontext* ctx) const {
    if (!ctx) return m_size;
    if (m_layout)
        return m_layout->preferred_size(ctx, this);
    else
        return m_size;
}

void Widget::apply_fill_parent() {
    if (!m_fill_parent || !m_parent)
        return;
    // The parent's usable content area starts at `co` (e.g. (0, header)
    // for a Window with a title bar) and ends at parent->size(). The
    // child's position within that content area is `m_pos - co`. We
    // preserve symmetric margins inside the content area, which gives
    //   filled = (parent_size - co) - 2 * (m_pos - co)
    //          = parent_size + co - 2 * m_pos
    const Vector2i &ps = m_parent->size();
    Vector2i co = m_parent->content_offset();
    Vector2i filled(
        std::max(0, ps.x() + co.x() - 2 * m_pos.x()),
        std::max(0, ps.y() + co.y() - 2 * m_pos.y())
    );
    if (filled.x() > 0 && filled.y() > 0)
        m_size = filled;
}

void Widget::perform_layout(NVGcontext* ctx) {
    if (!ctx) return;  // guard against early calls before NVG init
    apply_fill_parent();
    if (m_layout) {
        m_layout->perform_layout(ctx, this);
    }
    else {
        for (auto c : m_children) {
            Vector2i pref = c->preferred_size(ctx), fix = c->fixed_size();
            c->set_size(Vector2i(
                fix[0] ? fix[0] : pref[0],
                fix[1] ? fix[1] : pref[1]
            ));
            c->perform_layout(ctx);
        }
    }
    propagate_cache_dirty();
}

Widget* Widget::find_widget(const Vector2i& p) {
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        Widget* child = *it;
        if (child->visible() && child->contains(p - m_pos))
            return child->find_widget(p - m_pos);
    }
    return contains(p) ? this : nullptr;
}

const Widget* Widget::find_widget(const Vector2i& p) const {
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        Widget* child = *it;
        if (child->visible() && child->contains(p - m_pos))
            return child->find_widget(p - m_pos);
    }
    return contains(p) ? this : nullptr;
}

bool Widget::mouse_button_event(const Vector2i& p, int button, bool down, int modifiers) {
    propagate_cache_dirty();

    Screen* CanICastSreen = dynamic_cast<Screen*>(this);
    bool screen_widget = CanICastSreen != NULL;
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        Widget* child = *it;
        if (child->visible() && child->contains(p - m_pos))
        {
            if (child->mouse_button_event(p - m_pos, button, down, modifiers))
                return true;
            else if (screen_widget)break;// stop the loop if we are on the screen and found the first window the pointer is in
        }
    }
    if (button == GLFW_MOUSE_BUTTON_1 && down && !m_focused)
        request_focus();
    return false;
}

bool Widget::mouse_motion_event(const Vector2i& p, const Vector2i& rel, int button, int modifiers) {
    bool handled = false;

    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        Widget* child = *it;
        if (!child->visible())
            continue;

        bool contained = child->contains(p - m_pos),
            prev_contained = child->contains(p - m_pos - rel);

        if (contained != prev_contained)
            handled |= child->mouse_enter_event(p, contained);

        if (contained || prev_contained)
            handled |= child->mouse_motion_event(p - m_pos, rel, button, modifiers);
    }

    return handled;
}

bool Widget::scroll_event(const Vector2i& p, const Vector2f& rel) {
    propagate_cache_dirty();

    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        Widget* child = *it;
        if (!child->visible())
            continue;
        if (child->contains(p - m_pos) && child->scroll_event(p - m_pos, rel))
            return true;
    }
    return false;
}

bool Widget::zoom_event(double magnification, const Vector2i& pos) {
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        Widget* child = *it;
        if (!child->visible())
            continue;
        Vector2i rel = pos - m_pos;
        if (child->contains(rel) && child->zoom_event(magnification, rel))
            return true;
    }
    return false;
}

bool Widget::mouse_drag_event(const Vector2i& p, const Vector2i& rel, int button, int  modifiers) {
    propagate_cache_dirty();
    Screen* CanICastSreen = dynamic_cast<Screen*>(this);
    if (CanICastSreen != NULL)return false;

    if (parent()->mouse_drag_event(p, rel, button, modifiers))
        return true;

    return false;
}

bool Widget::mouse_enter_event(const Vector2i&, bool enter) {
    propagate_cache_dirty();
    m_mouse_focus = enter;
    return false;
}

bool Widget::focus_event(bool focused) {
    propagate_cache_dirty();

    m_focused = focused;
    return false;
}

bool Widget::keyboard_event(int, int, int, int) {
    propagate_cache_dirty();
    return false;
}

bool Widget::keyboard_character_event(unsigned int) {
    propagate_cache_dirty();
    return false;
}

void Widget::add_child(int index, Widget* widget) {
    assert(index <= child_count());
    m_children.insert(m_children.begin() + index, widget);
    widget->set_parent(this);
    widget->set_theme(m_theme);

    // Always account for the new owner (the list entry).
    widget->inc_ref();

    // If this widget was queued for destruction (from a previous parent), cancel the pending destruction.
    {
        std::lock_guard<std::mutex> lock(g_widgets_to_cleanup_mutex);
        auto it = std::find(g_widgets_to_cleanup.begin(), g_widgets_to_cleanup.end(), widget);
        if (it != g_widgets_to_cleanup.end()) {
            g_widgets_to_cleanup.erase(it);
            // release the extra ref that remove_child added to keep it alive
            widget->dec_ref();
        }
    }
}

void Widget::add_child(Widget* widget) {
    add_child(child_count(), widget);
}

void Widget::remove_child(const Widget* widget) {
    // Immediate removal from the tree (so child_count/iteration see the change)
    m_children.erase(std::remove(m_children.begin(), m_children.end(), widget),
                     m_children.end());
    const_cast<Widget*>(widget)->set_parent(nullptr);

    // Queue the final dec_ref so destructor runs outside the event path
    std::lock_guard<std::mutex> lock(g_widgets_to_cleanup_mutex);
    if (std::find(g_widgets_to_cleanup.begin(), g_widgets_to_cleanup.end(), widget) != g_widgets_to_cleanup.end())
        return;
    widget->inc_ref();
    g_widgets_to_cleanup.push_back(const_cast<Widget*>(widget));
}

void Widget::remove_child_at(int index) {
    if (index < 0 || index >= (int)m_children.size())
        throw std::runtime_error("Widget::remove_child_at(): out of bounds!");
    Widget* widget = m_children[index];
    // Immediate removal from the tree
    m_children.erase(m_children.begin() + index);
    widget->set_parent(nullptr);

    // Queue the final dec_ref
    std::lock_guard<std::mutex> lock(g_widgets_to_cleanup_mutex);
    if (std::find(g_widgets_to_cleanup.begin(), g_widgets_to_cleanup.end(), widget) != g_widgets_to_cleanup.end())
        return;
    widget->inc_ref();
    g_widgets_to_cleanup.push_back(widget);
}

void Widget::reparent(Widget* new_parent) {
    if (m_parent == new_parent)
        return;

    // Remove from current parent (if any) - direct, no cleanup queue
    if (m_parent) {
        auto& siblings = m_parent->m_children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), this),
                       siblings.end());
    }

    // Add to new parent (if any)
    if (new_parent) {
        new_parent->m_children.push_back(this);
        set_theme(new_parent->theme());
    }

    m_parent = new_parent;

    // If this widget was queued for destruction, cancel it
    {
        std::lock_guard<std::mutex> lock(g_widgets_to_cleanup_mutex);
        auto it = std::find(g_widgets_to_cleanup.begin(), g_widgets_to_cleanup.end(), this);
        if (it != g_widgets_to_cleanup.end()) {
            g_widgets_to_cleanup.erase(it);
            dec_ref(); // release the extra ref from remove_child
        }
    }
}

int Widget::child_index(Widget* widget) const {
    auto it = std::find(m_children.begin(), m_children.end(), widget);
    if (it == m_children.end())
        return -1;
    return (int)(it - m_children.begin());
}

Window* Widget::window() {
    Widget* widget = this;
    while (true) {
        if (!widget)
            return nullptr;
        Window* window = dynamic_cast<Window*>(widget);
        if (window)
            return window;
        widget = widget->parent();
    }
}

Screen* Widget::screen() {
    Widget* widget = this;
    int depth = 0;
    while (widget && depth < 1024) {
        Screen* screen = dynamic_cast<Screen*>(widget);
        if (screen)
            return screen;
        widget = widget->parent();
        ++depth;
    }
    return nullptr;
}

const Screen* Widget::screen() const { return const_cast<Widget*>(this)->screen(); }
const Window* Widget::window() const { return const_cast<Widget*>(this)->window(); }

void Widget::request_focus() {
    Widget* widget = this;
    while (widget->parent())
        widget = widget->parent();
    ((Screen*)widget)->update_focus(this);
}

std::pair<bool, float> Widget::get_animation_progress() {
    double current_time = glfwGetTime();
    float progress = -1.0f;
    bool anim_active = m_animation_start >= 0.0;

    if (anim_active) {
        //printf("anim_active TRUE for %s\n", m_id.c_str());
        double elapsed = current_time - m_animation_start;
        if (elapsed >= m_animation_duration) {
            progress = 1.0f;
            end_animation();
            m_animation_start = -1.0;
            if (Screen* scr = screen())
                scr->unregister_animation(this);
            /* NOTE: we intentionally keep m_animation_type set so that a
               subsequent start_animation() call (with the default
               AnimationType::None argument) re-runs the same animation
               the user previously selected. */
        } else {
            progress = static_cast<float>(elapsed / m_animation_duration);
        }
    }

    return {anim_active, progress};
}

void Widget::start_animation(AnimationType type) {
    if (type != AnimationType::None) {
        m_animation_type = type;
    }
    if (m_animation_type != AnimationType::None) {
        m_animation_start = glfwGetTime();

        // Capture the widget's current size so SlideUp / SlideDown have a
        // reference "full" height to interpolate against, and so we can
        // restore it when the animation finishes.
        m_animation_original_size = m_size;
        m_animation_original_min_size = m_min_size;

        if (m_animation_type == AnimationType::SlideDown) {
            // SlideDown grows from collapsed to original; start collapsed.
            m_size.y() = 0;
            m_min_size.y() = 0;
        }
        NANOGUI_TRACE("start animation %0.1f for %s", m_animation_start, m_id.c_str());

        // Register with the owning screen so animation_in_progress() is O(1).
        if (Screen* scr = screen())
            scr->register_animation(this);
    }
}

void Widget::stop_animation() {
    end_animation();
    m_animation_start = -1.0;
    if (Screen* scr = screen())
        scr->unregister_animation(this);
}

void Widget::stop_animations() {
    stop_animation();
}

void Widget::apply_animation_transform(NVGcontext* ctx, float progress) {
    if (progress < 0.0f)
        return;

    // Translate to center, apply transform, translate back
    Vector2f center = Vector2f(m_size) * 0.5f;
    nvgTranslate(ctx, center.x(), center.y());

    switch (m_animation_type) {
        case AnimationType::Sproing: {
            NANOGUI_TRACE("sproing %.2f for %s", progress, m_id.c_str());
            float scale = 1.0f + 0.5f * std::sin(progress * 4.0f * float(M_PI)) * std::exp(-progress * 3.0f);
            nvgScale(ctx, scale, scale);
            break;
        }
        case AnimationType::Warble: {
            float scale = 1.0f + 0.1f * std::sin(progress * 10.0f * float(M_PI));
            nvgScale(ctx, scale, scale);
            break;
        }
        case AnimationType::Rotate: {
            float angle = progress * 2.0f * float(M_PI);
            nvgRotate(ctx, angle);
            break;
        }
        case AnimationType::SlideOpen: {
            nvgTranslate(ctx, (1.0f - progress) * -m_size.x(), 0);
            nvgGlobalAlpha(ctx, progress);
            break;
        }
        case AnimationType::SlideClose: {
            nvgTranslate(ctx, progress * -m_size.x(), 0);
            nvgGlobalAlpha(ctx, 1.0f - progress);
            break;
        }
        case AnimationType::SlideUp: {
            // Height is animated by changing m_size in Widget::draw().
            // Here we just apply the fade-out.
            nvgGlobalAlpha(ctx, 1.0f - progress);
            break;
        }
        case AnimationType::SlideDown: {
            // Height is animated by changing m_size in Widget::draw().
            // Here we just apply the fade-in.
            nvgGlobalAlpha(ctx, progress);
            break;
        }
        default:
            break;
    }

    nvgTranslate(ctx, -center.x(), -center.y());
}

void Widget::end_animation() {
    if (m_animation_type == AnimationType::SlideClose) {
        m_visible = false;
        NANOGUI_TRACE("end animation for %s", m_id.c_str());
    } else if (m_animation_type == AnimationType::SlideUp) {
        // After collapsing, hide the widget but restore its size so it can
        // be shown again later (e.g. via SlideDown) at the right dimensions.
        m_visible = false;
        m_size = m_animation_original_size;
        m_min_size = m_animation_original_min_size;
        NANOGUI_TRACE("end SlideUp animation for %s", m_id.c_str());
    } else if (m_animation_type == AnimationType::SlideDown) {
        // Ensure we end at exactly the original size (avoids rounding drift).
        m_size = m_animation_original_size;
        m_min_size = m_animation_original_min_size;
        NANOGUI_TRACE("end SlideDown animation for %s", m_id.c_str());
    }
    // Subclasses can override for more logic
}


/* ------------------------------------------------------------------ */
/*  Display-list cache                                                */
/* ------------------------------------------------------------------ */

static void cache_register(Widget* w) {
    if (w) s_cache_pending.insert(w);
}

Widget& Widget::set_layout(Layout* layout) {
    m_layout = layout;
    // Containers with a layout are the natural display-list cache roots.
    if (layout)
        set_cached(true);
    return *this;
}

Widget& Widget::set_live(bool live) {
    m_live = live;
    if (live) {
        // Live widgets should not retain their own list either.
        set_cached(false);
        // Ancestor lists must rebuild without this child baked in.
        propagate_cache_dirty();
    }
    return *this;
}

Widget& Widget::set_cached(bool cached) {
    if (m_cached == cached)
        return *this;
    m_cached = cached;
    if (!cached) {
        s_cache_pending.erase(this);
        delete_draw_cache();
        m_cache_dirty = true;
    } else {
        m_cache_dirty = true;
        cache_register(this);
        if (Screen* scr = screen())
            scr->redraw();
    }
    return *this;
}

void Widget::cache_dirty() {
    if (!m_cached)
        return;
    m_cache_dirty = true;
    m_cache_redraw_delay_ms = 0;
    cache_register(this);
    if (Screen* scr = screen())
        scr->redraw();
}

void Widget::propagate_cache_dirty() {
    for (Widget* w = this; w != nullptr; w = w->parent()) {
        if (w->m_cached)
            w->cache_dirty();
    }
}

void Widget::cache_redraw(int ms_time) {
    if (!m_cached)
        return;
    m_cache_redraw_delay_ms = ms_time;
    m_cache_dirty = true;
    m_cache_last_request = glfwGetTime();
    cache_register(this);
    if (Screen* scr = screen())
        scr->redraw();
}

int Widget::cache_packet_count() const {
    return nvgDrawListSize(m_draw_list);
}

void Widget::delete_draw_cache() {
    if (m_draw_list) {
        NVGcontext* ctx = nullptr;
        if (Screen* scr = screen())
            ctx = scr->nvg_context();
        nvgDeleteDrawList(ctx, m_draw_list);
        m_draw_list = nullptr;
    }
    m_cache_size = Vector2i(0, 0);
    m_cache_pixel_ratio = 0.0f;
}

void Widget::draw_cached_content(NVGcontext* ctx) {
    // Record non-live children only. Live children (TextEditor, DataGrid, …)
    // repaint every frame after SubmitDrawList so scroll/selection stay correct
    // without freezing or constantly invalidating the parent list.
    if (m_layout)
        m_layout->draw_table(ctx, this);

    for (auto child : m_children) {
        if (!child->visible() || child->live())
            continue;
#if !defined(NANOGUI_SHOW_WIDGET_BOUNDS)
        nvgSave(ctx);
        nvgIntersectScissor(ctx, child->position().x(), child->position().y(),
                            child->size().x(), child->size().y());
#endif
        child->draw(ctx);
#if !defined(NANOGUI_SHOW_WIDGET_BOUNDS)
        nvgRestore(ctx);
#endif
    }
}


void Widget::draw_live_overlays(NVGcontext* ctx) {
    // Current transform is this widget's parent space (caller translated to parent).
    // Live widgets paint with their m_pos relative to that space (same as Widget::draw).
    for (auto child : m_children) {
        if (!child->visible())
            continue;
#if !defined(NANOGUI_SHOW_WIDGET_BOUNDS)
        nvgSave(ctx);
        nvgIntersectScissor(ctx, child->position().x(), child->position().y(),
                            child->size().x(), child->size().y());
#endif
        if (child->live()) {
            child->draw(ctx);
        } else {
            // Descend so a live grandchild under a non-live intermediate still paints.
            nvgTranslate(ctx, (float)child->position().x(), (float)child->position().y());
            child->draw_live_overlays(ctx);
            nvgTranslate(ctx, -(float)child->position().x(), -(float)child->position().y());
        }
#if !defined(NANOGUI_SHOW_WIDGET_BOUNDS)
        nvgRestore(ctx);
#endif
    }
}


void Widget::update_draw_cache(NVGcontext* ctx) {
    if (!m_cached || !ctx)
        return;
    if (m_size.x() <= 0 || m_size.y() <= 0)
        return;

    float pxRatio = 1.0f;
    if (Screen* scr = screen())
        pxRatio = scr->pixel_ratio();

    if (!m_draw_list) {
        m_draw_list = nvgCreateDrawList(ctx);
        if (!m_draw_list)
            return;
    }

    // Tessellate children once in local coordinates into a retained Path 2 list.
    nvgBeginFrame(ctx, (float)m_size.x(), (float)m_size.y(), pxRatio);
    nvgBeginDisplayList(ctx, m_draw_list);
    draw_cached_content(ctx);
    nvgEndDisplayList(ctx);
    nvgCancelFrame(ctx);

    m_cache_size = m_size;
    m_cache_pixel_ratio = pxRatio;
}

void Widget::process_pending_cache_updates(NVGcontext* ctx) {
    if (s_cache_pending.empty())
        return;

    double now = glfwGetTime();
    std::vector<Widget*> ready;
    ready.reserve(s_cache_pending.size());

    for (auto it = s_cache_pending.begin(); it != s_cache_pending.end(); ) {
        Widget* w = *it;
        if (!w->m_cached) {
            it = s_cache_pending.erase(it);
            continue;
        }
        if (w->m_cache_redraw_delay_ms > 0 &&
            (now - w->m_cache_last_request) * 1000.0 < (double)w->m_cache_redraw_delay_ms) {
            if (Screen* scr = w->screen())
                scr->redraw();
            ++it;
            continue;
        }
        ready.push_back(w);
        it = s_cache_pending.erase(it);
    }

    for (Widget* w : ready) {
        if (w->m_size.x() <= 0 || w->m_size.y() <= 0)
            continue;
        w->update_draw_cache(ctx);
        w->m_cache_dirty = false;
        w->m_cache_redraw_delay_ms = 0;
    }
}

void Widget::draw(NVGcontext* ctx) {
#if defined(NANOGUI_SHOW_WIDGET_BOUNDS)
    nvgStrokeWidth(ctx, 1.0f);
    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x() - 0.5f, m_pos.y() - 0.5f,
        m_size.x() + 1, m_size.y() + 1);
    nvgStrokeColor(ctx, nvgRGBA(255, 0, 0, 255));
    nvgStroke(ctx);
#endif

    if (!m_visible)
        return;

    // Live widgets (scroll views, HTML, virtual lists) must not be tessellated
    // into an ancestor display list: those packets replay at record-time
    // coordinates and scissor, which shows up as content painting across
    // sibling panes. draw_live_overlays() paints them in the real frame.
    if (m_live && nvgIsRecordingDisplayList(ctx))
        return;

    // Apply animation transform for this widget
    auto [anim_active, progress] = get_animation_progress();

    // For SlideUp / SlideDown we drive the widget's *actual* height so that
    // the parent's layout reflows around us, giving a true collapse / expand
    // effect (rather than just a visual scale).
    bool size_anim = anim_active &&
        (m_animation_type == AnimationType::SlideUp ||
         m_animation_type == AnimationType::SlideDown);
    if (size_anim) {
        float t = progress;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        if (progress < 1.0f) {
            int orig_h = m_animation_original_size.y();
            int new_h = (m_animation_type == AnimationType::SlideUp)
                ? (int)(orig_h * (1.0f - t))
                : (int)(orig_h * t);
            m_size.y() = new_h;
            // Keep fixed/min size in sync so the parent layout honors the
            // shrinking/growing height.
            if (m_animation_original_min_size.y() > 0)
                m_min_size.y() = new_h;
        }
        // Reflow siblings every frame (and on the final frame, after
        // end_animation() restored the original size).
        if (m_parent)
            m_parent->perform_layout(ctx);
    }

    int depth0 = nvgStateDepth(ctx);
    nvgSave(ctx);
    /* Failed nvgSave + nvgRestore pops the *parent* scissor/transform and
     * paints this subtree all over the window. Skip rather than corrupt. */
    if (nvgStateDepth(ctx) <= depth0)
        return;

    nvgTranslate(ctx, m_pos.x(), m_pos.y());

    if (anim_active) {
        apply_animation_transform(ctx, progress);
    }

    // Retained display-list path: submit a previously recorded local-space list.
    if (m_cached) {
        float pxRatio = 1.0f;
        if (Screen* scr = screen())
            pxRatio = scr->pixel_ratio();
        if (m_cache_size != m_size || m_cache_pixel_ratio != pxRatio ||
            nvgDrawListNeedsRebuild(ctx, m_draw_list)) {
            m_cache_dirty = true;
            cache_register(this);
            if (Screen* scr = screen())
                scr->redraw();
        }
        if (!m_cache_dirty && m_draw_list && nvgDrawListSize(m_draw_list) > 0) {
            nvgSubmitDrawList(ctx, m_draw_list);
            // Live overlays only in a real frame — never while a parent is
            // recording (those paints would hit GL and then be cancel'd).
            if (!nvgIsRecordingDisplayList(ctx))
                draw_live_overlays(ctx);
            nvgTranslate(ctx, -m_pos.x(), -m_pos.y());
            nvgRestore(ctx);
            return;
        }
        // Dirty / empty: fall through to immediate children draw for this frame.
    }

	// Draw table layout if enabled
	if(layout()) {
		layout()->draw_table(ctx, this);
	}

    // Draw children (their animations are handled in their own draw calls)
    if (!m_children.empty()) {
        const int recording = nvgIsRecordingDisplayList(ctx);
        for (auto child : m_children) {
            if (!child->visible())
                continue;
            if (recording && child->live())
                continue;
        #if !defined(NANOGUI_SHOW_WIDGET_BOUNDS)
            nvgSave(ctx);
            /* Sproing / Warble / Rotate visually grow past the widget's
               static rectangle, so we drop the scissor while they're
               running. Slide animations, on the other hand, depend on the
               parent's scissor to make the widget appear to enter / exit
               from off-screen, so we keep clipping them. */
            bool overflow_anim =
                child->animating() &&
                (child->animation_type() == AnimationType::Sproing ||
                 child->animation_type() == AnimationType::Warble  ||
                 child->animation_type() == AnimationType::Rotate);
            if (overflow_anim) {
                nvgResetScissor(ctx);
            } else {
                nvgIntersectScissor(ctx, child->m_pos.x(), child->m_pos.y(),
                    child->m_size.x(), child->m_size.y());
            }
        #endif

            child->draw(ctx);

        #if !defined(NANOGUI_SHOW_WIDGET_BOUNDS)
            nvgRestore(ctx);
        #endif
        }
    }

    nvgTranslate(ctx, -m_pos.x(), -m_pos.y());
    nvgRestore(ctx);
}

NAMESPACE_END(nanogui)
