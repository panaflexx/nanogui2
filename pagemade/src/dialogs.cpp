/*
 * pagemade/dialogs.cpp — see dialogs.h.
 */
#include "dialogs.h"

#include "composer/composer.h"
#include "composer/font.h"
#include "font_menu_ui.h"
#include "render_nvg.h"
#include "spinbox.h"

#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/menu.h>
#include <nanogui/layout.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanovg.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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

namespace {

using pagemade::Caps;
using pagemade::CharStyle;
using pagemade::FaceDesc;
using pagemade::FontLibrary;
using pagemade::FontMenuModel;
using pagemade::FontMenuView;
using pagemade::GlyphRun;
using pagemade::PlacedGlyph;
using pagemade::font_menu_query_edit;
using pagemade::refill_font_menu;

class TypePreview : public Widget {
public:
    TypePreview(Widget *parent, FontLibrary *fonts) : Widget(parent), m_fonts(fonts) {}

    void set_style(const CharStyle &cs) {
        m_style = cs;
        m_dirty = true;
    }

    void draw(NVGcontext *ctx) override {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, (float) m_pos.x(), (float) m_pos.y(),
                       (float) m_size.x(), (float) m_size.y(), 4.f);
        nvgFillColor(ctx, nvgRGB(255, 255, 255));
        nvgFill(ctx);
        nvgStrokeColor(ctx, nvgRGB(186, 186, 186));
        nvgStrokeWidth(ctx, 1.f);
        nvgStroke(ctx);
        if (m_dirty && m_fonts) {
            m_runs.clear();
            pagemade::Story story;
            pagemade::Paragraph para;
            CharStyle show = m_style;
            if (!(show.size > 0.f))
                show.size = 12.f;
            para.runs.push_back({show, "The quick brown fox jumps over the lazy dog."});
            story.paragraphs.push_back(para);
            pagemade::Composition comp = pagemade::compose(
                story, {{0, 0, 8000, 800}}, *m_fonts, nullptr);
            if (!comp.lines.empty())
                m_runs = comp.lines.front().runs;
            m_dirty = false;
        }
        float minx = 0, maxx = 0, base = 0;
        bool any = false;
        for (const GlyphRun &r : m_runs)
            for (const PlacedGlyph &g : r.glyphs) {
                if (g.flags & PlacedGlyph::Invisible)
                    continue;
                const float right = g.x + std::max(0.f, g.adv);
                if (!any) { minx = g.x; maxx = right; base = g.y; any = true; }
                else { minx = std::min(minx, g.x); maxx = std::max(maxx, right); }
            }
        if (!any)
            return;
        /* Grow the line until it fills the box, and stop at about 34pt so a
         * display size doesn't overflow the preview. */
        const float avail = std::max(1.f, (float) m_size.x() - 20.f);
        float sc = avail / std::max(1.f, maxx - minx);
        const float vis = m_style.size * sc;
        if (vis > 34.f)
            sc *= 34.f / vis;
        nvgSave(ctx);
        nvgIntersectScissor(ctx, (float) m_pos.x() + 1.f, (float) m_pos.y() + 1.f,
                            (float) m_size.x() - 2.f, (float) m_size.y() - 2.f);
        nvgTranslate(ctx, (float) m_pos.x() + 10.f - minx * sc,
                     (float) m_pos.y() + (float) m_size.y() * 0.68f - base * sc);
        nvgScale(ctx, sc, sc);
        for (const GlyphRun &r : m_runs)
            pagemade::draw_glyph_run(ctx, r);
        nvgRestore(ctx);
    }

private:
    FontLibrary *m_fonts = nullptr;
    CharStyle m_style;
    std::vector<GlyphRun> m_runs;
    bool m_dirty = true;
};

struct WeightRow {
    int weight = 400;
    std::string width, label, roman;
};

} // namespace

void commit_text_boxes(Widget *w) {
    for (Widget *child : w->children()) {
        if (auto *tb = dynamic_cast<TextBox *>(child))
            if (tb->focused())
                tb->focus_event(false);
        commit_text_boxes(child);
    }
}

