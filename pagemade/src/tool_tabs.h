/*
 * pagemade/tool_tabs.h — tool panels gathered as the tabs of one window.
 *
 * A ToolPanel is the body of one tool (Images, Layouts, Layers, Type): the
 * widgets, without a window around them. A ToolTabsWindow holds any number
 * of panels as tabs and docks beside the pages like any DockableWindow.
 * It tells each panel when the document changes and when its tab comes to
 * the front, so a panel that is out of sight does no work until it is
 * looked at.
 *
 * Tabs are draggable. Dropping one on another tool window merges it there
 * (the target draws a green outline, or a green band on its bottom third,
 * which stacks the two groups). Dropping on empty space tears it into a
 * new window. The window owns the chrome: title, close, docking, size
 * floor. The panel owns its contents.
 */
#pragma once

#include "dock_window.h"

#include <nanogui/tabwidget.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nanogui { class Split; }

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
    /* Closing the window hides it while any panel says so. A panel that
     * returns false is destroyed with a window that holds nothing else. */
    virtual bool persist_on_close() const { return true; }
    /* Height of the page body when this panel is alone and floating.
     * -1 leaves the window height alone. `width` is the window's width. */
    virtual int preferred_content_height(NVGcontext *, int) { return -1; }

    nanogui::Vector2i preferred_size(NVGcontext *) const override { return floor_size(); }
};

class ToolTabsWindow : public DockableWindow {
public:
    ToolTabsWindow(nanogui::Widget *parent, const std::string &title);
    ~ToolTabsWindow() override;

    /* A new panel of type T (its constructor takes the parent first, then
     * `args`) on a tab at the end. */
    template <typename T, typename... Args> T *add_panel(Args &&...args) {
        T *panel = new T(tab_parent(), std::forward<Args>(args)...);
        attach(panel, m_tabs);
        return panel;
    }

    size_t panel_count() const { return m_panels.size(); }
    ToolPanel *panel(size_t i) const { return i < m_panels.size() ? m_panels[i].panel : nullptr; }
    bool contains_panel(const ToolPanel *panel) const;
    /* True when closing should hide the window instead of destroying it. */
    bool any_persist() const;
    /* The two tab strips of a vertical split, or just the one. */
    bool split() const { return m_lower != nullptr; }
    /* The panel whose tab is in front of the upper strip (the lower strip
     * has its own selection; show_panel finds either). */
    ToolPanel *current() const;
    /* Bring a panel's tab to the front (it is told shown()). */
    void show_panel(ToolPanel *panel);
    /* The document changed. */
    void refresh();
    /* The window was shown again: the panel in front catches up. */
    void shown();
    /* Floating, one panel, and that panel names a body height: use it. */
    void fit_floating();

    /* The window a panel lives in, walking parents. */
    static ToolTabsWindow *host_of(nanogui::Widget *panel);
    static const std::vector<ToolTabsWindow *> &live();
    /* Called from the constructor, so a tear-off is wired like the others. */
    static void set_on_born(std::function<void(ToolTabsWindow *)> fn);
    /* The last panel was dragged out. The caller disposes the empty shell. */
    static void set_on_empty(std::function<void(ToolTabsWindow *)> fn);

    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down,
                            int modifiers) override;
    bool mouse_drag_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel,
                          int button, int modifiers) override;

private:
    struct Entry {
        ToolPanel *panel = nullptr;
        int tab = -1;
        nanogui::TabWidget *host = nullptr;
    };
    nanogui::Widget *tab_parent() const;
    nanogui::TabWidget *make_tabs(nanogui::Widget *parent);
    void attach(ToolPanel *panel, nanogui::TabWidget *host);
    void tab_changed(nanogui::TabWidget *host, int tab);
    void update_floor();
    void on_tab_drag(nanogui::TabWidget *host, int id, const nanogui::Vector2i &pos,
                     nanogui::TabDragPhase phase);
    void adopt(ToolPanel *panel, ToolTabsWindow *from, nanogui::TabWidget *dest, int index);
    void tear_off(ToolPanel *panel, const nanogui::Vector2i &pos);
    void ensure_split();
    void unwrap_empty_half();
    bool take(ToolPanel *panel, int *tab, nanogui::TabWidget **host);
    nanogui::TabWidget *tabs_at(const nanogui::Vector2i &screen_pos) const;
    int index_at(nanogui::TabWidget *tabs, const nanogui::Vector2i &screen_pos) const;
    nanogui::Window::DropHighlight hint_at(const nanogui::Vector2i &screen_pos) const;
    void relayout_now();

    nanogui::TabWidget *m_tabs = nullptr;
    nanogui::TabWidget *m_lower = nullptr;
    nanogui::Split *m_split = nullptr;
    std::vector<Entry> m_panels;
    bool m_fitting = false;

    static std::vector<ToolTabsWindow *> s_live;
    static std::function<void(ToolTabsWindow *)> s_on_born;
    static std::function<void(ToolTabsWindow *)> s_on_empty;
};
