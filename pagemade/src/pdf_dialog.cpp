/*
 * pagemade/pdf_dialog.cpp — see pdf_dialog.h.
 */
#include "pdf_dialog.h"

#include "dialogs.h"
#include "pdf.h"
#include "spinbox.h"

#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menu.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace nanogui;
using pagemade::PdfOutputProfile;
using pagemade::PdfSettings;

namespace {

/* The FOGRA39 profile if this computer has one, else the built-in sRGB */
std::string fogra39(const std::vector<PdfOutputProfile> &profiles) {
    for (const PdfOutputProfile &p : profiles)
        if (p.condition == "FOGRA39")
            return p.path;
    return std::string();
}

Widget *row(Widget *parent) {
    Widget *r = new Widget(parent);
    r->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    return r;
}

Label *label(Widget *parent, const std::string &text, int width, const char *font = "sans-bold") {
    Label *l = new Label(parent, text, font);
    l->set_fixed_size(Vector2i(width, 0));
    return l;
}

Label *heading(Widget *parent, const std::string &text) {
    Label *l = new Label(parent, text, "sans-bold", 16);
    l->set_fixed_size(Vector2i(300, 0));
    return l;
}

Label *note(Widget *parent, const std::string &text, int width = 0) {
    Label *l = new Label(parent, text, "sans", 14);
    l->set_color(Color(110, 110, 110, 255));
    if (width > 0)
        l->set_fixed_size(Vector2i(width, 0));
    return l;
}

struct PdfForm {
    PdfSettings s;
    std::vector<PdfOutputProfile> profiles;
    int page_count = 1;
    bool syncing = false;

    Dropdown *preset = nullptr;
    Button *all = nullptr, *range = nullptr;
    IntSpin *first = nullptr, *last = nullptr;
    CheckBox *hidden = nullptr;
    CheckBox *downsample = nullptr;
    FloatSpin *above = nullptr, *to = nullptr;
    Dropdown *compression = nullptr;
    IntSpin *quality = nullptr;
    Dropdown *color = nullptr;
    Dropdown *standard = nullptr;
    Dropdown *intent = nullptr;
    CheckBox *bleed = nullptr, *marks = nullptr;
    CheckBox *flatten = nullptr;
    IntSpin *flatten_ppi = nullptr;
    TextBox *title = nullptr, *author = nullptr, *subject = nullptr;
    CheckBox *preflight = nullptr, *open_after = nullptr;
    IntSpin *min_ppi = nullptr;

    /* The widgets from `s` */
    void show() {
        syncing = true;
        const std::vector<std::string> &names = pdf_preset_names();
        auto p = std::find(names.begin(), names.end(), s.preset);
        preset->set_selected_index(p == names.end() ? (int) names.size() : (int) (p - names.begin()));
        all->set_pushed(s.all_pages);
        range->set_pushed(!s.all_pages);
        first->set_value(std::clamp(s.first_page, 1, page_count));
        last->set_value(std::clamp(s.last_page, 1, page_count));
        first->set_enabled(!s.all_pages);
        last->set_enabled(!s.all_pages);
        hidden->set_checked(s.include_hidden);
        downsample->set_checked(s.downsample);
        above->set_value(s.downsample_above);
        to->set_value(s.downsample_to);
        above->set_enabled(s.downsample);
        to->set_enabled(s.downsample);
        compression->set_selected_index((int) s.compression);
        quality->set_value(s.jpeg_quality);
        quality->set_enabled(s.compression != PdfSettings::Compression::Lossless);
        /* PDF/X-4 tags every color: the RGB choice is fixed while it's on */
        const PdfSettings::Color shown = s.pdfx4 && s.color == PdfSettings::Color::Rgb
                                       ? PdfSettings::Color::RgbTagged : s.color;
        color->set_selected_index((int) shown);
        color->set_enabled(!s.pdfx4);
        standard->set_selected_index(s.pdfx4 ? 1 : 0);
        int ii = 0;
        for (size_t i = 0; i < profiles.size(); ++i)
            if (profiles[i].path == s.output_intent)
                ii = (int) i;
        intent->set_selected_index(ii);
        intent->set_enabled(s.pdfx4);
        bleed->set_checked(s.bleed);
        marks->set_checked(s.crop_marks);
        flatten->set_checked(s.flatten);
        flatten_ppi->set_value((int) std::lround(s.flatten_ppi));
        flatten_ppi->set_enabled(s.flatten);
        title->set_value(s.title);
        author->set_value(s.author);
        subject->set_value(s.subject);
        preflight->set_checked(s.preflight);
        min_ppi->set_value((int) std::lround(s.min_ppi));
        min_ppi->set_enabled(s.preflight);
        open_after->set_checked(s.open_after);
        syncing = false;
    }