struct TypeSpecsPanel::State {
    CharStyle style, before;
    FontLibrary *fonts = nullptr;
    FontMenuModel *fonts_menu = nullptr;
    TypePreview *preview = nullptr;
    Label *hint = nullptr, *face_name = nullptr;
    Dropdown *family = nullptr, *weight = nullptr, *slant = nullptr, *caps = nullptr;
    FloatBox<float> *size_box = nullptr, *leading = nullptr;
    FloatBox<float> *hscale = nullptr, *tracking = nullptr, *baseline = nullptr;
    CheckBox *underline = nullptr, *strike = nullptr, *kerning = nullptr, *ligatures = nullptr;
    Button *apply_btn = nullptr;
    std::function<void(const CharStyle &, const CharStyle &)> apply;
    std::vector<std::string> document, slants;
    std::string query;
    std::vector<WeightRow> rows;
    bool updating = false;
    bool family_unset = false;
};

TypeSpecsPanel::TypeSpecsPanel(Screen *screen, FontLibrary &fonts, FontMenuModel &fonts_menu,
                               std::function<void(const CharStyle &, const CharStyle &)> apply)
    : Window(screen, "Type", false) {
    auto st = std::make_shared<State>();
    m_state = st;
    st->fonts = &fonts;
    st->fonts_menu = &fonts_menu;
    st->apply = std::move(apply);

    /* A palette, not a dialog: no dimmer, no traffic lights, no resize grip.
       The title bar is the drag handle. The X lives in the button panel. */
    set_modal(false);
    set_traffic_lights(false);
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Maximum, 14, 10));
    Widget *form = new Widget(this);
    form->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 8));

    st->hint = new Label(form,
                         "The selection uses more than one style. Apply changes only what you edit.",
                         "sans", 14);
    st->hint->set_fixed_size(Vector2i(460, 0));
    st->hint->set_color(Color(110, 110, 110, 255));
    st->hint->set_visible(false);

    st->preview = new TypePreview(form, &fonts);
    st->preview->set_fixed_size(Vector2i(460, 86));
    st->face_name = new Label(form, "", "sans", 14);
    st->face_name->set_color(Color(110, 110, 110, 255));

    Widget *grid = new Widget(form);
    auto *layout = new GridLayout(Orientation::Horizontal, 2, Alignment::Middle, 0, 8);
    layout->set_col_alignment({Alignment::Maximum, Alignment::Fill});
    grid->set_layout(layout);

    auto refresh_preview = [st] {
        std::string name = st->style.family;
        if (!st->style.face.empty())
            name += "   " + st->style.face;
        st->face_name->set_caption(name);
        st->preview->set_style(st->style);
    };
    auto describe_now = [st](int &weight, bool &italic, std::string &width) {
        weight = st->style.bold ? 700 : 400;
        italic = st->style.italic;
        width.clear();
        if (!st->style.face.empty() &&
            st->fonts->describe(st->style.family, st->style.face, weight, italic))
            width = FontLibrary::width_of(st->style.face);
    };
    auto slant_prefer = [st] {
        if (!st->slant)
            return std::string();
        const int i = st->slant->selected_index();
        if (i < 0 || i >= (int) st->slants.size() || st->slants[(size_t) i] == "Roman")
            return std::string();
        return st->slants[(size_t) i];
    };
    auto take_face = [st](const std::string &face) {
        st->style.face = face;
        int w = 400;
        bool it = false;
        if (st->fonts->describe(st->style.family, face, w, it)) {
            st->style.bold = w >= 600;
            st->style.italic = it;
        }
    };
    auto apply_match = [st, take_face](int weight, bool italic, const std::string &width,
                                       const std::string &prefer) {
        std::string chosen;
        st->fonts->match(st->style.family, weight, italic, width, &chosen, prefer);
        if (!chosen.empty())
            take_face(chosen);
    };

    auto rebuild_weights = [st] {
        st->rows.clear();
        const std::vector<FaceDesc> faces = st->fonts->faces(st->style.family);
        for (const FaceDesc &f : faces) {
            if (f.italic)
                continue;
            WeightRow row;
            row.weight = f.weight;
            row.width = FontLibrary::width_of(f.style);
            row.label = f.style;
            row.roman = f.style;
            st->rows.push_back(row);
        }
        for (const FaceDesc &f : faces) {
            if (!f.italic)
                continue;
            const std::string width = FontLibrary::width_of(f.style);
            bool have = false;
            for (const WeightRow &r : st->rows)
                have |= r.weight == f.weight && r.width == width;
            if (have)
                continue;
            WeightRow row;
            row.weight = f.weight;
            row.width = width;
            row.label = f.style;
            st->rows.push_back(row);
        }
        if (st->rows.empty()) {
            WeightRow row;
            row.label = "Regular";
            row.roman = "Regular";
            st->rows.push_back(row);
        }
        std::vector<std::string> labels;
        for (const WeightRow &r : st->rows)
            labels.push_back(r.label);
        int cur_w = st->style.bold ? 700 : 400;
        bool cur_i = false;
        std::string cur_width;
        if (!st->style.face.empty())
            st->fonts->describe(st->style.family, st->style.face, cur_w, cur_i);
        if (!st->style.face.empty())
            cur_width = FontLibrary::width_of(st->style.face);
        int best = 0, best_score = 100000;
        for (int i = 0; i < (int) st->rows.size(); ++i) {
            if ((!st->rows[(size_t) i].roman.empty() && st->rows[(size_t) i].roman == st->style.face) ||
                st->rows[(size_t) i].label == st->style.face) {
                best = i;
                break;
            }
            int score = std::abs(st->rows[(size_t) i].weight - cur_w);
            if (st->rows[(size_t) i].width != cur_width)
                score += 80;
            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
        st->updating = true;
        st->weight->set_items(labels);
        st->weight->set_selected_index(best);
        st->updating = false;
    };
    auto rebuild_slants = [st] {
        const std::vector<FaceDesc> faces = st->fonts->faces(st->style.family);
        bool roman = faces.empty(), italic = false, oblique = false;
        for (const FaceDesc &f : faces) {
            if (!f.italic)
                roman = true;
            if (f.style.find("Oblique") != std::string::npos)
                oblique = true;
            else if (f.italic)
                italic = true;
        }
        st->slants.clear();
        if (roman)
            st->slants.push_back("Roman");
        if (italic)
            st->slants.push_back("Italic");
        if (oblique)
            st->slants.push_back("Oblique");
        if (st->slants.empty())
            st->slants.push_back("Roman");
        std::string want = "Roman";
        if (st->style.face.find("Oblique") != std::string::npos)
            want = "Oblique";
        else if (st->style.italic)
            want = "Italic";
        int idx = 0;
        for (int i = 0; i < (int) st->slants.size(); ++i)
            if (st->slants[(size_t) i] == want)
                idx = i;
        st->updating = true;
        st->slant->set_items(st->slants);
        st->slant->set_selected_index(idx);
        st->updating = false;
    };

    new Label(grid, "Font", "sans-bold");
    st->family = new Dropdown(grid, Dropdown::ComboBox, "Serif");
    st->family->set_min_size(Vector2i(220, 28));
    st->family->set_max_size(Vector2i(320, 36));
    st->family->set_tooltip("Font family. Type to search all fonts. Click the star to keep a favorite.");

    new Label(grid, "Weight", "sans-bold");
    Widget *ws = new Widget(grid);
    ws->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 8));
    st->weight = new Dropdown(ws, {"Regular"}, {}, Dropdown::ComboBox, "Regular");
    st->weight->set_min_size(Vector2i(140, 28));
    st->weight->set_max_size(Vector2i(240, 36));
    new Label(ws, "Style", "sans-bold");
    st->slant = new Dropdown(ws, {"Roman"}, {}, Dropdown::ComboBox, "Roman");
    st->slant->set_min_size(Vector2i(100, 28));
    st->slant->set_max_size(Vector2i(150, 36));

    new Label(grid, "Caps", "sans-bold");
    st->caps = new Dropdown(grid, {"Normal", "Small Caps", "All Caps"}, {},
                            Dropdown::ComboBox, "Normal");
    st->caps->set_fixed_size(Vector2i(160, 28));
    st->caps->set_selected_index(st->style.caps == Caps::Small ? 1 : st->style.caps == Caps::All ? 2 : 0);

    /* The number fields spin (spinbox.h). */
    auto metric = [&](const char *label, float value, float step, float lo, float hi,
                      const char *tip, const std::function<void(float)> &set) {
        new Label(grid, label, "sans-bold");
        auto *box = new FloatSpin(grid, value, step, lo, hi);
        box->number_format("%.2f");
        box->set_fixed_size(Vector2i(120, 28));
        box->set_tooltip(tip);
        box->TextBox::set_callback([box, set](const std::string &s) {
            char *end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (end == s.c_str())
                return false;
            set(v);
            box->set_value(v);
            return true;
        });
        return box;
    };

    st->size_box = metric("Size", 12.f, 1.f, 1.f, 720.f, "Type size, in points",
                          [st, refresh_preview](float v) {
        st->style.size = std::clamp(v, 1.f, 720.f);
        refresh_preview();
    });
    new Label(grid, "Leading", "sans-bold");
    auto *leading = new FloatSpin(grid, 0.f, 1.f, 0.f, 1300.f);
    leading->set_blank_value([st] {
        return st->style.leading > 0.f ? st->style.leading
                                       : st->style.size * pagemade::ParaStyle().autoleading * 0.01f;
    });
    leading->number_format("%.2f");
    st->leading = leading;
    st->leading->set_format("([Aa]uto)|([-+]?[0-9]*\\.?[0-9]+)");
    st->leading->set_fixed_size(Vector2i(120, 28));
    st->leading->set_tooltip("Line height. Auto (or 0) is proportional leading.");
    st->leading->TextBox::set_value("Auto");
    st->leading->TextBox::set_callback([st, refresh_preview](const std::string &s) {
        FloatBox<float> *lead = st->leading;
        float v = 0;
        if (!s.empty() && s != "Auto" && s != "auto") {
            char *end = nullptr;
            v = std::strtof(s.c_str(), &end);
            if (end == s.c_str())
                return false;
        }
        v = std::max(0.f, v);
        st->style.leading = v;
        if (v <= 0.f)
            lead->TextBox::set_value("Auto");
        else
            lead->set_value(v);
        refresh_preview();
        return true;
    });

    Widget *pair = new Widget(form);
    auto *pair_layout = new GridLayout(Orientation::Horizontal, 4, Alignment::Middle, 0, 8);
    pair_layout->set_col_alignment(
        {Alignment::Maximum, Alignment::Fill, Alignment::Maximum, Alignment::Fill});
    pair->set_layout(pair_layout);
    auto small_metric = [&](const char *label, float value, float step, float lo, float hi,
                            const char *tip,
                            const std::function<void(float)> &set) -> FloatBox<float> * {
        new Label(pair, label, "sans-bold");
        auto *box = new FloatSpin(pair, value, step, lo, hi);
        box->number_format("%.2f");
        box->set_fixed_size(Vector2i(90, 28));
        box->set_tooltip(tip);
        box->TextBox::set_callback([box, set](const std::string &s) {
            char *end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (end == s.c_str())
                return false;
            set(v);
            box->set_value(v);
            return true;
        });
        return box;
    };
    st->hscale = small_metric("Set width", 100.f, 1.f, 1.f, 1000.f, "Horizontal scale, percent",
                              [st, refresh_preview](float v) {
        st->style.hscale = std::clamp(v, 1.f, 1000.f);
        refresh_preview();
    });
    st->tracking = small_metric("Track", 0.f, 10.f, -500.f, 1000.f, "Tracking, thousandths of an em",
                                [st, refresh_preview](float v) {
        st->style.tracking = std::clamp(v, -500.f, 1000.f);
        refresh_preview();
    });
    st->baseline = small_metric("Baseline", 0.f, 0.5f, -500.f, 500.f,
                                "Baseline shift. Positive raises the type.",
                                [st, refresh_preview](float v) {
        st->style.baseline_shift = std::clamp(v, -500.f, 500.f);
        refresh_preview();
    });

    Widget *checks = new Widget(form);
    checks->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 16));
    auto check = [&](const char *caption, const std::function<void(bool)> &set) {
        auto *box = new CheckBox(checks, caption, [set, refresh_preview](bool v) {
            set(v);
            refresh_preview();
        });
        return box;
    };
    st->underline = check("Underline", [st](bool v) { st->style.underline = v; });
    st->strike = check("Strikethrough", [st](bool v) { st->style.strike = v; });
    st->kerning = check("Kerning", [st](bool v) { st->style.kerning = v; });
    st->ligatures = check("Ligatures", [st](bool v) { st->style.ligatures = v; });

    auto refill_holder = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> refill_weak = refill_holder;
    *refill_holder = [st, refill_weak, apply_match, describe_now, rebuild_weights, rebuild_slants,
                      refresh_preview, slant_prefer] {
        FontMenuView view;
        view.model = st->fonts_menu;
        view.installed = st->fonts->families();
        if (view.installed.empty())
            view.installed.push_back(st->style.family.empty() ? "Serif" : st->style.family);
        view.document = st->document;
        view.query = st->query;
        view.current = st->family_unset ? std::string() : st->style.family;
        view.mixed = st->family_unset;
        refill_font_menu(
            st->family, view,
            [st, apply_match, describe_now, rebuild_weights, rebuild_slants, refresh_preview,
             slant_prefer](const std::string &fam) {
                if (st->updating || fam.empty())
                    return;
                st->family_unset = false;
                int weight = 400;
                bool italic = false;
                std::string width;
                describe_now(weight, italic, width);
                const std::string prefer = slant_prefer();
                st->style.family = fam;
                st->fonts_menu->note_use(fam);
                apply_match(weight, italic, width, prefer);
                rebuild_weights();
                rebuild_slants();
                refresh_preview();
            },
            [st, refill_weak](const std::string &fam) {
                st->fonts_menu->toggle_star(fam);
                if (st->family && st->family->popup() && st->family->popup()->visible())
                    if (auto again = refill_weak.lock())
                        (*again)();
            });
    };
    st->family->set_open_callback([st, refill_holder] {
        st->query.clear();
        (*refill_holder)();
    });
    st->family->set_close_callback([st] { st->query.clear(); });
    st->family->set_query_callback([st, refill_holder](unsigned codepoint) {
        if (!font_menu_query_edit(st->query, codepoint))
            return false;
        (*refill_holder)();
        return true;
    });
    st->weight->set_selected_callback([st, take_face, apply_match, refresh_preview](int i) {
        if (st->updating || i < 0 || i >= (int) st->rows.size())
            return;
        const WeightRow &row = st->rows[(size_t) i];
        const int si = st->slant->selected_index();
        const bool italic = si >= 0 && si < (int) st->slants.size() && st->slants[(size_t) si] != "Roman";
        const std::string prefer = italic ? st->slants[(size_t) si] : std::string();
        if (!italic && !row.roman.empty())
            take_face(row.roman);
        else
            apply_match(row.weight, italic, row.width, prefer);
        refresh_preview();
    });
    st->slant->set_selected_callback([st, take_face, apply_match, refresh_preview](int i) {
        if (st->updating || i < 0 || i >= (int) st->slants.size())
            return;
        const int wi = st->weight->selected_index();
        if (wi < 0 || wi >= (int) st->rows.size())
            return;
        const WeightRow &row = st->rows[(size_t) wi];
        const bool italic = st->slants[(size_t) i] != "Roman";
        const std::string prefer = italic ? st->slants[(size_t) i] : std::string();
        if (!italic && !row.roman.empty())
            take_face(row.roman);
        else
            apply_match(row.weight, italic, row.width, prefer);
        refresh_preview();
    });
    st->caps->set_selected_callback([st, refresh_preview](int i) {
        if (st->updating)
            return;
        st->style.caps = i == 1 ? Caps::Small : i == 2 ? Caps::All : Caps::Normal;
        refresh_preview();
    });

    m_refresh = [refill_holder, rebuild_weights, rebuild_slants, refresh_preview] {
        (*refill_holder)();
        rebuild_weights();
        rebuild_slants();
        refresh_preview();
    };

    Widget *actions = new Widget(this);
    actions->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 0));
    st->apply_btn = new Button(actions, "Apply");
    st->apply_btn->set_fixed_size(Vector2i(88, 28));
    st->apply_btn->set_tooltip("Change the selected text");
    st->apply_btn->set_callback([this] {
        commit_text_boxes(this);
        /* Copy first. Apply restyles the selection, which reloads this panel. */
        const CharStyle before = m_state->before;
        const CharStyle after = m_state->style;
        if (m_state->apply)
            m_state->apply(before, after);
    });

    Button *close = new Button(button_panel(), "", FA_TIMES);
    close->set_tooltip("Close");
    close->set_transparent(true);
    close->set_callback([this] {
        std::function<void()> cb = close_callback();
        if (cb)
            cb();
        async([this] {
            if (parent())
                dispose();
        });
    });
}

