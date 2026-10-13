/*
 * pagemade/dock_window.cpp — see dock_window.h.
 */
#include "dock_window.h"

#include <nanogui/button.h>
#include <nanogui/icons.h>
#include <nanogui/screen.h>

using namespace nanogui;

DockableWindow::DockableWindow(Widget *parent, const std::string &title, bool resizable)
    : Window(parent, title, resizable) {
    auto dock_button = [this](int icon, const char *tip, Dock side) {
        auto *b = new Button(button_panel(), "", icon);
        b->set_flags(Button::ToggleButton);
        b->set_tooltip(tip);
        b->set_change_callback([this, side](bool on) {
            if (on_dock)
                on_dock(on ? side : Dock::None);
        });
        return b;
    };
    m_dock_left = dock_button(FA_ARROW_LEFT, "Dock on the left of the pages", Dock::Left);
    m_dock_right = dock_button(FA_ARROW_RIGHT, "Dock on the right of the pages", Dock::Right);
}

void DockableWindow::set_docked(Dock side) {
    m_dock = side;
    m_dock_left->set_pushed(side == Dock::Left);
    m_dock_right->set_pushed(side == Dock::Right);
    set_draw_shadow(side == Dock::None);   // docked, it sits flush beside the pages
    if (screen())
        screen()->redraw();
}

bool DockableWindow::mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button,
                                      int modifiers) {
    /* Moving a docked window takes it out of the dock first */
    if (m_dock != Dock::None && m_drag && on_dock)
        on_dock(Dock::None);
    const Vector2i before = size();
    const bool handled = Window::mouse_drag_event(p, rel, button, modifiers);
    if (m_dock != Dock::None && m_resize && size() != before && on_dock_resize)
        on_dock_resize();
    return handled;
}
