/*
    nanogui/fluent.h -- Lightweight fluent builder for NanoGUI2 widgets.

    CORRECT USAGE:
        ButtonDemoWindow->add<Button>("Plain button")
            .tooltip("short tooltip")
            .callback([] { std::cout << "pushed!" << std::endl; });
*/

#pragma once

#include <nanogui/common.h>
#include <nanogui/vector.h>
// Forward declaration only (breaks circular include)
NAMESPACE_BEGIN(nanogui)
class Widget;
NAMESPACE_END(nanogui)

NAMESPACE_BEGIN(nanogui)

template <typename T>
class Fluent {
public:
    explicit Fluent(T* widget) : m_widget(widget) {}

    // Common Widget setters
    Fluent& size(const Vector2i& v)          { m_widget->set_size(v);        return *this; }
    Fluent& pos(const Vector2i& v)           { m_widget->set_position(v);    return *this; }
    Fluent& fixed_size(const Vector2i& v)    { m_widget->set_fixed_size(v);  return *this; }
    Fluent& min_size(const Vector2i& v)      { m_widget->set_min_size(v);    return *this; }
    Fluent& max_size(const Vector2i& v)      { m_widget->set_max_size(v);    return *this; }
    Fluent& width(int w)                     { m_widget->set_width(w);       return *this; }
    Fluent& height(int h)                    { m_widget->set_height(h);      return *this; }
    Fluent& visible(bool b)                  { m_widget->set_visible(b);     return *this; }
    Fluent& enabled(bool b)                  { m_widget->set_enabled(b);     return *this; }
    Fluent& tooltip(const std::string& s)    { m_widget->set_tooltip(s);     return *this; }
    Fluent& id(const std::string& s)         { m_widget->set_id(s);          return *this; }
    Fluent& font_size(int fs)                { m_widget->set_font_size(fs);  return *this; }
    Fluent& layout(Layout* l)                { m_widget->set_layout(l);      return *this; }
    Fluent& theme(Theme* t)                  { m_widget->set_theme(t);       return *this; }
    Fluent& cursor(Cursor c)                 { m_widget->set_cursor(c);      return *this; }

    // Most common widget-specific setters
    template <typename Callback>
    Fluent& callback(Callback&& cb) {
        m_widget->set_callback(std::forward<Callback>(cb));
        return *this;
    }

    template <typename Callback>
    Fluent& change_callback(Callback&& cb) {
        m_widget->set_change_callback(std::forward<Callback>(cb));
        return *this;
    }

    Fluent& value(float v) { m_widget->set_value(v); return *this; }
    Fluent& value(int v)   { m_widget->set_value(v); return *this; }
    Fluent& icon(int i)    { m_widget->set_icon(i);  return *this; }
    Fluent& flags(int f)   { m_widget->set_flags(f); return *this; }
    Fluent& background(const Color& background_color)   { m_widget->set_background_color( background_color); return *this; }

    // Escape hatch for anything else
    template <typename F>
    Fluent& tap(F&& f) { f(m_widget); return *this; }

    T* get() const { return m_widget; }
    operator T*() const { return m_widget; }
    T* operator->() const { return m_widget; }
    T& operator*() const { return *m_widget; }

private:
    T* m_widget;
};

// Compatibility helpers (these do NOT depend on Widget::add)
template <typename T>
Fluent<T> Make(T* widget) { return Fluent<T>(widget); }

template <typename T, typename... Args>
Fluent<T> Make(Args&&... args) {
    return Fluent<T>(new T(std::forward<Args>(args)...));
}

NAMESPACE_END(nanogui)
