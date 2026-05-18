/*
    nanogui/fluent.h -- Lightweight fluent-builder wrapper for any widget.

    Usage:

        // Wrap something you already have
        Make(existing_button).tooltip("hi").size({100, 30});

        // Or construct in-place
        auto* btn = Make<Button>(parent, "OK")
                       .pos({10, 20})
                       .size({100, 30})
                       .tooltip("Confirm")
                       .tap([](Button* b) { b->set_flags(Button::ToggleButton); })
                       .get();

    The Fluent<T> wrapper holds a single T* and forwards a curated set of
    Widget-level setters (size/pos/visible/...) so chains read cleanly. For
    derived-specific setters (e.g. Button::set_flags, Slider::set_value), use
    `.tap([&](T* w){ ... })` -- it's a one-line escape hatch that keeps you
    in the chain without needing per-widget specializations of Fluent.

    All operations return Fluent<T>& so the most-derived widget type is
    preserved at every step. Implicit conversion to T* lets you assign the
    result of a chain directly to a typed pointer.
*/

#pragma once

#include <nanogui/widget.h>
#include <utility>

NAMESPACE_BEGIN(nanogui)

template <typename T>
class Fluent {
public:
    explicit Fluent(T* widget) : m_widget(widget) {}

    /// --- common Widget setters -------------------------------------------
    Fluent& size(const Vector2i& v)        { m_widget->set_size(v);        return *this; }
    Fluent& pos(const Vector2i& v)         { m_widget->set_position(v);    return *this; }
    Fluent& fixed_size(const Vector2i& v)  { m_widget->set_fixed_size(v);  return *this; }
    Fluent& min_size(const Vector2i& v)    { m_widget->set_min_size(v);    return *this; }
    Fluent& max_size(const Vector2i& v)    { m_widget->set_max_size(v);    return *this; }
    Fluent& width(int w)                   { m_widget->set_width(w);       return *this; }
    Fluent& height(int h)                  { m_widget->set_height(h);      return *this; }
    Fluent& visible(bool b)                { m_widget->set_visible(b);     return *this; }
    Fluent& enabled(bool b)                { m_widget->set_enabled(b);     return *this; }
    Fluent& tooltip(const std::string& s)  { m_widget->set_tooltip(s);     return *this; }
    Fluent& id(const std::string& s)       { m_widget->set_id(s);          return *this; }
    Fluent& font_size(int fs)              { m_widget->set_font_size(fs);  return *this; }
    Fluent& layout(Layout* l)              { m_widget->set_layout(l);      return *this; }
    Fluent& theme(Theme* t)                { m_widget->set_theme(t);       return *this; }
    Fluent& cursor(Cursor c)               { m_widget->set_cursor(c);      return *this; }

    /// Universal escape hatch -- call anything on the underlying widget.
    /// Example: .tap([](Button* b) { b->set_flags(Button::ToggleButton); })
    template <typename F>
    Fluent& tap(F&& f) { f(m_widget); return *this; }

    /// Unwrap to the underlying typed pointer.
    T* get() const { return m_widget; }
    operator T*() const { return m_widget; }
    T* operator->() const { return m_widget; }
    T& operator*()  const { return *m_widget; }

private:
    T* m_widget;
};

/// Wrap an existing widget pointer in a fluent builder.
template <typename T>
Fluent<T> Make(T* widget) { return Fluent<T>(widget); }

/// Construct a new T in-place and return it wrapped in a fluent builder.
/// Example: Make<Button>(parent, "OK", FA_CHECK).tooltip("go").size({80,30});
template <typename T, typename Arg, typename... Rest>
Fluent<T> Make(Arg&& arg, Rest&&... rest) {
    return Fluent<T>(new T(std::forward<Arg>(arg), std::forward<Rest>(rest)...));
}

NAMESPACE_END(nanogui)
