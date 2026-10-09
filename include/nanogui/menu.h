//
// Copyright (C) Wojciech Jarosz <wjarosz@gmail.com>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE.txt file.
//

#pragma once

#include <nanogui/button.h>
#include <nanogui/popup.h>

/**
    Represents a key press optionally combined with one or more modifier keys.
    A Shortcut also stores a human-readable #text string describing the key combination for use by UI elements.
*/
struct NANOGUI_EXPORT Shortcut
{
    int         modifiers, key; ///< The GLFW modifiers (shift, command, etc) and key used to execute this shortcut
    std::string text;           ///< Human-readable string auto-generated from the above

    /// Construct a shortcut from a GLFW modifier and key code combination
    Shortcut(int m = 0, int k = 0);

    bool operator==(const Shortcut &rhs) const { return modifiers == rhs.modifiers && key == rhs.key; }
    bool operator!=(const Shortcut &rhs) const { return modifiers != rhs.modifiers || key != rhs.key; }
    bool operator<(const Shortcut &rhs) const
    {
        return modifiers < rhs.modifiers || (modifiers == rhs.modifiers && key < rhs.key);
    }

    //! Takes a fmt-format string and replaces any instances of {CMD} and {ALT} with CMD and ALT.
    static std::string key_string(const std::string &text);
};

NAMESPACE_BEGIN(nanogui)

/**
    A #MenuItem can have one or more keyboard #Shortcuts which can be used to run the callback associated with the item.
    These callbacks are run by #MenuBar::process_shortcuts for all #MenuItems associated with a #MenuBar.

    If an item has more than one shortcut, the first one is the default one that is shown on the drawn UI (for instance,
    along the right side of a dropdown menu). Since each shortcut can only represent a single key (plus modifiers), it
    is sometimes useful to associate multiple keyboard shortcuts with the same menu item (e.g. to allow zooming with the
    '+' key on the number row of the keyboard, as well as the '+' on the number pad).

    These additional shortcuts are not currently visible directly in the UI. In the future, the plan is to also allow
    the drawn UI to display alternate shortcuts based on what modifiers are currently being pressed (e.g. show "Close"
    when only the command key is pressed, but "Close all" when the shift key is also pressed).
*/
class NANOGUI_EXPORT MenuItem : public Button
{
public:
    MenuItem(Widget *parent, const std::string &caption = "Untitled", int button_icon = 0,
             const std::vector<Shortcut> &s = {{0, 0}});

    size_t                       num_shortcuts() const { return m_shortcuts.size(); }
    /// Shortcut \p i, or an empty one when the item has none.  draw() and
    /// preferred_text_size() call shortcut() unconditionally, so an item built
    /// with an empty shortcut list must not throw on its first draw.
    const Shortcut              &shortcut(size_t i = 0) const {
        static const Shortcut none;   // Shortcut(0, 0) has empty text
        return i < m_shortcuts.size() ? m_shortcuts[i] : none;
    }
    const std::vector<Shortcut> &shortcuts() const { return m_shortcuts; }

    bool mouse_enter_event(const Vector2i &p, bool enter) override;

    /// Whether or not this MenuItem is currently highlighted.
    bool highlighted() const { return m_highlighted; }
    /// Sets whether or not this MenuItem is currently highlighted.
    void set_highlighted(bool highlight, bool unhighlight_siblings = false, bool run_callbacks = false);

    /// Return the highlight callback
    std::function<void(bool)> highlight_callback() const { return m_highlight_callback; }
    /// Set the highlight callback
    void set_highlight_callback(const std::function<void(bool)> &callback) { m_highlight_callback = callback; }

    /* A mark drawn at the right of the row (the font menu's star). Clicking
     * it runs `callback` and does not choose the row. The callback is
     * deferred so it can rebuild the menu that owns this item. */
    void set_mark(bool on, const std::function<void()> &callback);
    void clear_mark();
    bool has_mark() const { return m_has_mark; }
    bool mark_on() const { return m_mark_on; }
    bool over_mark(const Vector2i &p) const;
    bool mark_armed() const { return m_mark_armed; }
    void arm_mark() { m_mark_armed = m_has_mark; }
    void disarm_mark() { m_mark_armed = false; }
    void release_mark(bool fire);

    virtual void     draw(NVGcontext *ctx) override;
    Vector2i         preferred_text_size(NVGcontext *ctx) const;
    virtual Vector2i preferred_size(NVGcontext *ctx) const override;

protected:
    std::vector<Shortcut> m_shortcuts;

