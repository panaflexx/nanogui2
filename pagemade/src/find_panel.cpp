/*
 * pagemade/find_panel.cpp — see find_panel.h.
 */
#include "find_panel.h"

#include "composerview.h"
#include "composer/edit.h"

#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/datagrid.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>

#include <algorithm>
#include <functional>
#include <set>

using namespace nanogui;
using namespace pagemade;

namespace {

constexpr int kPageCol = 0, kTextCol = 1, kActionCol = 2;
constexpr int kPageW = 44, kActionW = 74;

/* A text box that also answers Enter, changed or not */
class EnterBox : public TextBox {
public:
    EnterBox(Widget *parent) : TextBox(parent, "") {
        set_editable(true);
        set_alignment(TextBox::Alignment::Left);
    }
    std::function<void()> on_enter;
    bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        const bool enter = (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) &&
                           action == GLFW_PRESS && focused();
        const bool r = TextBox::keyboard_event(key, scancode, action, modifiers);
        if (enter && on_enter)
            on_enter();
        return r || enter;
    }
};

} // namespace

class FindModel : public DataGridModel {
public:
    explicit FindModel(const FindPanel *p) : m_panel(p) {}
    size_t row_count() const override { return m_panel->rows().size(); }
    size_t column_count() const override { return 3; }
    std::string column_name(int c) const override {
        return c == kPageCol ? "Page" : c == kTextCol ? "Text" : "";
    }
    DataType column_type(int) const override { return DataType::String; }
    nanogui::any get_value(int row, int col) const override {
        if (row < 0 || (size_t) row >= m_panel->rows().size())
            return {};
        if (col == kPageCol)
            return nanogui::any(m_panel->rows()[(size_t) row].page);
        return nanogui::any(row);
    }
    CellStyle get_cell_style(int row, int col) const override {
        CellStyle s;
        s.fontSize = 15.f;
        if (col == kPageCol)
            s.h_align = Alignment::Middle;
        if (row >= 0 && (size_t) row < m_panel->rows().size() &&
            m_panel->rows()[(size_t) row].find.state == FoundText::State::Stale)
            s.fgColor = nvgRGBA(150, 150, 150, 255);
        return s;
    }

private:
    const FindPanel *m_panel;
};

