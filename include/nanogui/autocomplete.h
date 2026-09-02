/*
    nanogui/autocomplete.h -- Text box with a completion popup.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/textbox.h>
#include <functional>
#include <string>
#include <vector>

NAMESPACE_BEGIN(nanogui)

class PopupMenu;

/**
 * \class AutoCompleteBox autocomplete.h nanogui/autocomplete.h
 *
 * \brief A \ref TextBox that offers completions from a caller-supplied source.
 *
 * The widget knows nothing about where suggestions come from: install a
 * provider that maps the text typed so far to a list of \ref Item, and the box
 * takes care of the popup, the keyboard navigation and the substitution.
 *
 * \rst
 * .. code-block:: cpp
 *
 *    auto *box = new AutoCompleteBox(parent);
 *    box->set_token_separator(',');           // complete one address of a list
 *    box->set_provider([&](const std::string &q) {
 *        std::vector<AutoCompleteBox::Item> out;
 *        for (const Contact &c : contacts.search(q))
 *            out.push_back({ c.name, c.address, format_address(c) });
 *        return out;
 *    });
 * \endrst
 */
class NANOGUI_EXPORT AutoCompleteBox : public TextBox {
public:
    /// One row of the completion popup.
    struct Item {
        std::string label;   ///< Primary text, drawn left-aligned.
        std::string detail;  ///< Secondary text, drawn right-aligned; may be empty.
        std::string value;   ///< What replaces the typed text when picked.
    };

    /// Maps the text typed so far to the suggestions to offer.
    using Provider = std::function<std::vector<Item>(const std::string &query)>;

    AutoCompleteBox(Widget *parent, const std::string &value = "");
    virtual ~AutoCompleteBox();

    /// Install the completion source.  Without one the box is a plain TextBox.
    void set_provider(const Provider &provider) { m_provider = provider; }

    /// Longest list the popup will show (default 8).
    void set_max_items(int n) { m_max_items = n; }
    int  max_items() const { return m_max_items; }

    /**
     * \brief Complete one item of a separated list rather than the whole field.
     *
     * With a separator set, only the text after the last one is treated as the
     * query and replaced on selection, so "a@x.com, jan" completes just "jan".
     * '\0' (the default) completes against the entire contents.
     */
    void set_token_separator(char sep) { m_separator = sep; }
    char token_separator() const { return m_separator; }

    /// Text appended after a completion when a separator is set (default ", ").
    void set_separator_suffix(const std::string &s) { m_separator_suffix = s; }

    /// Invoked when the user picks a suggestion.
    void set_selected_callback(const std::function<void(const Item &)> &cb) {
        m_selected_callback = cb;
    }

    /// Whether the completion popup is currently on screen.
    bool popup_visible() const;

    /// Close the popup, if open.
    void hide_popup();

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override;
    virtual bool keyboard_character_event(unsigned int codepoint) override;
    virtual bool focus_event(bool focused) override;
    virtual void draw(NVGcontext *ctx) override;

protected:
    /// Re-query the provider and open, update or close the popup.
    void refresh_suggestions();
    /// Insert item \p index and close the popup.
    void accept(int index);
    /// Move the highlight by \p delta, wrapping at both ends.
    void move_highlight(int delta);
    void apply_highlight();

    /// The text being edited: TextBox keeps it in a scratch buffer while focused.
    const std::string &edit_text() const;
    void set_edit_text(const std::string &text);

    /// Bounds of the query within edit_text(), honouring the separator.
    size_t token_begin() const;
    std::string current_token() const;

    /// True while the pointer is over the popup, where a click is a selection
    /// rather than a dismissal.
    bool mouse_over_popup() const;

    Provider m_provider;
    PopupMenu *m_popup = nullptr;
    std::vector<Item> m_items;
    int m_highlighted = -1;
    int m_max_items = 8;
    char m_separator = '\0';
    std::string m_separator_suffix = ", ";
    std::function<void(const Item &)> m_selected_callback;
};

NAMESPACE_END(nanogui)