    /// Whether or not this MenuItem is currently highlighted.
    bool m_highlighted = false;

    /// The callback issued for all types of buttons.
    std::function<void(bool)> m_highlight_callback;

    bool m_has_mark = false;
    bool m_mark_on = false;
    bool m_mark_armed = false;
    std::function<void()> m_mark_callback;

    /// Memoized preferred_text_size(); Dropdown::preferred_size() calls this
    /// for every item in its popup, so it is the hottest measurement here.
    mutable TextSizeCache m_text_size_cache;
};

class NANOGUI_EXPORT Separator : public MenuItem
{
public:
    Separator(Widget *parent);
    virtual void draw(NVGcontext *ctx) override;
};

/// The popup window containing the menu
class NANOGUI_EXPORT PopupMenu : public Popup
{
public:
    /// Create a new popup menu parented to a screen (first argument) and a parent window (if applicable)
    PopupMenu(Screen *screen, Window *parent_window, MenuItem *parent_item, bool exclusive);

    MenuItem       *parent_item() { return m_parent_item; }
    const MenuItem *parent_item() const { return m_parent_item; }

    /// Returns the idx-th item in the menu
    MenuItem *item(int idx) const;

    void set_highlighted_index(int idx);

    /// For PopupMenus with mutually exclusive items, returns the currently selected index.
    int selected_index() const { return m_selected_idx; }

    /// For PopupMenus with mutually exclusive items, set the selected index.
    void set_selected_index(int idx);

    /// The callback to execute when an item is selected.
    std::function<void(int)> selected_callback() const { return m_selected_callback; }

    /// Sets the callback to execute when an item is selected
    void set_selected_callback(const std::function<void(int)> &callback) { m_selected_callback = callback; }

    /// Content size. Window::preferred_size freezes once the popup has a size,
    /// which would stop a refilled menu from growing and hide the overflow.
    virtual Vector2i preferred_size(NVGcontext *ctx) const override;

    /// Lay items out at their natural height, then shift them by the scroll offset.
    virtual void perform_layout(NVGcontext *ctx) override;

    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override;
    virtual bool keyboard_character_event(unsigned int codepoint) override;
    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override;
    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override;
    virtual bool scroll_event(const Vector2i &p, const Vector2f &rel) override;
    virtual Widget *find_widget(const Vector2i &p) override;
    virtual const Widget *find_widget(const Vector2i &p) const override;

    /// Scroll so item \p index is inside the popup. Child positions must already be laid out.
    void reveal(int index);
    void set_scroll(int y) { m_scroll = y < 0 ? 0 : y; }
    int scroll() const { return m_scroll; }

    /// Draw the popup window
    virtual void draw(NVGcontext *ctx) override;

protected:
    int max_scroll() const;
    bool over_scrollbar(const Vector2i &p) const;
    void scroll_from_track(int local_y);

    MenuItem                *m_parent_item = nullptr; ///< Parent MenuItem that this Popup is spawned from.
    bool                     m_exclusive   = false;   ///< Whether the items are mutually exclusive
    std::function<void(int)> m_selected_callback;     ///< The callback to execute when an item is selected.
    int m_selected_idx    = -1; ///< For mutually exclusive popups, the index of the currently selected item.
    int m_highlighted_idx = -1; ///< The index of the currently hovered/highlighted item
    int m_scroll = 0;           ///< Pixels the items are shifted up when the menu is taller than the screen.
    bool m_scrollbar_drag = false;
};

/// A ComboBox or Menubar menu
class NANOGUI_EXPORT Dropdown : public MenuItem
{
public:
    enum Mode : int
    {
        ComboBox,
        Menu,
        Submenu
    };

    /// Create an empty combo box
    Dropdown(Widget *parent, Mode mode = ComboBox, const std::string &caption = "Untitled");

    /**
     * \brief Create a new combo box with the given items, providing long names and optionally short names and icons for
     * each item
     */
    Dropdown(Widget *parent, const std::vector<std::string> &items, const std::vector<int> &icons = {},
             Mode mode = ComboBox, const std::string &caption = "Untitled");
    ~Dropdown();

    Mode mode() const { return m_mode; }
    void set_mode(Mode mode) { m_mode = mode; }

    MenuItem *add_item(const std::string &caption, int icon = 0, const std::vector<Shortcut> &s = {{0, 0}});
	MenuItem *add_item(const std::pair<std::string, std::string> &item_data, int icon,
					   const std::function<void()> &callback, const std::vector<Shortcut> &shortcuts,
					   				   bool visible = true);
    Dropdown *add_submenu(const std::string &caption, int icon = 0);
	void remove_item(int index);

