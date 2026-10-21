/*
 * pagemade/print_dialog.cpp — see print_dialog.h.
 */
#include "print_dialog.h"

#include "dialogs.h"
#include "print.h"
#include "spinbox.h"

#include <nanogui/button.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menu.h>
#include <nanogui/screen.h>

#include <algorithm>
#include <memory>

using namespace nanogui;

namespace {

Widget *row(Widget *parent) {
    Widget *r = new Widget(parent);
    r->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    return r;
}

Label *label(Widget *parent, const std::string &text, int width) {
    Label *l = new Label(parent, text, "sans-bold");
    l->set_fixed_size(Vector2i(width, 0));
    return l;
}

Label *note(Widget *parent, const std::string &text) {
    Label *l = new Label(parent, text, "sans", 14);
    l->set_color(Color(110, 110, 110, 255));
    return l;
}

struct Form {
    PrintChoice choice;
    int page_count = 1;
    int current_page = 1;
    std::vector<pagemade::Printer> printers;

    Dropdown *printer = nullptr;
    IntSpin *copies = nullptr;
    Button *all = nullptr, *current = nullptr, *range = nullptr;
    IntSpin *first = nullptr, *last = nullptr;

    void show() {
        all->set_pushed(choice.pages == PrintPages::All);
        current->set_pushed(choice.pages == PrintPages::Current);
        range->set_pushed(choice.pages == PrintPages::Range);
        const bool ranged = choice.pages == PrintPages::Range;
        first->set_enabled(ranged);
        last->set_enabled(ranged);
        if (choice.pages == PrintPages::Current) {
            first->set_value(current_page);
            last->set_value(current_page);
        } else {
            first->set_value(choice.first_page);
            last->set_value(choice.last_page);
        }
    }

    PrintChoice take() const {
        PrintChoice c = choice;
        c.copies = std::clamp(copies->value(), 1, 99);
        const int index = printer ? printer->selected_index() : -1;
        if (index >= 0 && index < (int) printers.size())
            c.printer = printers[(size_t) index].name;
        if (c.pages == PrintPages::Current) {
            c.first_page = current_page;
            c.last_page = current_page;
        } else if (c.pages == PrintPages::Range) {
            c.first_page = std::clamp(first->value(), 1, page_count);
            c.last_page = std::clamp(last->value(), c.first_page, page_count);
        }
        return c;
    }
};

} // namespace

void ask_print(Screen *screen, const std::vector<pagemade::Printer> &printers, int page_count,
               int current_page, const PrintChoice &initial, bool available,
               const std::string &unavailable_reason, std::function<void(const PrintChoice &)> done) {
    auto st = std::make_shared<Form>();
    st->choice = initial;
    st->page_count = std::max(1, page_count);
    st->current_page = std::clamp(current_page, 1, st->page_count);
    st->choice.first_page = std::clamp(st->choice.first_page, 1, st->page_count);
    st->choice.last_page = std::clamp(st->choice.last_page, st->choice.first_page, st->page_count);
    st->choice.copies = std::clamp(st->choice.copies, 1, 99);
    st->printers = printers;
    if (st->printers.empty()) {
        pagemade::Printer fallback;
        fallback.label = "System default";
        fallback.is_default = true;
        st->printers.push_back(fallback);
    }

    auto *d = new ModalDialog(screen, "Print");
    Widget *form = d->body();
    form->set_fixed_size(Vector2i(480, 0));

    if (!available) {
        Label *why = new Label(form, unavailable_reason, "sans");
        why->set_fixed_size(Vector2i(460, 0));
        why->set_color(Color(160, 60, 40, 255));
    }

    Widget *r = row(form);
    label(r, "Printer", 90);
    std::vector<std::string> labels;
    int selected = 0;
    for (int i = 0; i < (int) st->printers.size(); ++i) {
        const pagemade::Printer &p = st->printers[(size_t) i];
        labels.push_back(p.label.empty() ? p.name : p.label);
        if (!st->choice.printer.empty() && p.name == st->choice.printer)
            selected = i;
        else if (st->choice.printer.empty() && p.is_default)
            selected = i;
    }
    st->printer = new Dropdown(r, labels, {}, Dropdown::ComboBox, labels[(size_t) selected]);
    st->printer->set_fixed_size(Vector2i(340, 28));
    st->printer->set_selected_index(selected);
    st->printer->set_enabled(available);

    r = row(form);
    label(r, "Copies", 90);
    st->copies = new IntSpin(r, st->choice.copies, 1, 1, 99);
    st->copies->set_fixed_size(Vector2i(72, 28));
    st->copies->set_enabled(available);

    r = row(form);
    label(r, "Pages", 90);
    st->all = new Button(r, "All");
    st->current = new Button(r, "Current");
    st->range = new Button(r, "Range");
    for (Button *b : {st->all, st->current, st->range}) {
        b->set_flags(Button::RadioButton | Button::ToggleButton);
        b->set_fixed_size(Vector2i(84, 28));
        b->set_enabled(available);
    }
    st->all->set_callback([st] { st->choice.pages = PrintPages::All; st->show(); });
    st->current->set_callback([st] { st->choice.pages = PrintPages::Current; st->show(); });
    st->range->set_callback([st] { st->choice.pages = PrintPages::Range; st->show(); });
    r = row(form);
    label(r, "", 90);
    st->first = new IntSpin(r, st->choice.first_page, 1, 1, st->page_count);
    st->first->set_fixed_size(Vector2i(72, 28));
    note(r, "to");
    st->last = new IntSpin(r, st->choice.last_page, 1, 1, st->page_count);
    st->last->set_fixed_size(Vector2i(72, 28));
    note(r, "of " + std::to_string(st->page_count));
    st->first->set_callback([st](int v) {
        st->choice.pages = PrintPages::Range;
        st->choice.first_page = v;
        st->choice.last_page = std::max(st->choice.last_page, v);
        st->show();
    });
    st->last->set_callback([st](int v) {
        st->choice.pages = PrintPages::Range;
        st->choice.last_page = v;
        st->choice.first_page = std::min(st->choice.first_page, v);
        st->show();
    });

    st->show();

    d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
    Button *print = d->add_button("Print", [st, done] {
        if (done)
            done(st->take());
    }, ModalDialog::Role::Default);
    print->set_enabled(available);
    d->open();
}
