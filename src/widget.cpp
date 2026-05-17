/*
    src/widget.cpp -- Base class of all widgets

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/widget.h>
#include <nanogui/layout.h>
#include <nanogui/theme.h>
#include <nanogui/window.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>

#define _USE_MATH_DEFINES
#include <cmath>
#include <GLFW/glfw3.h>

/* Uncomment the following definition to draw red bounding
   boxes around widgets (useful for debugging drawing code) */

//    #define NANOGUI_SHOW_WIDGET_BOUNDS 1

NAMESPACE_BEGIN(nanogui)

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
        m_min_size = Vector2i(0,0);
        m_max_size = parent->size();
    }
}

Widget::~Widget() {
#define  DEBUG
#ifdef DEBUG
    #warning DEBUG ENABLED
    if(m_id.length())
        printf("~Widget id=%s debugname=%s\n", 
        this->m_id.c_str(),
        this->DebugName.c_str()
    );
#endif
    Screen* CanICastSreen = dynamic_cast<Screen*>(this);
    bool screen_widget = CanICastSreen != NULL;
    if (screen_widget) {
        this->screen()->notify_widget_destroyed(this);
    }

    if (std::uncaught_exceptions() > 0) {
        /* If a widget constructor throws an exception, it is immediately
           dealloated but may still be referenced by a parent. Be conservative
           and don't decrease the reference count of children while dispatching
           exceptions. */
        return;
    }
    for (auto child : m_children) {
        if (child)
            child->dec_ref();
    }
}

void Widget::set_theme(Theme* theme) {
    if (m_theme.get() == theme)
        return;
    m_theme = theme;
    for (auto child : m_children)
        child->set_theme(theme);
}

int Widget::font_size() const {
    return (m_font_size < 0 && m_theme) ? m_theme->m_standard_font_size : m_font_size;
}

Vector2i Widget::preferred_size(NVGcontext* ctx) const {
    if (m_layout)
        return m_layout->preferred_size(ctx, this);
    else
        return m_size;
}

void Widget::perform_layout(NVGcontext* ctx) {
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
    Screen* CanICastSreen = dynamic_cast<Screen*>(this);
    if (CanICastSreen != NULL)return false;

    if (parent()->mouse_drag_event(p, rel, button, modifiers))
        return true;

    return false;
}

bool Widget::mouse_enter_event(const Vector2i&, bool enter) {
    m_mouse_focus = enter;
    return false;
}

bool Widget::focus_event(bool focused) {
    m_focused = focused;
    return false;
}

bool Widget::keyboard_event(int, int, int, int) {
    return false;
}

bool Widget::keyboard_character_event(unsigned int) {
    return false;
}

void Widget::add_child(int index, Widget* widget) {
    assert(index <= child_count());
    m_children.insert(m_children.begin() + index, widget);
    widget->inc_ref();
    widget->set_parent(this);
    widget->set_theme(m_theme);
}

void Widget::add_child(Widget* widget) {
    add_child(child_count(), widget);
}

void Widget::remove_child(const Widget* widget) {
    size_t child_count = m_children.size();
    m_children.erase(std::remove(m_children.begin(), m_children.end(), widget),
        m_children.end());
    if (m_children.size() == child_count)
        throw std::runtime_error("Widget::remove_child(): widget not found!");
    widget->dec_ref();
}

void Widget::remove_child_at(int index) {
    if (index < 0 || index >= (int)m_children.size())
        throw std::runtime_error("Widget::remove_child_at(): out of bounds!");
    Widget* widget = m_children[index];
    m_children.erase(m_children.begin() + index);
    widget->dec_ref();
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
    while (true) {
        if (!widget)
            return nullptr;
        Screen* screen = dynamic_cast<Screen*>(widget);
        if (screen)
            return screen;
        widget = widget->parent();
    }
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
        printf("anim_active TRUE for %s\n", m_id.c_str());
        double elapsed = current_time - m_animation_start;
        if (elapsed >= m_animation_duration) {
            progress = 1.0f;
            end_animation();
            m_animation_start = -1.0;
            m_animation_type = AnimationType::None;
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
        printf("Start animation %0.1f for %s\n", m_animation_start, m_id.c_str());
    }
}

void Widget::apply_animation_transform(NVGcontext* ctx, float progress) {
    if (progress < 0.0f)
        return;

    // Translate to center, apply transform, translate back
    Vector2f center = Vector2f(m_size) * 0.5f;
    nvgTranslate(ctx, center.x(), center.y());

    switch (m_animation_type) {
        case AnimationType::Sproing: {
            printf("sproing %.2f for %s\n", progress, m_id.c_str());
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
        default:
            break;
    }

    nvgTranslate(ctx, -center.x(), -center.y());
}

void Widget::end_animation() {
    if (m_animation_type == AnimationType::SlideClose) {
        m_visible = false;
        printf("End animation for %s\n", m_id.c_str());
    }
    // Subclasses can override for more logic
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

    nvgSave(ctx);
    nvgTranslate(ctx, m_pos.x(), m_pos.y());

    // Apply animation transform for this widget
    auto [anim_active, progress] = get_animation_progress();
    if (anim_active) {
        apply_animation_transform(ctx, progress);
    }

	// Draw table layout if enabled
	if(layout()) { 
		layout()->draw_table(ctx, this);
	}

    // Draw children (their animations are handled in their own draw calls)
    if (!m_children.empty()) {
        for (auto child : m_children) {
            if (!child->visible())
                continue;
        #if !defined(NANOGUI_SHOW_WIDGET_BOUNDS)
            nvgSave(ctx);
            nvgIntersectScissor(ctx, child->m_pos.x(), child->m_pos.y(),
                child->m_size.x(), child->m_size.y());
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