TypeSpecsPanel::~TypeSpecsPanel() = default;

bool TypeSpecsPanel::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    /* A title-bar double-click would maximize the palette. Keep it floating. */
    const bool handled = Window::mouse_button_event(p, button, down, modifiers);
    m_last_title_click = 0;
    return handled;
}

void TypeSpecsPanel::set_apply_enabled(bool enabled) {
    if (m_state && m_state->apply_btn)
        m_state->apply_btn->set_enabled(enabled);
}

void TypeSpecsPanel::relayout() {
    Screen *s = screen();
    if (!s || !s->nvg_context() || size() == Vector2i(0, 0))
        return;
    NVGcontext *ctx = s->nvg_context();
    const Vector2i pref = preferred_size(ctx);
    if (pref.x() > 40 && pref.y() > 40 && pref != size())
        set_size(pref);
    perform_layout(ctx);
    s->redraw();
}

void TypeSpecsPanel::load(const CharStyle &style, bool mixed,
                          const std::vector<std::string> &document_fonts) {
    if (!m_state)
        return;
    State &st = *m_state;
    auto popup_open = [](Dropdown *d) {
        return d && d->popup() && d->popup()->visible();
    };
    if (popup_open(st.family) || popup_open(st.weight) || popup_open(st.slant) ||
        popup_open(st.caps))
        return;

    st.updating = true;
    st.before = style;
    st.style = style;
    st.family_unset = mixed;
    st.document = document_fonts;
    st.query.clear();
    if (st.hint)
        st.hint->set_visible(mixed);
    if (st.size_box)
        st.size_box->set_value(style.size);
    if (st.leading) {
        if (style.leading <= 0.f)
            st.leading->TextBox::set_value("Auto");
        else
            st.leading->set_value(style.leading);
    }
    if (st.hscale)
        st.hscale->set_value(style.hscale);
    if (st.tracking)
        st.tracking->set_value(style.tracking);
    if (st.baseline)
        st.baseline->set_value(style.baseline_shift);
    if (st.underline)
        st.underline->set_checked(style.underline);
    if (st.strike)
        st.strike->set_checked(style.strike);
    if (st.kerning)
        st.kerning->set_checked(style.kerning);
    if (st.ligatures)
        st.ligatures->set_checked(style.ligatures);
    if (st.caps)
        st.caps->set_selected_index(style.caps == Caps::Small ? 1 :
                                    style.caps == Caps::All ? 2 : 0);
    st.updating = false;
    if (m_refresh)
        m_refresh();
    relayout();
}