FindPanel::FindPanel(Widget *parent, ComposerView *view) : ToolPanel(parent), m_view(view) {
    auto *flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                AlignItems::Stretch, 8, 6);
    set_layout(flex);

    auto line = [&] {
        Widget *w = new Widget(this);
        auto *l = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart, AlignItems::Center, 0, 6);
        w->set_layout(l);
        return std::make_pair(w, l);
    };
    auto field = [&](const char *caption, const char *placeholder) {
        auto row = line();
        auto *label = new Label(row.first, caption);
        label->set_fixed_size(Vector2i(56, 0));
        auto *box = new EnterBox(row.first);
        box->set_placeholder(placeholder);
        box->set_fixed_size(Vector2i(0, 26));
        row.second->set_flex_item(box, FlexLayout::FlexItem(1.f, 1.f, 0));
        return box;
    };
    EnterBox *find = field("Find", "Text to find");
    EnterBox *repl = field("Replace", "Replace with");
    m_find = find;
    m_replace = repl;
    find->on_enter = [this] { find_all(); };
    repl->on_enter = [this] { find_all(); };

    auto opts = line();
    new Label(opts.first, "");
    opts.first->children().back()->set_fixed_size(Vector2i(56, 0));
    m_case = new CheckBox(opts.first, "Match case");
    m_word = new CheckBox(opts.first, "Whole word");

    auto buttons = line();
    m_find_all = new Button(buttons.first, "Find All", FA_SEARCH);
    m_find_all->set_tooltip("List every match in the publication (Enter)");
    m_find_all->set_fixed_size(Vector2i(0, 28));
    buttons.second->set_flex_item(m_find_all, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_replace_all = new Button(buttons.first, "Replace All", FA_EXCHANGE_ALT);
    m_replace_all->set_tooltip("Replace every match in the list (one undo step)");
    m_replace_all->set_fixed_size(Vector2i(0, 28));
    buttons.second->set_flex_item(m_replace_all, FlexLayout::FlexItem(1.f, 1.f, 0));
    m_find_all->set_callback([this] { find_all(); });
    m_replace_all->set_callback([this] { replace_all(); });

    m_grid = new DataGrid(this);
    m_grid->set_editing_enabled(false);
    m_grid->set_selection_mode(DataGrid::SelectionMode::Single);
    flex->set_flex_item(m_grid, FlexLayout::FlexItem(1.f, 1.f, 0));

    DataGridColumn page;
    page.title = "Page";
    page.width = kPageW;
    page.min_width = 36;
    page.reorderable = false;

    DataGridColumn text;
    text.title = "Text";
    text.width = 200;
    text.reorderable = false;
    text.custom_draw = [this](NVGcontext *ctx, const Vector2i &pos, const Vector2i &size,
                              const nanogui::any &value, const CellStyle &, bool, bool) {
        const int *ri = nanogui::any_cast<int>(&value);
        if (!ri || *ri < 0 || (size_t) *ri >= m_rows.size())
            return;
        const Row &r = m_rows[(size_t) *ri];
        const bool stale = r.find.state == FoundText::State::Stale;
        const bool done = r.find.state == FoundText::State::Replaced;
        const NVGcolor ink = stale ? nvgRGBA(150, 150, 150, 255)
                                   : (m_theme ? NVGcolor(m_theme->m_text_color) : nvgRGB(220, 220, 220));
        const NVGcolor dim = stale ? ink : nvgRGBA((int) (ink.r * 255) , (int) (ink.g * 255),
                                                   (int) (ink.b * 255), 170);
        const float pad = 6.f, avail = (float) size.x() - 2.f * pad;
        const float cy = pos.y() + size.y() * 0.5f;
        nvgSave(ctx);
        nvgIntersectScissor(ctx, (float) pos.x() + 2.f, (float) pos.y(), (float) size.x() - 4.f,
                            (float) size.y());
        nvgFontSize(ctx, 15.f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        const std::string &mid = r.find.current();
        nvgFontFace(ctx, "sans");
        const float hw = nvgTextBounds(ctx, 0, 0, r.head.c_str(), nullptr, nullptr);
        nvgFontFace(ctx, "sans-bold");
        const float mw = mid.empty() ? 4.f : nvgTextBounds(ctx, 0, 0, mid.c_str(), nullptr, nullptr);
        /* The head takes at most a third, cut on the left, so the find shows */
        const float head_w = std::min(hw, std::max(0.f, std::min(avail * 0.34f, avail - mw)));
        float x = pos.x() + pad;
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, dim);
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x, (float) pos.y(), head_w, (float) size.y());
        nvgText(ctx, x + head_w - hw, cy, r.head.c_str(), nullptr);
        nvgRestore(ctx);
        x += head_w;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x - 1.f, cy - 9.f, mw + 2.f, 18.f, 3.f);
        nvgFillColor(ctx, stale ? nvgRGBA(128, 128, 128, 60)
                                : done ? nvgRGBA(60, 190, 90, 110) : nvgRGBA(255, 205, 0, 120));
        nvgFill(ctx);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, ink);
        nvgText(ctx, x, cy, mid.c_str(), nullptr);
        x += mw;
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, dim);
        nvgText(ctx, x, cy, r.tail.c_str(), nullptr);
        nvgRestore(ctx);
    };

    DataGridColumn action;
    action.title = "";
    action.width = kActionW;
    action.min_width = kActionW;
    action.resizable = false;
    action.reorderable = false;
    action.custom_draw = [this](NVGcontext *ctx, const Vector2i &pos, const Vector2i &size,
                                const nanogui::any &value, const CellStyle &, bool, bool) {
        const int *ri = nanogui::any_cast<int>(&value);
        if (!ri || *ri < 0 || (size_t) *ri >= m_rows.size())
            return;
        const FoundText::State st = m_rows[(size_t) *ri].find.state;
        if (st == FoundText::State::Stale)
            return;
        const float x = pos.x() + 4.f, y = pos.y() + 3.f;
        const float w = size.x() - 8.f, h = size.y() - 6.f;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x, y, w, h, 3.f);
        nvgFillColor(ctx, st == FoundText::State::Replaced ? nvgRGBA(255, 255, 255, 28)
                                                           : nvgRGBA(70, 130, 255, 150));
        nvgFill(ctx);
        nvgStrokeColor(ctx, nvgRGBA(0, 0, 0, 90));
        nvgStrokeWidth(ctx, 1.f);
        nvgStroke(ctx);
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 14.f);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, nvgRGB(235, 235, 235));
        nvgText(ctx, x + w * 0.5f, y + h * 0.5f,
                st == FoundText::State::Replaced ? "Revert" : "Replace", nullptr);
    };
    m_grid->set_columns({page, text, action});
    m_grid->set_cell_clicked_callback([this](int row, int col) {
        if (row < 0 || (size_t) row >= m_rows.size())
            return;
        if (col == kActionCol)
            toggle((size_t) row);
        else
            go_to((size_t) row);
    });

    m_status = new Label(this, "Type what to find, then Enter");
    m_status->set_font_size(14);
    update_status();
}

FindOptions FindPanel::options() const {
    FindOptions o;
    o.match_case = m_case->checked();
    o.whole_word = m_word->checked();
    return o;
}

void FindPanel::focus_find() {
    m_find->request_focus();
}

