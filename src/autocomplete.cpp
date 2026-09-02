/*
    src/autocomplete.cpp -- Text box with a completion popup.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/autocomplete.h>
#include <nanogui/menu.h>
#include <nanogui/screen.h>
#include <nanogui/window.h>
#include <nanogui/opengl.h>
#include <nanogui/theme.h>

#include <algorithm>

NAMESPACE_BEGIN(nanogui)

AutoCompleteBox::AutoCompleteBox(Widget *parent, const std::string &value)
    : TextBox(parent, value) {
    set_editable(true);
    set_alignment(TextBox::Alignment::Left);
}

AutoCompleteBox::~AutoCompleteBox() {
    /* The popup is parented to the Screen, not to us, so it outlives this
       widget unless we take it down explicitly -- the same ownership wrinkle
       Dropdown has to deal with. */
    if (m_popup) {
        m_popup->set_visible(false);
        if (Screen *s = screen())
            s->remove_popup_visible(m_popup);
        m_popup->dispose();
        m_popup = nullptr;
    }
}

bool AutoCompleteBox::popup_visible() const {
    return m_popup && m_popup->visible();
}

// ---------------------------------------------------------------------------
// Text access
// ---------------------------------------------------------------------------

const std::string &AutoCompleteBox::edit_text() const {
    /* While focused TextBox edits a scratch copy and only commits on blur. */
    return m_committed ? m_value : m_value_temp;
}

void AutoCompleteBox::set_edit_text(const std::string &text) {
    m_value = text;
    m_value_temp = text;
    m_cursor_pos = (int)text.size();
    m_selection_pos = kNoCursor;
}

size_t AutoCompleteBox::token_begin() const {
    if (m_separator == '\0') return 0;
    const std::string &t = edit_text();
    size_t sep = t.find_last_of(m_separator);
    if (sep == std::string::npos) return 0;
    /* Skip the whitespace that follows the separator. */
    size_t b = sep + 1;
    while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
    return b;
}

std::string AutoCompleteBox::current_token() const {
    return edit_text().substr(token_begin());
}

// ---------------------------------------------------------------------------
// Popup
// ---------------------------------------------------------------------------

void AutoCompleteBox::hide_popup() {
    if (!m_popup) return;
    m_popup->set_visible(false);
    if (Screen *s = screen())
        s->remove_popup_visible(m_popup);
    m_highlighted = -1;
}

bool AutoCompleteBox::mouse_over_popup() const {
    if (!popup_visible() || !m_popup->parent()) return false;
    Vector2i p = screen()->mouse_pos() - m_popup->parent()->absolute_position();
    return m_popup->contains(p);
}

void AutoCompleteBox::refresh_suggestions() {
    if (!m_provider || !focused()) { hide_popup(); return; }

    m_items = m_provider(current_token());
    if ((int)m_items.size() > m_max_items)
        m_items.resize((size_t)m_max_items);

    if (m_items.empty()) { hide_popup(); return; }

    Screen *s = screen();
    Window *w = window();
    if (!s || !w) { hide_popup(); return; }

    if (!m_popup) {
        /* No parent MenuItem: PopupMenu guards every use of it, and we drive
           the highlight ourselves rather than handing over focus. */
        m_popup = new PopupMenu(s, w, nullptr, false);
        m_popup->set_visible(false);
    }

    /* Rebuild the rows.  The list is short (m_max_items), so replacing it
       wholesale is cheaper than diffing and keeps the indices honest. */
    while (m_popup->child_count() > 0)
        m_popup->remove_child_at(m_popup->child_count() - 1);

    for (int i = 0; i < (int)m_items.size(); ++i) {
        const Item &item = m_items[(size_t)i];
        std::vector<Shortcut> sc;
        if (!item.detail.empty()) {
            Shortcut s0;
            s0.text = item.detail;
            sc.push_back(s0);
        }
        auto *mi = new MenuItem(m_popup, item.label, 0, sc);
        mi->set_theme(m_popup->theme());
        mi->set_callback([this, i] { accept(i); });
    }

    NVGcontext *ctx = s->nvg_context();
    Vector2i pref = m_popup->preferred_size(ctx);
    /* Never narrower than the field it belongs to. */
    pref.x() = std::max(pref.x(), width());
    m_popup->set_size(pref);
    m_popup->perform_layout(ctx);

    Vector2i pos = absolute_position() + Vector2i(0, height() + 2);
    /* Flip above the field when there is no room below. */
    if (pos.y() + m_popup->size().y() > s->height() &&
        absolute_position().y() - m_popup->size().y() - 2 >= 0)
        pos.y() = absolute_position().y() - m_popup->size().y() - 2;
    pos.x() = std::min(pos.x(), std::max(0, s->width() - m_popup->size().x()));

    m_popup->set_position(pos);
    if (!m_popup->visible()) {
        m_popup->set_visible(true);
        s->set_popup_visible(m_popup);
    }

    if (m_highlighted >= (int)m_items.size()) m_highlighted = -1;
    apply_highlight();
}