    /* A setting was changed by hand: the preset no longer describes it */
    void changed() {
        if (syncing)
            return;
        s.preset = "Custom";
        show();
    }
};

} // namespace

const std::vector<std::string> &pdf_preset_names() {
    static const std::vector<std::string> names = {
        "High quality print", "Prodigi photo book", "Press (PDF/X-4)", "Smallest file"};
    return names;
}

void apply_pdf_preset(const std::string &name, PdfSettings &s) {
    PdfSettings d;                       // the defaults: High quality print
    const std::string intent = fogra39(pagemade::pdf_output_profiles());
    s.preset = name;
    s.downsample = true;
    s.downsample_above = d.downsample_above;
    s.downsample_to = d.downsample_to;
    s.compression = PdfSettings::Compression::Automatic;
    s.jpeg_quality = d.jpeg_quality;
    s.color = PdfSettings::Color::Rgb;
    s.pdfx4 = false;
    s.output_intent.clear();
    s.bleed = true;
    s.crop_marks = false;
    s.flatten = false;
    s.flatten_ppi = d.flatten_ppi;
    s.min_ppi = d.min_ppi;
    if (name == "Prodigi photo book") {
        /* Prodigi (HP Indigo): PDF/X-4 against FOGRA39, RGB, no bleed or
         * marks (their system adds them), 300 ppi pictures. Transparency
         * stays live, as PDF/X-4 allows, so text and art stay vector and
         * pictures untouched; Flatten is there if a job is refused. */
        s.jpeg_quality = 95;
        s.color = PdfSettings::Color::RgbTagged;
        s.pdfx4 = true;
        s.output_intent = intent;
        s.bleed = false;
        s.min_ppi = 250.f;
    } else if (name == "Press (PDF/X-4)") {
        s.jpeg_quality = 95;
        s.color = PdfSettings::Color::RgbTagged;
        s.pdfx4 = true;
        s.output_intent = intent;
        s.crop_marks = true;
        s.min_ppi = 250.f;
    } else if (name == "Smallest file") {
        s.downsample_above = 225.f;
        s.downsample_to = 150.f;
        s.compression = PdfSettings::Compression::Jpeg;
        s.jpeg_quality = 75;
        s.bleed = false;
        s.min_ppi = 100.f;
    }
}

