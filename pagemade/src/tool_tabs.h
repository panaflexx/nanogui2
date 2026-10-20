/*
 * pagemade/tool_tabs.h — tool panels gathered as the tabs of one window.
 *
 * A ToolPanel is the body of one tool (Image Assets, Layouts, ...): the
 * widgets, without a window around them. A ToolTabsWindow holds any number
 * of panels as tabs and docks beside the pages like any DockableWindow.
 * It tells each panel when the document changes and when its tab comes to
 * the front, so a panel that is out of sight does no work until it is
 * looked at.
 *
 * To fold another dialog in, make its body a ToolPanel and add_panel() it
 * to a window. The window owns the chrome: title, close, docking, size
 * floor. The panel owns its contents and what they do.
 */
#pragma once

#include "dock_window.h"

#include <string>
#include <utility>
#include <vector>

namespace nanogui { class TabWidget; }

class ToolPanel : public nanogui::Widget {
public:
    explicit ToolPanel(nanogui::Widget *parent) : nanogui::Widget(parent) {}

    /* The caption on its tab. */
    virtual std::string title() const = 0;
    /* The document changed. Only the panel in front hears at once; the
     * others are refreshed when their tab is next chosen. */
    virtual void refresh() {}
    /* Its tab came to the front, or the window was shown with it there.
     * Panels behind were possibly told nothing meanwhile: catch up. */
    virtual void shown() { refresh(); }
    /* The smallest size it is usable at. The window's floor is the largest
     * of its panels', and the tab widget asks for no more than this, so
     * scroll panels inside don't pin the window to the screen's height. */
    virtual nanogui::Vector2i floor_size() const { return nanogui::Vector2i(200, 300); }

    nanogui::Vector2i preferred_size(NVGcontext *) const override { return floor_size(); }
};

class ToolTabsWindow : public DockableWindow {
public:
    ToolTabsWindow(nanogui::Widget *parent, const std::string &title);

    /* A new panel of type T (its constructor takes the parent first, then
     * `args`) on a tab at the end. */
    template <typename T, typename... Args> T *add_panel(Args &&...args) {
        T *panel = new T(tab_parent(), std::forward<Args>(args)...);
        attach(panel);
        return panel;
    }

    size_t panel_count() const { return m_panels.size(); }
    ToolPanel *panel(size_t i) const { return i < m_panels.size() ? m_panels[i].panel : nullptr; }
    /* The panel whose tab is in front. */
    ToolPanel *current() const;
    /* Bring a panel's tab to the front (it is told shown()). */
    void show_panel(ToolPanel *panel);
    /* The document changed. */
    void refresh();
    /* The window was shown again: the panel in front catches up. */
    void shown();

private:
    struct Entry {
        ToolPanel *panel = nullptr;
        int tab = -1;
    };
    nanogui::Widget *tab_parent() const;
    void attach(ToolPanel *panel);
    void tab_changed(int tab);
    void update_floor();

    nanogui::TabWidget *m_tabs = nullptr;
    std::vector<Entry> m_panels;
};