void FindPanel::perform_layout(NVGcontext *ctx) {
    ToolPanel::perform_layout(ctx);
    /* The text column takes what the page and button columns leave */
    const int w = m_grid->width() - kPageW - kActionW - 14;
    if (w > 40 && m_grid->columns().size() == 3 && m_grid->columns()[kTextCol].width != w) {
        m_grid->set_column_width(kPageCol, kPageW);
        m_grid->set_column_width(kTextCol, w);
    }
}

void FindPanel::find_all() {
    m_needle = m_find->value();
    std::vector<FoundText> finds =
        pagemade::find_all(m_view->document(), m_needle, m_replace->value(), options());
    m_rows.clear();
    for (FoundText &f : finds) {
        Row r;
        r.find = std::move(f);
        m_rows.push_back(std::move(r));
    }
    resolve();
    if (!m_model)
        m_model = std::make_shared<FindModel>(this);
    m_grid->set_model(m_model);          // back to the top
    update_status();
    if (!m_rows.empty())
        go_to(0);
}

void FindPanel::resolve() {
    std::vector<FoundText> finds;
    finds.reserve(m_rows.size());
    for (const Row &r : m_rows)
        finds.push_back(r.find);
    const PageDoc &doc = m_view->document();
    resolve_found(doc, finds);
    const auto &comps = m_view->compositions();
    for (size_t i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        r.find = std::move(finds[i]);
        found_context(doc, r.find, 40, 80, r.head, r.tail);
        /* Its page: the frame of the line it starts on */
        r.page = "\xE2\x80\x94";
        const size_t si = doc.story_index(r.find.story);
        if (si == SIZE_MAX || si >= comps.size() || r.find.state == FoundText::State::Stale)
            continue;
        const StoryEntry &se = doc.stories[si];
        const Caret at = caret_at(comps[si], se.story, TextPos{r.find.para, r.find.byte});
        if (!at.valid() || comps[si].lines[at.line].frame >= se.thread.size())
            continue;
        size_t page = 0;
        if (doc.find_item(se.thread[comps[si].lines[at.line].frame], &page))
            r.page = std::to_string(page + 1);
    }
}

void FindPanel::update_status() {
    if (m_rows.empty()) {
        m_status->set_caption(m_needle.empty() ? "Type what to find, then Enter"
                                               : "No matches for “" + m_needle + "”");
        m_replace_all->set_enabled(false);
        return;
    }
    size_t replaced = 0, stale = 0;
    for (const Row &r : m_rows) {
        replaced += r.find.state == FoundText::State::Replaced;
        stale += r.find.state == FoundText::State::Stale;
    }
    std::string s = std::to_string(m_rows.size()) + (m_rows.size() == 1 ? " match" : " matches");
    if (replaced)
        s += ", " + std::to_string(replaced) + " replaced";
    if (stale)
        s += ", " + std::to_string(stale) + " changed since";
    m_status->set_caption(s);
    m_replace_all->set_enabled(replaced + stale < m_rows.size());
}

void FindPanel::go_to(size_t row) {
    if (row >= m_rows.size())
        return;
    resolve();
    const FoundText &f = m_rows[row].find;
    if (f.state == FoundText::State::Stale)
        return;
    m_view->show_text(f.story, TextPos{f.para, f.byte},
                      TextPos{f.para, f.byte + (uint32_t) f.current().size()});
}

void FindPanel::toggle(size_t row) {
    if (row >= m_rows.size())
        return;
    resolve();
    Row &r = m_rows[row];
    if (r.find.state == FoundText::State::Stale)
        return;
    const bool replace = r.find.state == FoundText::State::Found;
    if (replace)
        r.find.replacement = m_replace->value();   // what the box says now
    m_busy = true;
    m_view->push_undo();
    const bool changed = apply_found(m_view->document(), r.find, replace);
    if (changed)
        m_view->story_text_changed(r.find.story);
    m_busy = false;
    resolve();
    update_status();
    if (changed)
        go_to(row);
    if (Screen *s = screen())
        s->redraw();
}

void FindPanel::replace_all() {
    resolve();
    bool any = false;
    for (Row &r : m_rows)
        if (r.find.state == FoundText::State::Found) {
            r.find.replacement = m_replace->value();
            any = true;
        }
    if (!any)
        return;
    m_busy = true;
    m_view->push_undo();
    /* Last first: a replacement moves only the text after it */
    std::set<StoryId> stories;
    PageDoc &doc = m_view->document();
    for (size_t i = m_rows.size(); i-- > 0;) {
        Row &r = m_rows[i];
        if (r.find.state == FoundText::State::Found && apply_found(doc, r.find, true))
            stories.insert(r.find.story);
    }
    for (StoryId id : stories)
        m_view->story_text_changed(id);
    m_busy = false;
    resolve();
    update_status();
    if (Screen *s = screen())
        s->redraw();
}

void FindPanel::refresh() {
    if (m_busy || m_rows.empty())
        return;
    resolve();
    update_status();
}
