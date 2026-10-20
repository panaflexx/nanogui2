/*
 * pagemade/dock_window.h — a tool window that can dock beside the pages.
 *
 * Two arrow buttons at the top right of the title bar ask for the left or
 * right side (pressed again, back to floating). The window only asks: the
 * app makes the room, places the window and then calls set_docked.
 * Dragging the title bar of a docked window undocks it; resizing one asks
 * for its room again. On the right, the width grip is the left edge so
 * the window grows into the page.
 */
#pragma once

#include <nanogui/window.h>

#include <functional>
#include <string>

namespace nanogui { class Button; }

class DockableWindow : public nanogui::Window {
public:
    enum class Dock { None, Left, Right };

    DockableWindow(nanogui::Widget *parent, const std::string &title, bool resizable);

    Dock docked() const { return m_dock; }
    void set_docked(Dock side);
    std::function<void(Dock)> on_dock;
    std::function<void()> on_dock_resize;

    bool mouse_drag_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel,
                          int button, int modifiers) override;

protected:
    bool check_horizontal_resize(const nanogui::Vector2i &mousePos) override;
    bool resize_from_left() const override { return m_dock == Dock::Right; }

private:
    nanogui::Button *m_dock_left = nullptr, *m_dock_right = nullptr;
    Dock m_dock = Dock::None;
};
