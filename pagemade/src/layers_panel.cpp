/*
 * pagemade/layers_panel.cpp — see layers_panel.h.
 */
#include "layers_panel.h"

#include "composerview.h"
#include "dialogs.h"

#include <nanogui/button.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/screen.h>

using namespace nanogui;
using namespace pagemade;

LayersPanel::LayersPanel(Widget *parent, ComposerView *view)
    : ToolPanel(parent), m_view(view) {
    auto *flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                AlignItems::Stretch, 8, 6);
    set_layout(flex);

    m_rows = new Widget(this);
    m_rows->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 3));
    static_assert(sizeof m_row / sizeof m_row[0] == kMaxLayers, "a row per layer");
    for (size_t k = 0; k < kMaxLayers; ++k) {
        Row &r = m_row[k];
        r.row = new Widget(m_rows);
        auto *fl = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                  AlignItems::Center, 0, 3);
        r.row->set_layout(fl);
        r.eye = new Button(r.row, "", FA_EYE);
        r.eye->set_fixed_size(Vector2i(26, 26));
        r.eye->set_callback([this, k] {
            const LayerId id = layer_at_row(k);
            for (const Layer &l : m_view->layers())
                if (l.id == id)
                    m_view->set_layer_visible(id, !l.visible);
            refresh();
        });
        r.lock = new Button(r.row, "", FA_LOCK_OPEN);
        r.lock->set_fixed_size(Vector2i(26, 26));
        r.lock->set_callback([this, k] {
            const LayerId id = layer_at_row(k);
            for (const Layer &l : m_view->layers())
                if (l.id == id)
                    m_view->set_layer_locked(id, !l.locked);
            refresh();
        });
        r.name = new Button(r.row, "");
        r.name->set_flags(Button::ToggleButton);
        r.name->set_fixed_size(Vector2i(0, 26));
        r.name->set_change_callback([this, k](bool) {
            if (const LayerId id = layer_at_row(k))
                m_view->set_active_layer(id);
            m_sig.clear();               // pressed again: show it pressed still
            refresh();
        });
        fl->set_flex_item(r.name, FlexLayout::FlexItem(1.f, 1.f, 0));
    }

    /* The commands, in two short rows so a narrow docked panel keeps them */
    auto bar = [&] {
        Widget *w = new Widget(this);
        auto *l = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                 AlignItems::Center, 0, 4);
        w->set_layout(l);
        return std::make_pair(w, l);
    };
    auto tool = [](std::pair<Widget *, FlexLayout *> b, int icon, const char *tip) {
        auto *btn = new Button(b.first, "", icon);
        btn->set_tooltip(tip);
        btn->set_fixed_size(Vector2i(0, 26));
        b.second->set_flex_item(btn, FlexLayout::FlexItem(1.f, 1.f, 0));
        return btn;
    };
    auto row1 = bar();
    m_new = tool(row1, FA_PLUS, "New layer, above the active one");
    m_delete = tool(row1, FA_TRASH_ALT, "Delete the active layer (its items go to the layer below)");
    m_up = tool(row1, FA_ARROW_UP, "Move the active layer up");
    m_down = tool(row1, FA_ARROW_DOWN, "Move the active layer down");
    m_rename = tool(row1, FA_PEN, "Rename the active layer");
    auto row2 = bar();
    m_move_here = new Button(row2.first, "Move Selection Here", FA_SIGN_IN_ALT);
    m_move_here->set_tooltip("Put the selected items on the active layer");
    m_move_here->set_fixed_size(Vector2i(0, 26));
    row2.second->set_flex_item(m_move_here, FlexLayout::FlexItem(1.f, 1.f, 0));

    m_new->set_callback([this] {
        const int n = (int) m_view->layers().size() + 1;
        m_view->add_layer("Layer " + std::to_string(n));
    });
    m_delete->set_callback([this] { m_view->remove_layer(m_view->active_layer()); });
    m_up->set_callback([this] { m_view->move_layer(m_view->active_layer(), 1); });
    m_down->set_callback([this] { m_view->move_layer(m_view->active_layer(), -1); });
    m_rename->set_callback([this] {
        const LayerId id = m_view->active_layer();
        std::string name;
        for (const Layer &l : m_view->layers())
            if (l.id == id)
                name = l.name;
        if (Screen *s = screen())
            ask_text(s, "Rename Layer", "Name:", name, "",
                     [this, id](const std::string &n) { m_view->rename_layer(id, n); });
    });
    m_move_here->set_callback([this] { m_view->move_selection_to_layer(m_view->active_layer()); });

    rebuild();
}

std::string LayersPanel::signature() const {
    std::string sig = std::to_string(m_view->active_layer()) + "|" +
                      std::to_string(m_view->selection().empty()) + "|";
    for (const Layer &l : m_view->layers())
        sig += std::to_string(l.id) + ":" + l.name + ":" + (l.visible ? "v" : "h") +
               (l.locked ? "l" : "u") + ":" + std::to_string(m_view->layer_item_count(l.id)) + ";";
    return sig;
}

LayerId LayersPanel::layer_at_row(size_t k) const {
    const std::vector<Layer> &layers = m_view->layers();
    return k < layers.size() ? layers[layers.size() - 1 - k].id : 0;
}

void LayersPanel::refresh() {
    if (signature() != m_sig)
        rebuild();
}

void LayersPanel::rebuild() {
    m_sig = signature();
    const std::vector<Layer> &layers = m_view->layers();
    const LayerId active = m_view->active_layer();
    /* Top layer first, as it stacks */
    for (size_t k = 0; k < kMaxLayers; ++k) {
        Row &r = m_row[k];
        const LayerId id = layer_at_row(k);
        r.row->set_visible(id != 0);
        if (!id)
            continue;
        const Layer &l = layers[layers.size() - 1 - k];
        r.eye->set_icon(l.visible ? FA_EYE : FA_EYE_SLASH);
        r.eye->set_tooltip(l.visible ? "Shown: click to hide (hidden layers don't print)"
                                     : "Hidden: click to show");
        r.lock->set_icon(l.locked ? FA_LOCK : FA_LOCK_OPEN);
        r.lock->set_tooltip(l.locked ? "Locked: click to unlock" : "Unlocked: click to lock");
        r.name->set_caption(l.name + "  (" + std::to_string(m_view->layer_item_count(id)) + ")");
        r.name->set_pushed(id == active);
        r.name->set_tooltip(id == active ? "The active layer: new items go here"
                                         : "Click to make this the active layer");
        r.name->set_text_color(l.visible ? nanogui::Color(0, 0, 0, 0)        // theme color
                                         : nanogui::Color(150, 150, 150, 255));
    }

    size_t ai = 0;
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].id == active)
            ai = i;
    m_new->set_enabled(layers.size() < kMaxLayers);
    m_new->set_tooltip(layers.size() < kMaxLayers ? "New layer, above the active one"
                                                  : "A publication has at most 8 layers");
    m_delete->set_enabled(layers.size() > 1);
    m_up->set_enabled(ai + 1 < layers.size());
    m_down->set_enabled(ai > 0);
    m_move_here->set_enabled(!m_view->selection().empty());

    if (auto *frame = ToolTabsWindow::host_of(this)) {
        if (frame->docked() == DockableWindow::Dock::None)
            frame->fit_floating();
        else if (Screen *s = screen())
            if (NVGcontext *ctx = s->nvg_context())
                perform_layout(ctx);
    }
    if (Screen *s = screen())
        s->redraw();
}

int LayersPanel::preferred_content_height(NVGcontext *ctx, int) {
    return std::max(120, Widget::preferred_size(ctx).y());
}
