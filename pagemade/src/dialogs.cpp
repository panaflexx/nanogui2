/*
 * pagemade/dialogs.cpp — see dialogs.h.
 */
#include "dialogs.h"

#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanovg.h>
#include <GLFW/glfw3.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>

using namespace nanogui;
namespace fs = std::filesystem;

namespace {

int s_open_dialogs = 0;

const Color kDefaultButton(56, 116, 226, 255);
const Color kHintText(110, 110, 110, 255);

std::string folder_of(const std::string &path) {
    return fs::path(path).parent_path().string();
}

std::string file_name_of(const std::string &path) {
    return fs::path(path).filename().string();
}

std::string default_folder() {
    if (const char *home = std::getenv("HOME")) {
        fs::path docs = fs::path(home) / "Documents";
        std::error_code ec;
        if (fs::is_directory(docs, ec))
            return docs.string();
        return home;
    }
    std::error_code ec;
    return fs::current_path(ec).string();
}

std::string ellipsize_left(const std::string &s, size_t max) {
    if (s.size() <= max)
        return s;
    return "…" + s.substr(s.size() - (max - 1));
}

/* Warning or information icon, then a bold headline and a gray note. */
void message_body(ModalDialog *d, int icon, const std::string &headline, const std::string &note,
                  int width) {
    Widget *row = new Widget(d->body());
    row->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Minimum, 0, 14));
    Label *glyph = new Label(row, std::string(utf8(icon).data()), "icons");
    glyph->set_font_size(44);
    Widget *text = new Widget(row);
    text->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Minimum, 0, 6));
    Label *head = new Label(text, headline, "sans-bold", 18);
    head->set_fixed_size(Vector2i(width, 0));
    if (!note.empty()) {
        Label *small = new Label(text, note, "sans", 15);
        small->set_fixed_size(Vector2i(width, 0));
        small->set_color(kHintText);
    }
}

void open_save_as(Screen *screen, const std::string &current_path, const std::string &folder,
                  const std::string &name, bool embed,
                  std::function<void(const std::string &, bool)> done);

} // namespace

/* ---- ModalDialog ----------------------------------------------------------- */

ModalDialog::ModalDialog(Screen *screen, const std::string &title) : Window(screen, title) {
    ++s_open_dialogs;
    set_modal(true);
    /* Everything right-aligned: the body has a fixed width, so only the
     * button row visibly moves to the right edge. */
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Maximum, 18, 16));
    m_body = new Widget(this);
    m_body->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Minimum, 0, 10));
    m_buttons = new Widget(this);
    m_buttons->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
}

ModalDialog::~ModalDialog() {
    if (!m_closing)
        --s_open_dialogs;
}

bool ModalDialog::any_open() {
    return s_open_dialogs > 0;
}

Button *ModalDialog::add_button(const std::string &caption, std::function<void()> action, Role role) {
    Button *b = new Button(m_buttons, caption);
    b->set_fixed_size(Vector2i(std::max(96, (int) caption.size() * 9 + 24), 30));
    b->set_callback([this, b] { press(b); });
    m_actions.push_back({b, std::move(action)});
    if (role == Role::Default || role == Role::DefaultCancel) {
        m_default = b;
        b->set_background_color(kDefaultButton);
        b->set_text_color(Color(255, 255, 255, 255));
    }
    if (role == Role::Cancel || role == Role::DefaultCancel)
        m_cancel = b;
    return b;
}

void ModalDialog::add_shortcut(int key, Button *button) {
    m_shortcuts.push_back({key, button});
}

void ModalDialog::open() {
    center();                            // sizes and lays out a new window
    request_focus();
}

void ModalDialog::commit_text_boxes(Widget *w) {
    for (Widget *child : w->children()) {
        if (auto *tb = dynamic_cast<TextBox *>(child))
            if (tb->focused())
                tb->focus_event(false);  // a box commits its text when it loses focus
        commit_text_boxes(child);
    }
}

void ModalDialog::press(Button *button) {
    if (m_closing || !button)
        return;
    commit_text_boxes(this);
    std::function<void()> fn;
    for (const Action &a : m_actions)
        if (a.button == button)
            fn = a.fn;
    m_closing = true;
    --s_open_dialogs;
    dispose();
    if (fn)
        fn();                            // may open the next dialog
}

bool ModalDialog::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE && m_cancel) {
            press(m_cancel);
            return true;
        }
        if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) && m_default) {
            press(m_default);
            return true;
        }
        if (modifiers & SYSTEM_COMMAND_MOD)
            for (const auto &sc : m_shortcuts)
                if (sc.first == key) {
                    press(sc.second);
                    return true;
                }
    }
    return Window::keyboard_event(key, scancode, action, modifiers);
}

void ModalDialog::draw(NVGcontext *ctx) {
    /* Dim the window behind: a modal dialog should look like one. */
    if (const Screen *s = screen()) {
        nvgSave(ctx);
        nvgResetScissor(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, (float) s->width(), (float) s->height());
        nvgFillColor(ctx, nvgRGBA(0, 0, 0, 70));
        nvgFill(ctx);
        nvgRestore(ctx);
    }
    Window::draw(ctx);
}

/* ---- The dialogs -------------------------------------------------------------- */