void ask_pdf_settings(Screen *screen, const PdfSettings &current, int page_count, float doc_bleed,
                      const std::string &ok_caption,
                      std::function<void(const PdfSettings &)> done) {
    auto st = std::make_shared<PdfForm>();
    st->s = current;
    st->page_count = std::max(1, page_count);
    st->profiles = pagemade::pdf_output_profiles();

    auto *d = new ModalDialog(screen, "PDF Settings");
    Widget *form = d->body();
    form->set_fixed_size(Vector2i(560, 0));

    /* ---- Preset */
    Widget *r = row(form);
    label(r, "Preset", 110);
    std::vector<std::string> presets = pdf_preset_names();
    presets.push_back("Custom");
    st->preset = new Dropdown(r, presets, {}, Dropdown::ComboBox, presets.front());
    st->preset->set_fixed_size(Vector2i(300, 28));
    st->preset->set_tooltip("A starting point. Changing any setting below makes it Custom.");
    st->preset->set_selected_callback([st](int i) {
        if (st->syncing || i < 0 || i >= (int) pdf_preset_names().size())
            return;
        apply_pdf_preset(pdf_preset_names()[(size_t) i], st->s);
        st->show();
    });

    /* ---- Pages */
    heading(form, "Pages");
    r = row(form);
    label(r, "", 110);
    st->all = new Button(r, "All");
    st->range = new Button(r, "Range");
    for (Button *b : {st->all, st->range}) {
        b->set_flags(Button::RadioButton | Button::ToggleButton);
        b->set_fixed_size(Vector2i(72, 28));
    }
    st->all->set_callback([st] { st->s.all_pages = true; st->show(); });
    st->range->set_callback([st] { st->s.all_pages = false; st->show(); });
    st->first = new IntSpin(r, 1, 1, 1, st->page_count);
    st->first->set_fixed_size(Vector2i(72, 28));
    note(r, "to");
    st->last = new IntSpin(r, st->page_count, 1, 1, st->page_count);
    st->last->set_fixed_size(Vector2i(72, 28));
    note(r, "of " + std::to_string(st->page_count));
    st->first->set_callback([st](int v) {
        st->s.first_page = v;
        st->s.last_page = std::max(st->s.last_page, v);
        st->show();
    });
    st->last->set_callback([st](int v) {
        st->s.last_page = v;
        st->s.first_page = std::min(st->s.first_page, v);
        st->show();
    });
    r = row(form);
    label(r, "", 110);
    st->hidden = new CheckBox(r, "Include hidden pages", [st](bool v) {
        st->s.include_hidden = v;
    });

    /* ---- Pictures */
    heading(form, "Pictures");
    r = row(form);
    label(r, "", 110);
    st->downsample = new CheckBox(r, "Downsample above", [st](bool v) {
        st->s.downsample = v;
        st->changed();
    });
    st->above = new FloatSpin(r, 450.f, 25.f, 72.f, 2400.f);
    st->above->number_format("%.0f");
    st->above->set_units("ppi");
    st->above->set_fixed_size(Vector2i(96, 28));
    note(r, "to");
    st->to = new FloatSpin(r, 300.f, 25.f, 72.f, 2400.f);
    st->to->number_format("%.0f");
    st->to->set_units("ppi");
    st->to->set_fixed_size(Vector2i(96, 28));
    st->above->set_tooltip("Pictures finer than this, as placed, are resampled. 450 leaves a "
                           "margin above 300 so pictures near it aren't touched.");
    st->above->set_callback([st](float v) { st->s.downsample_above = v; st->changed(); });
    st->to->set_callback([st](float v) { st->s.downsample_to = v; st->changed(); });
    r = row(form);
    label(r, "Compression", 110);
    st->compression = new Dropdown(r, {"Automatic", "JPEG", "Lossless"}, {}, Dropdown::ComboBox,
                                   "Automatic");
    st->compression->set_fixed_size(Vector2i(150, 28));
    st->compression->set_tooltip("Automatic: JPEG files keep their own data unless resampled, "
                                 "HEIC photos become JPEG, other pictures stay lossless.\n"
                                 "JPEG: every opaque picture is JPEG (smallest).\n"
                                 "Lossless: nothing adds JPEG loss (largest).");
    st->compression->set_selected_callback([st](int i) {
        st->s.compression = (PdfSettings::Compression) std::clamp(i, 0, 2);
        st->changed();
    });
    label(r, "JPEG quality", 100);
    st->quality = new IntSpin(r, 92, 1, 10, 100);
    st->quality->set_fixed_size(Vector2i(80, 28));
    st->quality->set_callback([st](int v) { st->s.jpeg_quality = v; st->changed(); });

    /* ---- Color */
    heading(form, "Color");
    r = row(form);
    label(r, "Color", 110);
    st->color = new Dropdown(r, {"RGB, pictures keep their profiles", "RGB, every color tagged (sRGB)",
                                 "CMYK, converted for press (coming later)"},
                             {}, Dropdown::ComboBox, "RGB");
    st->color->set_fixed_size(Vector2i(360, 28));
    if (PopupMenu *pop = st->color->popup())
        if (MenuItem *cmyk = pop->item(2))
            cmyk->set_enabled(false);
    st->color->set_selected_callback([st](int i) {
        st->s.color = i == 1 ? PdfSettings::Color::RgbTagged : PdfSettings::Color::Rgb;
        st->changed();
    });
    r = row(form);
    label(r, "Standard", 110);
    st->standard = new Dropdown(r, {"None (PDF 1.7)", "PDF/X-4"}, {}, Dropdown::ComboBox, "None");
    st->standard->set_fixed_size(Vector2i(160, 28));
    st->standard->set_tooltip("PDF/X-4 is what print services ask for: tagged color, an output "
                              "intent, trim and bleed boxes, and identification metadata.");
    st->standard->set_selected_callback([st](int i) {
        st->s.pdfx4 = i == 1;
        if (st->s.pdfx4 && st->s.output_intent.empty())
            st->s.output_intent = fogra39(st->profiles);
        st->changed();
    });
    r = row(form);
    label(r, "Output intent", 110);
    std::vector<std::string> intents;
    for (const PdfOutputProfile &p : st->profiles)
        intents.push_back(p.name);
    st->intent = new Dropdown(r, intents, {}, Dropdown::ComboBox, intents.front());
    st->intent->set_fixed_size(Vector2i(360, 28));
    st->intent->set_tooltip("The printing condition the PDF is made for (PDF/X-4). The printer "
                            "converts the RGB to it. Coated FOGRA39 is the usual one in Europe; "
                            "GRACoL in the US.");
    st->intent->set_selected_callback([st](int i) {
        if (i >= 0 && i < (int) st->profiles.size())
            st->s.output_intent = st->profiles[(size_t) i].path;
        st->changed();
    });

    /* ---- Marks and bleed */
    heading(form, "Marks and Bleed");
    r = row(form);
    label(r, "", 110);
    char bleed_text[64];
    std::snprintf(bleed_text, sizeof bleed_text, "Include bleed (%.3g in, from Document Setup)",
                  (double) (doc_bleed / 72.f));
    st->bleed = new CheckBox(r, bleed_text, [st](bool v) { st->s.bleed = v; st->changed(); });
    r = row(form);
    label(r, "", 110);
    st->marks = new CheckBox(r, "Crop marks", [st](bool v) { st->s.crop_marks = v; st->changed(); });
    note(r, "outside the bleed");

    /* ---- Transparency */
    heading(form, "Transparency");
    r = row(form);
    label(r, "", 110);
    st->flatten = new CheckBox(r, "Flatten at", [st](bool v) { st->s.flatten = v; st->changed(); });
    st->flatten->set_tooltip("Pages with anything see-through (PNG alpha, opacity, fading gradients) "
                             "are written as one picture. Other pages stay vector.");
    st->flatten_ppi = new IntSpin(r, 300, 10, 72, 1200);
    st->flatten_ppi->set_units("ppi");
    st->flatten_ppi->set_fixed_size(Vector2i(96, 28));
    st->flatten_ppi->set_callback([st](int v) { st->s.flatten_ppi = (float) v; st->changed(); });
    note(r, "for printers that ask for it");

    /* ---- Document */
    heading(form, "Document");
    auto text_row = [&](const char *name, TextBox *&box, std::string PdfSettings::*field) {
        Widget *tr = row(form);
        label(tr, name, 110);
        box = new TextBox(tr, "");
        box->set_editable(true);
        box->set_alignment(TextBox::Alignment::Left);
        box->set_fixed_size(Vector2i(360, 28));
        box->set_callback([st, field](const std::string &v) {
            st->s.*field = v;
            return true;
        });
    };
    text_row("Title", st->title, &PdfSettings::title);
    text_row("Author", st->author, &PdfSettings::author);
    text_row("Subject", st->subject, &PdfSettings::subject);

    /* ---- Before and after */
    heading(form, "Before and After");
    r = row(form);
    label(r, "", 110);
    st->preflight = new CheckBox(r, "Check first; report pictures below", [st](bool v) {
        st->s.preflight = v;
        st->show();
    });
    st->min_ppi = new IntSpin(r, 200, 10, 50, 1200);
    st->min_ppi->set_units("ppi");
    st->min_ppi->set_fixed_size(Vector2i(96, 28));
    st->min_ppi->set_callback([st](int v) { st->s.min_ppi = (float) v; st->changed(); });
    r = row(form);
    label(r, "", 110);
    st->open_after = new CheckBox(r, "Open the PDF when it's written", [st](bool v) {
        st->s.open_after = v;
    });

    st->show();
    d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
    d->add_button(ok_caption, [st, done] {
        if (done)
            done(st->s);
    }, ModalDialog::Role::Default);
    d->open();
}
