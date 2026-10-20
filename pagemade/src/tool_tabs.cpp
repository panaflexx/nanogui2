/*
 * pagemade/tool_tabs.cpp — see tool_tabs.h.
 */
#include "tool_tabs.h"

#include <nanogui/layout.h>
#include <nanogui/screen.h>
#include <nanogui/split.h>
#include <nanogui/tabwidget.h>

#include <algorithm>

using namespace nanogui;

std::vector<ToolTabsWindow *> ToolTabsWindow::s_live;
std::function<void(ToolTabsWindow *)> ToolTabsWindow::s_on_born;
std::function<void(ToolTabsWindow *)> ToolTabsWindow::s_on_empty;

ToolTabsWindow::ToolTabsWindow(Widget *parent, const std::string &title)
    : DockableWindow(parent, title, true) {
    s_live.push_back(this);
    set_modal(false);
    set_traffic_lights_mask(0x1);
    auto *flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                AlignItems::Stretch, 4, 0);
    set_layout(flex);
    m_tabs = make_tabs(this);
    flex->set_flex_item(m_tabs, FlexLayout::FlexItem(1.f, 1.f, 0));
    set_size(Vector2i(350, 780));
    if (s_on_born)
        s_on_born(this);
}

ToolTabsWindow::~ToolTabsWindow() {
    s_live.erase(std::remove(s_live.begin(), s_live.end(), this), s_live.end());
}

const std::vector<ToolTabsWindow *> &ToolTabsWindow::live() {
    return s_live;
}

void ToolTabsWindow::set_on_born(std::function<void(ToolTabsWindow *)> fn) {
    s_on_born = std::move(fn);
}

void ToolTabsWindow::set_on_empty(std::function<void(ToolTabsWindow *)> fn) {
    s_on_empty = std::move(fn);
}

ToolTabsWindow *ToolTabsWindow::host_of(Widget *panel) {
    for (Widget *w = panel; w; w = w->parent())
        if (auto *frame = dynamic_cast<ToolTabsWindow *>(w))
            return frame;
    return nullptr;
}

TabWidget *ToolTabsWindow::make_tabs(Widget *parent) {
    auto *tabs = new TabWidget(parent);
    tabs->set_tabs_draggable(true);
    tabs->set_tabs_closeable(false);
    tabs->set_remove_children(false);
    tabs->set_callback([this, tabs](int tab) { tab_changed(tabs, tab); });
    tabs->set_tab_drag_callback([this, tabs](int id, const Vector2i &pos, TabDragPhase phase) {
        on_tab_drag(tabs, id, pos, phase);
    });
    return tabs;
}

Widget *ToolTabsWindow::tab_parent() const {
    return m_tabs;
}

void ToolTabsWindow::attach(ToolPanel *panel, TabWidget *host) {
    /* append_tab announces the first tab before its id is stored, so the
     * panel isn't told shown() while it is being built; the owner calls
     * shown() when it puts the window up */
    m_panels.push_back(Entry{panel, -1, host});
    m_panels.back().tab = host->append_tab(panel->title(), panel);
    update_floor();
    relayout_now();
}

void ToolTabsWindow::update_floor() {
    Vector2i floor(0, 0);
    for (const Entry &e : m_panels)
        if (e.panel)
            floor = max(floor, e.panel->floor_size());
    /* The tab strip and the window's own frame above the panel */
    set_min_width(std::max(150, floor.x() + 8));
    set_min_height(floor.y() + 70);
}

bool ToolTabsWindow::contains_panel(const ToolPanel *panel) const {
    for (const Entry &e : m_panels)
        if (e.panel == panel)
            return true;
    return false;
}

bool ToolTabsWindow::any_persist() const {
    for (const Entry &e : m_panels)
        if (e.panel && e.panel->persist_on_close())
            return true;
    return false;
}

ToolPanel *ToolTabsWindow::current() const {
    if (!m_tabs || m_tabs->tab_count() == 0)
        return nullptr;
    const int tab = m_tabs->selected_id();
    for (const Entry &e : m_panels)
        if (e.host == m_tabs && e.tab == tab)
            return e.panel;
    return nullptr;
}

void ToolTabsWindow::show_panel(ToolPanel *panel) {
    for (const Entry &e : m_panels)
        if (e.panel == panel && e.host && e.tab >= 0) {
            if (e.host->selected_id() != e.tab)
                e.host->set_selected_id(e.tab);
            tab_changed(e.host, e.tab);
            break;
        }
    /* The other half of a split was not the one just chosen. It still
     * catches up: it may have been hidden with the window. */
    if (m_lower && m_tabs) {
        TabWidget *other = nullptr;
        for (const Entry &e : m_panels)
            if (e.panel == panel)
                other = e.host == m_tabs ? m_lower : m_tabs;
        if (other && other->tab_count() > 0)
            tab_changed(other, other->selected_id());
    }
    relayout_now();
}