void ask_save_changes(Screen *screen, const std::string &doc_name,
                      std::function<void(SaveChoice)> done) {
    auto *d = new ModalDialog(screen, "");
    message_body(d, d->theme()->m_message_warning_icon,
                 "Save changes to “" + doc_name + "” before closing it?",
                 "Your changes will be lost if you don't save them.", 330);
    Button *dont = d->add_button("Don't Save", [done] { done(SaveChoice::DontSave); });
    d->add_button("Cancel", [done] { done(SaveChoice::Cancel); }, ModalDialog::Role::Cancel);
    d->add_button("Save", [done] { done(SaveChoice::Save); }, ModalDialog::Role::Default);
    d->add_shortcut(GLFW_KEY_D, dont);
    d->open();
}

void ask_replace(Screen *screen, const std::string &path, std::function<void(bool)> done) {
    auto *d = new ModalDialog(screen, "");
    message_body(d, d->theme()->m_message_warning_icon,
                 "“" + file_name_of(path) + "” already exists. Do you want to replace it?",
                 "Replacing it overwrites what is saved there now.", 330);
    d->add_button("Replace", [done] { done(true); });
    d->add_button("Cancel", [done] { done(false); }, ModalDialog::Role::DefaultCancel);
    d->open();
}

void show_alert(Screen *screen, const std::string &title, const std::string &message) {
    auto *d = new ModalDialog(screen, "");
    message_body(d, d->theme()->m_message_information_icon, title, message, 330);
    d->add_button("OK", {}, ModalDialog::Role::DefaultCancel);
    d->open();
}

void ask_save_as(Screen *screen, const std::string &current_path, bool embed_assets,
                 std::function<void(const std::string &, bool)> done) {
    const bool untitled = current_path.empty();
    open_save_as(screen, current_path,
                 untitled ? default_folder() : folder_of(current_path),
                 untitled ? std::string("Untitled.pagemade") : file_name_of(current_path),
                 embed_assets, std::move(done));
}

namespace {

void open_save_as(Screen *screen, const std::string &current_path, const std::string &folder,
                  const std::string &name, bool embed,
                  std::function<void(const std::string &, bool)> done) {
    struct State {
        std::string folder, name;
        bool embed;
    };
    auto st = std::make_shared<State>(State{folder, name, embed});

    auto *d = new ModalDialog(screen, "Save Publication");
    Widget *grid = new Widget(d->body());
    auto *layout = new GridLayout(Orientation::Horizontal, 2, Alignment::Middle, 0, 10);
    layout->set_col_alignment({Alignment::Maximum, Alignment::Fill});
    grid->set_layout(layout);

    new Label(grid, "Save as:", "sans-bold");
    TextBox *name_box = new TextBox(grid, st->name);
    name_box->set_editable(true);
    name_box->set_alignment(TextBox::Alignment::Left);
    name_box->set_fixed_size(Vector2i(340, 28));
    name_box->set_callback([st](const std::string &v) { st->name = v; return true; });

    new Label(grid, "In:", "sans-bold");
    Widget *where = new Widget(grid);
    where->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    Label *folder_label = new Label(where, ellipsize_left(st->folder, 34));
    folder_label->set_fixed_size(Vector2i(248, 0));
    folder_label->set_tooltip(st->folder);
    Button *browse = new Button(where, "Browse…");
    browse->set_fixed_size(Vector2i(84, 28));
    browse->set_callback([st, name_box, folder_label] {
        auto paths = file_dialog({{"pagemade", "pagemade publication"}}, true, false, st->folder);
        if (paths.empty() || paths[0].empty())
            return;
        st->folder = folder_of(paths[0]);
        st->name = file_name_of(paths[0]);
        name_box->set_value(st->name);
        folder_label->set_caption(ellipsize_left(st->folder, 34));
        folder_label->set_tooltip(st->folder);
    });

    CheckBox *embed_box = new CheckBox(d->body(), "Embed assets (images, SVGs, fonts)",
                                       [st](bool on) { st->embed = on; });
    embed_box->set_checked(st->embed);
    Label *hint = new Label(d->body(),
                            "Copies the fonts and pictures this publication uses into the "
                            "file, so it opens and prints the same on any computer. Without "
                            "it, the file links to them where they are.",
                            "sans", 14);
    hint->set_fixed_size(Vector2i(430, 0));
    hint->set_color(kHintText);

    d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
    d->add_button("Save", [screen, current_path, st, done] {
        std::string n = st->name;
        while (!n.empty() && std::isspace((unsigned char) n.back())) n.pop_back();
        while (!n.empty() && std::isspace((unsigned char) n.front())) n.erase(0, 1);
        if (n.empty())
            n = "Untitled";
        const std::string ext = std::string(".") + "pagemade";
        if (n.size() < ext.size() || n.compare(n.size() - ext.size(), ext.size(), ext) != 0)
            n += ext;
        const std::string full = (fs::path(st->folder) / n).string();
        std::error_code ec;
        if (full != current_path && fs::exists(full, ec)) {
            ask_replace(screen, full, [=](bool replace) {
                if (replace)
                    done(full, st->embed);
                else
                    open_save_as(screen, current_path, st->folder, n, st->embed, done);
            });
            return;
        }
        done(full, st->embed);
    }, ModalDialog::Role::Default);
    d->open();
    name_box->request_focus();
}

} // namespace
