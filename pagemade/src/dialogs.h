/*
 * pagemade/dialogs.h — modal dialogs: the save-changes prompt, Save
 * Publication (with the option to embed assets), replace confirmation and
 * alerts.
 *
 * A dialog dims the window behind it and is modal: clicks outside it are
 * ignored and it holds the keyboard. Return presses the default button and
 * Escape cancels; the save prompt also takes Cmd/Ctrl+D for Don't Save, as
 * on the Mac. Answers come back through callbacks that run after the dialog
 * has closed, so a callback may open the next dialog.
 */
#pragma once

#include <nanogui/window.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nanogui {
class Button;
class Screen;
class TextBox;
}

class ModalDialog : public nanogui::Window {
public:
    /* Default answers Return, Cancel answers Escape; DefaultCancel answers
     * both (the safe choice in Replace? and alerts). */
    enum class Role { Normal, Default, Cancel, DefaultCancel };

    ModalDialog(nanogui::Screen *screen, const std::string &title);
    ~ModalDialog() override;

    /* Where the dialog's content goes, above the buttons. */
    nanogui::Widget *body() { return m_body; }
    /* Buttons line up on the right in the order added. The action runs
     * after the dialog closes. */
    nanogui::Button *add_button(const std::string &caption, std::function<void()> action,
                                Role role = Role::Normal);
    /* Cmd/Ctrl + key presses `button`. */
    void add_shortcut(int key, nanogui::Button *button);
    /* Lay out, center over the window, take the keyboard. */
    void open();

    /* True while any dialog is up: the app ignores its own shortcuts. */
    static bool any_open();

    void draw(NVGcontext *ctx) override;
    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

private:
    struct Action { nanogui::Button *button; std::function<void()> fn; };
    void press(nanogui::Button *button);
    void commit_text_boxes(nanogui::Widget *w);

    nanogui::Widget *m_body = nullptr;
    nanogui::Widget *m_buttons = nullptr;
    std::vector<Action> m_actions;
    std::vector<std::pair<int, nanogui::Button *>> m_shortcuts;
    nanogui::Button *m_default = nullptr;
    nanogui::Button *m_cancel = nullptr;
    bool m_closing = false;
};

enum class SaveChoice { Save, DontSave, Cancel };

/* "Save changes to “name” before closing it?" — Save, Don't Save, Cancel. */
void ask_save_changes(nanogui::Screen *screen, const std::string &doc_name,
                      std::function<void(SaveChoice)> done);

/* PageMaker's Save Publication dialog: the file name, the folder (Browse…
 * opens the system file chooser) and "Embed assets (images, SVGs, fonts)".
 * Asks before replacing a file other than `current_path`. `done` gets the
 * full path (with the .pagemade extension) and the embed choice; it isn't
 * called if the user cancels. */
void ask_save_as(nanogui::Screen *screen, const std::string &current_path, bool embed_assets,
                 std::function<void(const std::string &path, bool embed_assets)> done);

/* "“name” already exists. Do you want to replace it?" */
void ask_replace(nanogui::Screen *screen, const std::string &path, std::function<void(bool)> done);

void show_alert(nanogui::Screen *screen, const std::string &title, const std::string &message);

/* A one-field prompt ("New Style", "Name:"). `done` gets the trimmed text;
 * it isn't called on Cancel or when the text is empty. */
void ask_text(nanogui::Screen *screen, const std::string &title, const std::string &label,
              const std::string &initial, const std::string &hint,
              std::function<void(const std::string &)> done);

namespace pagemade {
class FontLibrary;
class FontMenuModel;
struct CharStyle;
}

/* Floating Type panel: the same character fields as PageMaker's Type
 * Specifications, with a preview. It is not modal — the page stays live,
 * and the panel can be dragged by its title bar. Apply writes the edited
 * fields onto the current selection; closing drops unapplied edits.
 * `apply` receives the style the panel was loaded from and the edited one. */
class TypeSpecsPanel : public nanogui::Window {
public:
    TypeSpecsPanel(nanogui::Screen *screen, pagemade::FontLibrary &fonts,
                   pagemade::FontMenuModel &fonts_menu,
                   std::function<void(const pagemade::CharStyle &before,
                                      const pagemade::CharStyle &after)> apply);
    ~TypeSpecsPanel() override;

    /* Show `style`. A mixed selection leaves the font caption blank until
       the user picks one, and says that Apply changes only edited fields. */
    void load(const pagemade::CharStyle &style, bool mixed,
              const std::vector<std::string> &document_fonts);
    void set_apply_enabled(bool enabled);

    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down,
                            int modifiers) override;

private:
    struct State;
    std::shared_ptr<State> m_state;
    std::function<void()> m_refresh;
    void relayout();
};