void ToolTabsWindow::tab_changed(TabWidget *host, int tab) {
    for (const Entry &e : m_panels)
        if (e.host == host && e.tab == tab && e.panel)
            e.panel->shown();
    if (Screen *s = screen())
        s->redraw();
}

void ToolTabsWindow::refresh() {
    if (ToolPanel *front = current())
        front->refresh();
    if (m_lower && m_lower->tab_count() > 0) {
        const int tab = m_lower->selected_id();
        for (const Entry &e : m_panels)
            if (e.host == m_lower && e.tab == tab && e.panel)
                e.panel->refresh();
    }
}

void ToolTabsWindow::shown() {
    if (m_tabs && m_tabs->tab_count() > 0)
        tab_changed(m_tabs, m_tabs->selected_id());
    if (m_lower && m_lower->tab_count() > 0)
        tab_changed(m_lower, m_lower->selected_id());
}

void ToolTabsWindow::relayout_now() {
    if (Screen *s = screen())
        if (NVGcontext *ctx = s->nvg_context())
            perform_layout(ctx);
}

void ToolTabsWindow::fit_floating() {
    if (m_fitting || docked() != Dock::None || m_lower || m_panels.size() != 1)
        return;
    ToolPanel *panel = m_panels[0].panel;
    Screen *s = screen();
    NVGcontext *ctx = s ? s->nvg_context() : nullptr;
    if (!panel || !ctx || width() <= 0)
        return;
    perform_layout(ctx);
    const int content = panel->preferred_content_height(ctx, width());
    if (content < 0)
        return;
    const int header = theme() ? theme()->m_window_header_height : 30;
    int h = header + 36 + content;
    if (parent()) {
        const int room = parent()->height();
        const int below = room - 16 - position().y();
        if (h > below && below >= 320)
            h = below;
        h = std::min(h, std::max(160, room - 32));
        if (position().y() + h > room - 16)
            set_position(Vector2i(position().x(), std::max(16, room - 16 - h)));
    }
    m_fitting = true;
    if (h != height())
        set_size(Vector2i(width(), h));
    perform_layout(ctx);
    m_fitting = false;
    if (s)
        s->redraw();
}

bool ToolTabsWindow::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    const bool handled = DockableWindow::mouse_button_event(p, button, down, modifiers);
    /* A title double-click would maximize the palette. Keep it floating. */
    m_last_title_click = 0;
    return handled;
}

bool ToolTabsWindow::mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button,
                                      int modifiers) {
    const int before = width();
    const bool handled = DockableWindow::mouse_drag_event(p, rel, button, modifiers);
    if (m_resize && docked() == Dock::None && width() != before)
        fit_floating();
    return handled;
}

bool ToolTabsWindow::take(ToolPanel *panel, int *tab, TabWidget **host) {
    for (auto it = m_panels.begin(); it != m_panels.end(); ++it) {
        if (it->panel != panel)
            continue;
        *tab = it->tab;
        *host = it->host;
        m_panels.erase(it);
        return true;
    }
    return false;
}

void ToolTabsWindow::adopt(ToolPanel *panel, ToolTabsWindow *from, TabWidget *dest, int index) {
    if (!panel || !from || !dest)
        return;
    int old = -1;
    TabWidget *src = nullptr;
    if (!from->take(panel, &old, &src) || !src)
        return;
    /* Record the panel before insert_tab, which tells the first tab it is shown. */
    m_panels.push_back(Entry{panel, -1, dest});
    const int nid = src->transfer_tab(old, dest, index);
    m_panels.back().tab = nid;
    if (nid >= 0) {
        dest->set_selected_id(nid);
        /* insert_tab only announces a strip's first tab, and it does that
         * before the id is stored above, so the panel is told here. */
        tab_changed(dest, nid);
    }
    update_floor();
    from->update_floor();
    relayout_now();
    from->relayout_now();
}

Window::DropHighlight ToolTabsWindow::hint_at(const Vector2i &screen_pos) const {
    if (m_lower)
        return DropHighlight::Outline;
    const int header = theme() ? theme()->m_window_header_height : 30;
    const int top = absolute_position().y() + header;
    const int body = std::max(1, height() - header);
    if (screen_pos.y() >= top + (body * 2) / 3)
        return DropHighlight::Band;
    return DropHighlight::Outline;
}