    /// Remove every entry. Does not change the caption or run a callback.
    void clear_items();

    /// Replace every entry. Does not run the selected callback.
    /// The first item is selected when \p items is not empty.
    void set_items(const std::vector<std::string> &items);

    /* Fired when a click or key is about to open the popup, before it is
     * sized. Must not call set_pushed. */
    void set_open_callback(const std::function<void()> &callback) { m_open_callback = callback; }
    /* Fired when the popup closes. Must not rebuild the popup: set_pushed
     * also runs from draw once the popup is no longer visible. */
    void set_close_callback(const std::function<void()> &callback) { m_close_callback = callback; }
    /* While the popup is open, typed characters, Backspace (codepoint 0)
     * and Escape (0x1B) are forwarded here. Return false from Escape when
     * the menu itself should close. */
    void set_query_callback(const std::function<bool(unsigned)> &callback) { m_query_callback = callback; }
    bool has_query_callback() const { return (bool) m_query_callback; }
    bool query_event(unsigned codepoint) { return m_query_callback && m_query_callback(codepoint); }

    /// The current index this Dropdown has selected.
    int selected_index() const { return m_popup->selected_index(); }

    /// Sets the current index this Dropdown has selected.
    void set_selected_index(int idx)
    {
        m_popup->set_selected_index(idx);
        if (auto i = m_popup->item(selected_index()))
            set_caption(i->caption());
    }

    /// The callback to execute for this Dropdown.
    std::function<void(int)> selected_callback() const { return m_popup->selected_callback(); }

    /// Sets the callback to execute for this Dropdown.
    void set_selected_callback(const std::function<void(int)> &callback)
    {
        m_popup->set_selected_callback(
            [this, callback](int idx)
            {
                callback(idx);
                if (auto i = m_popup->item(selected_index()))
                    set_caption(i->caption());
            });
    }

    PopupMenu       *popup() { return m_popup; }
    const PopupMenu *popup() const { return m_popup; }

    virtual Vector2i preferred_size(NVGcontext *ctx) const override;
    virtual void     draw(NVGcontext *ctx) override;
    virtual bool     mouse_enter_event(const Vector2i &p, bool enter) override;
    virtual bool     mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override;
	virtual bool     keyboard_event(int key, int scancode, int action, int modifiers) override;
    virtual void     set_pushed(bool pushed) override;

protected:
    void update_popup_geometry() const;
    /// On open, bring the selected combo row into the viewport.
    void align_open_scroll();

    PopupMenu *m_popup = nullptr;

    Mode m_mode = ComboBox;
    std::function<void()> m_open_callback, m_close_callback;
    std::function<bool(unsigned)> m_query_callback;
};

/// A horizontal menu bar containing a row of Dropdown menu items and responsible for handling their hotkeys
class NANOGUI_EXPORT MenuBar : public Window
{
public:
    MenuBar(Widget *parent, const std::string &title = "Untitled");

    Dropdown *add_menu(const std::string &name);

    /// Return the menu item specified by menu_path.
    MenuItem *find_item(const std::vector<std::string> &menu_path, bool throw_on_fail = true) const;

    bool process_shortcuts(int modifiers, int key);

    /// Sync the menubar skin from the parent theme (light/dark aware).
    virtual void set_theme(Theme *theme) override;

    /// Prefer a compact strip height (not half the parent — MenuBar is chrome).
    /// When parented to a Screen, width tracks the full screen width.
    virtual Vector2i preferred_size(NVGcontext *ctx) const override;

    virtual void perform_layout(NVGcontext *ctx) override;
    virtual void draw(NVGcontext *ctx) override;

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override;

protected:
    /// Rebuild m_menu_theme from the source palette + menubar chrome overrides.
    void sync_menubar_theme(Theme *source);

    /// Pin geometry to a full-width strip on top of the parent (typically Screen).
    void sync_strip_geometry();

    /// Compact bar height for the active theme/font.
    int strip_height() const;

    ref<Theme> m_menu_theme;
};

/// Wrap another widget with a right-click popup menu
class NANOGUI_EXPORT PopupWrapper : public Widget
{
public:
    PopupWrapper(Widget *parent);

    PopupMenu       *popup() { return m_popup; }
    const PopupMenu *popup() const { return m_popup; }

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override;

protected:
    PopupMenu *m_popup = nullptr;
};

NAMESPACE_END(nanogui)
