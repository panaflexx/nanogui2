/*
 * pagemade/tool_tabs.cpp — see tool_tabs.h.
 */
#include "tool_tabs.h"

#include <nanogui/layout.h>
#include <nanogui/screen.h>
#include <nanogui/tabwidget.h>

#include <algorithm>

using namespace nanogui;

ToolTabsWindow::ToolTabsWindow(Widget *parent, const std::string &title)
    : DockableWindow(parent, title, true) {
    set_modal(false);
    set_traffic_lights_mask(0x1);
    auto *flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                AlignItems::Stretch, 4, 0);
    set_layout(flex);
    m_tabs = new TabWidget(this);
    flex->set_flex_item(m_tabs, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_tabs->set_callback([this](int tab) { tab_changed(tab); });
    set_size(Vector2i(350, 780));
}

Widget *ToolTabsWindow::tab_parent() const {
    return m_tabs;
}

void ToolTabsWindow::attach(ToolPanel *panel) {
    /* append_tab announces the first tab before its id is stored, so the
     * panel isn't told shown() while it is being built; the owner calls
     * shown() when it puts the window up */
    m_panels.push_back(Entry{panel, -1});
    m_panels.back().tab = m_tabs->append_tab(panel->title(), panel);
    update_floor();
    if (Screen *s = screen())
        if (s->nvg_context())
            perform_layout(s->nvg_context());
}

void ToolTabsWindow::update_floor() {
    Vector2i floor(0, 0);
    for (const Entry &e : m_panels)
        floor = max(floor, e.panel->floor_size());
    /* The tab strip and the window's own frame above the panel */
    set_min_width(floor.x() + 8);
    set_min_height(floor.y() + 70);
}

ToolPanel *ToolTabsWindow::current() const {
    const int tab = m_tabs->tab_count() ? m_tabs->selected_id() : -1;
    for (const Entry &e : m_panels)
        if (e.tab == tab)
            return e.panel;
    return nullptr;
}

void ToolTabsWindow::show_panel(ToolPanel *panel) {
    for (const Entry &e : m_panels)
        if (e.panel == panel) {
            if (m_tabs->selected_id() != e.tab)
                m_tabs->set_selected_id(e.tab);
            tab_changed(e.tab);
            break;
        }
    if (Screen *s = screen())
        if (s->nvg_context())
            perform_layout(s->nvg_context());
}

void ToolTabsWindow::tab_changed(int tab) {
    for (const Entry &e : m_panels)
        if (e.tab == tab)
            e.panel->shown();
    if (Screen *s = screen())
        s->redraw();
}

void ToolTabsWindow::refresh() {
    if (ToolPanel *front = current())    // the others catch up in shown()
        front->refresh();
}

void ToolTabsWindow::shown() {
    if (ToolPanel *front = current())
        front->shown();
}