TabWidget *ToolTabsWindow::tabs_at(const Vector2i &screen_pos) const {
    auto covers = [&](TabWidget *t) {
        if (!t)
            return false;
        const Vector2i a = t->absolute_position();
        return screen_pos.x() >= a.x() && screen_pos.y() >= a.y()
            && screen_pos.x() < a.x() + t->width()
            && screen_pos.y() < a.y() + t->height();
    };
    if (covers(m_lower))
        return m_lower;
    return m_tabs;
}

int ToolTabsWindow::index_at(TabWidget *tabs, const Vector2i &screen_pos) const {
    if (!tabs)
        return 0;
    if (!tabs->parent())
        return tabs->tab_count();
    const Vector2i local = screen_pos - tabs->parent()->absolute_position();
    const int at = tabs->tab_index_at(local);
    return at >= 0 ? at : tabs->tab_count();
}

void ToolTabsWindow::ensure_split() {
    if (m_lower)
        return;
    m_split = new Split(this, Split::Orientation::Vertical);
    m_tabs->reparent(m_split);
    m_lower = make_tabs(m_split);
    if (auto *flex = dynamic_cast<FlexLayout *>(layout()))
        flex->set_flex_item(m_split, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_split->set_drag_position(0.55f);
    relayout_now();
}

void ToolTabsWindow::unwrap_empty_half() {
    if (!m_split || !m_lower || !parent())
        return;
    TabWidget *empty = nullptr;
    TabWidget *keep = nullptr;
    if (m_tabs->tab_count() == 0 && m_lower->tab_count() > 0) {
        empty = m_tabs;
        keep = m_lower;
    } else if (m_lower->tab_count() == 0 && m_tabs->tab_count() > 0) {
        empty = m_lower;
        keep = m_tabs;
    } else {
        return;
    }
    keep->reparent(this);
    (void) empty;
    remove_child(m_split);
    m_split = nullptr;
    m_lower = nullptr;
    m_tabs = keep;
    if (auto *flex = dynamic_cast<FlexLayout *>(layout()))
        flex->set_flex_item(m_tabs, FlexLayout::FlexItem(1.f, 1.f, 0));
    relayout_now();
}

void ToolTabsWindow::tear_off(ToolPanel *panel, const Vector2i &pos) {
    Screen *s = screen();
    if (!s || !panel)
        return;
    auto *born = new ToolTabsWindow(s, panel->title());
    const Vector2i floor = panel->floor_size();
    born->set_size(Vector2i(std::max(240, floor.x() + 16), floor.y() + 70));
    Vector2i at = pos - Vector2i(48, 18);
    at.x() = std::max(0, std::min(at.x(), s->width() - 80));
    at.y() = std::max(0, std::min(at.y(), s->height() - 40));
    born->set_position(at);
    born->adopt(panel, this, born->m_tabs, 0);
    born->set_visible(true);
    s->move_window_to_front(born);
    born->fit_floating();
}

void ToolTabsWindow::on_tab_drag(TabWidget *host, int id, const Vector2i &pos, TabDragPhase phase) {
    for (ToolTabsWindow *w : s_live)
        w->set_drop_highlight(DropHighlight::None);
    if (phase == TabDragPhase::Cancel)
        return;

    ToolPanel *panel = nullptr;
    for (const Entry &e : m_panels)
        if (e.host == host && e.tab == id)
            panel = e.panel;
    if (!panel)
        return;

    /* The front widget under the pointer wins. A swatch, a popup, or the
     * page in front of a tool window is empty space: the drop does not
     * pass through it onto a palette behind. */
    ToolTabsWindow *target = nullptr;
    if (Screen *scr = screen()) {
        for (Widget *w = scr->find_widget(pos); w && w != scr; w = w->parent()) {
            if (auto *frame = dynamic_cast<ToolTabsWindow *>(w)) {
                target = frame;
                break;
            }
        }
    }

    const bool other = target && target != this;
    DropHighlight hint = DropHighlight::None;
    if (other) {
        hint = target->hint_at(pos);
        if (phase == TabDragPhase::Move)
            target->set_drop_highlight(hint);
    }
    if (phase != TabDragPhase::Release)
        return;

    ToolTabsWindow *dest = nullptr;
    if (other && hint == DropHighlight::Band) {
        target->ensure_split();
        target->adopt(panel, this, target->m_lower, target->m_lower->tab_count());
        dest = target;
    } else if (other) {
        TabWidget *tabs = target->tabs_at(pos);
        target->adopt(panel, this, tabs, target->index_at(tabs, pos));
        dest = target;
    } else if (!target) {
        tear_off(panel, pos);
    }

    ToolTabsWindow *src = this;
    async([src, dest] {
        if (src->parent()) {
            src->unwrap_empty_half();
            if (src->panel_count() == 0 && s_on_empty)
                s_on_empty(src);
        }
        if (dest && dest != src && dest->parent())
            dest->unwrap_empty_half();
    });
}