void AutoCompleteBox::apply_highlight() {
    if (!m_popup) return;
    for (int i = 0; i < m_popup->child_count(); ++i) {
        if (auto *mi = dynamic_cast<MenuItem *>(m_popup->child_at(i)))
            mi->set_highlighted(i == m_highlighted);
    }
}

void AutoCompleteBox::move_highlight(int delta) {
    if (m_items.empty()) return;
    const int n = (int)m_items.size();
    if (m_highlighted < 0)
        m_highlighted = delta > 0 ? 0 : n - 1;
    else
        m_highlighted = (m_highlighted + delta % n + n) % n;
    apply_highlight();
}

void AutoCompleteBox::accept(int index) {
    if (index < 0 || index >= (int)m_items.size()) return;
    const Item item = m_items[(size_t)index];

    std::string text = edit_text();
    text.erase(token_begin());
    text += item.value;
    if (m_separator != '\0')
        text += m_separator_suffix;

    set_edit_text(text);
    hide_popup();

    /* A click moved focus into the popup; put the caret back so the user can
       carry straight on typing the next address. */
    request_focus();
    m_committed = false;
    m_value_temp = text;
    m_cursor_pos = (int)text.size();

    if (m_selected_callback) m_selected_callback(item);
    if (m_callback) m_callback(m_value);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

bool AutoCompleteBox::keyboard_event(int key, int scancode, int action,
                                     int modifiers) {
    if (m_editable && focused() && popup_visible() &&
        (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        switch (key) {
            case GLFW_KEY_DOWN:   move_highlight(1);  return true;
            case GLFW_KEY_UP:     move_highlight(-1); return true;
            case GLFW_KEY_ESCAPE:
                /* Dismiss the list first; a second Escape leaves the field. */
                hide_popup();
                return true;
            case GLFW_KEY_ENTER:
            case GLFW_KEY_KP_ENTER:
            case GLFW_KEY_TAB:
                if (m_highlighted >= 0) { accept(m_highlighted); return true; }
                hide_popup();
                break;               /* nothing picked: let TextBox commit */
            default: break;
        }
    }

    bool handled = TextBox::keyboard_event(key, scancode, action, modifiers);

    /* Backspace and Delete change the query without producing a character. */
    if (m_editable && focused() &&
        (action == GLFW_PRESS || action == GLFW_REPEAT) &&
        (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE))
        refresh_suggestions();

    return handled;
}

bool AutoCompleteBox::keyboard_character_event(unsigned int codepoint) {
    bool handled = TextBox::keyboard_character_event(codepoint);
    if (handled) refresh_suggestions();
    return handled;
}

bool AutoCompleteBox::focus_event(bool focused) {
    /* Losing focus to a click on our own popup is a selection in progress --
       tearing the list down here would swallow the click. */
    if (!focused && mouse_over_popup())
        return TextBox::focus_event(focused);

    if (!focused) hide_popup();
    return TextBox::focus_event(focused);
}

void AutoCompleteBox::draw(NVGcontext *ctx) {
    /* The popup follows the field if a scroll or resize moved it. */
    if (popup_visible()) {
        Vector2i pos = absolute_position() + Vector2i(0, height() + 2);
        if (m_popup->position().x() != pos.x() ||
            (m_popup->position().y() != pos.y() &&
             m_popup->position().y() > absolute_position().y()))
            m_popup->set_position(pos);
    }
    TextBox::draw(ctx);
}

NAMESPACE_END(nanogui)
