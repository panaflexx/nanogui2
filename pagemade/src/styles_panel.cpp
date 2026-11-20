/*
 * pagemade/styles_panel.cpp — see styles_panel.h.
 */
#include "styles_panel.h"

#include "composerview.h"
#include "dialogs.h"
#include "font_menu.h"
#include "paragraph_panel.h"
#include "render_nvg.h"

#include <GLFW/glfw3.h>
#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menu.h>
#include <nanogui/screen.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanovg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

using namespace nanogui;
using pagemade::CharStyle;
using pagemade::CharStyleDef;
using pagemade::ComposedLine;
using pagemade::GlyphRun;
using pagemade::PageDoc;
using pagemade::Paragraph;
using pagemade::ParaStyle;
using pagemade::Run;
using pagemade::StyleDef;

namespace {

constexpr int kMargin = 8, kGap = 6, kRowH = 34, kFieldH = 26;
const char *const kSample = "The quick brown fox jumps over the lazy dog";

/* ---- Previews ------------------------------------------------------------ */

/* Lines set by the composer, drawn scaled into a box. */
struct Preview {
    std::vector<ComposedLine> lines;
    float scale = 1.f;
};

/* Set `story` with its largest type at about `px` pixels (between lo and
 * hi), in a measure of `width` pixels (0: one long line). */
Preview set_preview(const pagemade::FontLibrary &fonts, const pagemade::Story &story,
                    float width, float lo, float hi) {
    Preview pv;
    float size = 0.f;
    for (const Paragraph &p : story.paragraphs)
        for (const Run &r : p.runs)
            size = std::max(size, r.style.size);
    if (!(size > 0.f))
        size = 12.f;
    pv.scale = std::clamp(size, lo, hi) / size;
    const float w = width > 0.f ? width / pv.scale : 1e4f;
    pagemade::Composition comp = pagemade::compose(story, {{0, 0, w, 1e5f}}, fonts, nullptr);
    pv.lines = std::move(comp.lines);
    return pv;
}

/* What a list row shows: one line, set left, without the space around. */
pagemade::Story row_story(const StyleDef &d, const std::vector<CharStyleDef> &chars) {
    Paragraph p;
    p.runs.push_back(Run{d.type, kSample});
    pagemade::apply_style(p, d, chars);
    ParaStyle &ps = p.style;
    ps.align = pagemade::Align::Left;    // one long line: anything else lands far off
    ps.left_indent = ps.right_indent = ps.first_indent = 0.f;
    ps.space_before = ps.space_after = 0.f;
    ps.drop_lines = 0;
    for (Run &r : p.runs)
        r.style.leading = 0.f;
    pagemade::Story s;
    s.paragraphs.push_back(std::move(p));
    return s;
}

/* Normal's type, with the character style laid over it */
CharStyle char_over_normal(const PageDoc &doc, const CharStyleDef &def) {
    CharStyle cs;
    if (const StyleDef *n = doc.find_style("Normal"))
        cs = n->type;
    cs.char_style.clear();
    pagemade::apply_char_style(cs, def);
    return cs;
}

CharStyle normal_type(const PageDoc &doc) {
    CharStyle cs;
    if (const StyleDef *n = doc.find_style("Normal"))
        cs = n->type;
    cs.char_style.clear();
    return cs;
}

/* Draw the lines in the box (x, y, w, h): the first line's band centered
 * when `center`, else from the top. White behind, like the page. */
void draw_preview(NVGcontext *ctx, const PageDoc &doc, const Preview &pv, float x, float y,
                  float w, float h, float pad, bool center, size_t max_lines) {
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, x, y, w, h, 3.f);
    nvgFillColor(ctx, nvgRGB(255, 255, 255));
    nvgFill(ctx);
    nvgStrokeColor(ctx, nvgRGB(190, 190, 190));
    nvgStrokeWidth(ctx, 1.f);
    nvgStroke(ctx);
    if (pv.lines.empty())
        return;
    nvgSave(ctx);
    nvgIntersectScissor(ctx, x + 1.f, y + 1.f, w - 2.f, h - 2.f);
    const ComposedLine &first = pv.lines.front();
    float oy;
    if (center)
        oy = y + (h - first.leading * pv.scale) * 0.5f - first.top * pv.scale;
    else
        oy = y + pad - first.top * pv.scale;
    nvgTranslate(ctx, x + pad, oy);
    nvgScale(ctx, pv.scale, pv.scale);
    const size_t n = std::min(max_lines, pv.lines.size());
    for (size_t i = 0; i < n; ++i)
        for (const GlyphRun &r : pv.lines[i].runs) {
            pagemade::Color c = doc.text_color(r.swatch, r.tint, r.color);
            if (r.swatch == 0 && c.r == 0 && c.g == 0 && c.b == 0)
                c = pagemade::Color{0.f, 0.f, 0.f, 1.f};
            pagemade::draw_glyph_run(ctx, r, c);
        }
    nvgRestore(ctx);
}

/* ---- Small widgets --------------------------------------------------------- */

/* A text box that reports each keystroke, for the filter */
class LiveTextBox : public TextBox {
public:
    explicit LiveTextBox(Widget *parent) : TextBox(parent, "") {
        set_editable(true);
        set_alignment(TextBox::Alignment::Left);
    }
    std::function<void(const std::string &)> on_edit;
    /* The text as typed so far, committed or not */
    std::string text() const { return focused() ? m_value_temp : value(); }

    bool keyboard_character_event(unsigned int codepoint) override {
        const bool r = TextBox::keyboard_character_event(codepoint);
        if (on_edit)
            on_edit(text());
        return r;
    }
    bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        const bool r = TextBox::keyboard_event(key, scancode, action, modifiers);
        if (on_edit && (action == GLFW_PRESS || action == GLFW_REPEAT))
            on_edit(text());
        return r;
    }
};

/* The edit page's preview: a few lines at the style's size */
class EditPreview : public Widget {
public:
    EditPreview(Widget *parent) : Widget(parent) {}
    const PageDoc *doc = nullptr;
    Preview pv;
    void draw(NVGcontext *ctx) override {
        if (doc)
            draw_preview(ctx, *doc, pv, (float) m_pos.x(), (float) m_pos.y(), (float) m_size.x(),
                         (float) m_size.y(), 8.f, false, 6);
    }
    Vector2i preferred_size(NVGcontext *) const override { return Vector2i(100, 84); }
};

/* ---- The list -------------------------------------------------------------- */

class StyleList : public Widget {
public:
    struct Row {
        std::string name;
        int used = 0;
        bool current = false;        // the caret's style
        bool warn = false;           // a near-duplicate of another name
        bool fixed = false;          // Normal: no merge or remove
        const Preview *preview = nullptr;
    };
    enum class Part { None, Row, Edit, Merge, Remove };

    explicit StyleList(Widget *parent) : Widget(parent) {}

    const PageDoc *doc = nullptr;
    std::vector<Row> rows;
    int selected = -1;
    std::function<void(int)> on_select, on_edit, on_merge, on_remove, on_apply;

    Vector2i preferred_size(NVGcontext *) const override {
        return Vector2i(200, std::max(kRowH, (int) rows.size() * kRowH) + 2);
    }

    void draw(NVGcontext *ctx) override {
        const float x = (float) m_pos.x(), y = (float) m_pos.y(), w = (float) m_size.x();
        const Theme *th = theme();
        auto nvg = [](const Color &c) { return nvgRGBAf(c.r(), c.g(), c.b(), c.w()); };
        const NVGcolor text = th ? nvg(th->m_text_color) : nvgRGB(30, 30, 30);
        const NVGcolor dim = th ? nvg(th->m_disabled_text_color) : nvgRGB(140, 140, 140);
        const Geometry g = geometry();
        if (rows.empty()) {
            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, 14.f);
            nvgFillColor(ctx, dim);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgText(ctx, x + 8.f, y + 8.f, "No styles match.", nullptr);
            return;
        }
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row &r = rows[i];
            const float ry = y + (float) i * kRowH;
            if ((int) i == selected || (int) i == m_hover) {
                nvgBeginPath(ctx);
                nvgRect(ctx, x, ry, w, (float) kRowH);
                nvgFillColor(ctx, (int) i == selected ? nvgRGBA(70, 130, 230, 70)
                                                      : nvgRGBA(128, 128, 128, 30));
                nvgFill(ctx);
            }
            const float mid = ry + kRowH * 0.5f;
            /* ▸ the caret's style */
            if (r.current) {
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, x + 3.f, mid - 4.f);
                nvgLineTo(ctx, x + 8.f, mid);
                nvgLineTo(ctx, x + 3.f, mid + 4.f);
                nvgClosePath(ctx);
                nvgFillColor(ctx, nvgRGB(60, 140, 230));
                nvgFill(ctx);
            }
            /* The name, cut to its column; ⚠ after a near-duplicate */
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x + 10.f, ry, g.name_w - 12.f, (float) kRowH);
            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, 14.f);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(ctx, text);
            const float end = nvgText(ctx, x + 11.f, mid, r.name.c_str(), nullptr);
            if (r.warn) {
                nvgFontFace(ctx, "icons");
                nvgFontSize(ctx, 12.f);
                nvgFillColor(ctx, nvgRGB(225, 160, 30));
                nvgText(ctx, end + 5.f, mid, utf8(FA_EXCLAMATION_TRIANGLE).c_str(), nullptr);
            }
            nvgRestore(ctx);
            /* The preview */
            if (r.preview && doc && g.preview_w > 24.f)
                draw_preview(ctx, *doc, *r.preview, x + g.preview_x, ry + 4.f, g.preview_w,
                             (float) kRowH - 8.f, 5.f, true, 1);
            /* Uses */
            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, 13.f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFillColor(ctx, r.used ? text : dim);
            nvgText(ctx, x + g.used_x + g.used_w - 2.f, mid, std::to_string(r.used).c_str(), nullptr);
            /* Edit, merge, remove */
            const int icons[3] = {FA_PEN, FA_COMPRESS_ARROWS_ALT, FA_TRASH_ALT};
            nvgFontFace(ctx, "icons");
            nvgFontSize(ctx, 14.f);
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            for (int k = 0; k < 3; ++k) {
                if (k > 0 && r.fixed)
                    continue;
                const bool hot = (int) i == m_hover && (int) m_hover_part == k + 2;
                const float cx = x + g.icons_x + (k + 0.5f) * kIcon;
                if (hot) {
                    nvgBeginPath(ctx);
                    nvgRoundedRect(ctx, cx - 11.f, mid - 11.f, 22.f, 22.f, 4.f);
                    nvgFillColor(ctx, nvgRGBA(128, 128, 128, 60));
                    nvgFill(ctx);
                }
                nvgFillColor(ctx, hot ? text : dim);
                nvgText(ctx, cx, mid, utf8(icons[k]).c_str(), nullptr);
            }
        }
    }

    bool mouse_button_event(const Vector2i &p, int button, bool down, int) override {
        if (button != GLFW_MOUSE_BUTTON_1 || !down)
            return true;
        int row = -1;
        const Part part = part_at(p - m_pos, row);
        if (row < 0)
            return true;
        const double now = glfwGetTime();
        const bool again = row == m_last_row && now - m_last_click < 0.4;
        m_last_click = now;
        m_last_row = row;
        selected = row;
        switch (part) {
        case Part::Edit:   if (on_edit) on_edit(row); break;
        case Part::Merge:  if (on_merge) on_merge(row); break;
        case Part::Remove: if (on_remove) on_remove(row); break;
        default:
            if (on_select)
                on_select(row);
            if (again && on_apply)
                on_apply(row);           // double-click: apply it
            break;
        }
        return true;
    }

    bool mouse_motion_event(const Vector2i &p, const Vector2i &, int, int) override {
        int row = -1;
        const Part part = part_at(p - m_pos, row);
        if (row != m_hover || part != m_hover_part) {
            m_hover = row;
            m_hover_part = part;
            switch (part) {
            case Part::Edit:   set_tooltip("Edit this style"); break;
            case Part::Merge:  set_tooltip("Merge into another style: its text takes that style, and this one goes"); break;
            case Part::Remove: set_tooltip("Remove this style"); break;
            case Part::Row:    set_tooltip("Click to choose; double-click to apply it to the selected text. The number is how often it is used."); break;
            default:           set_tooltip(""); break;
            }
        }
        return true;
    }

    bool mouse_enter_event(const Vector2i &p, bool enter) override {
        if (!enter) {
            m_hover = -1;
            m_hover_part = Part::None;
        }
        return Widget::mouse_enter_event(p, enter);
    }

private:
    static constexpr float kIcon = 24.f;
    /* The scroll bar overlays the right 8 px; 5 more keep it off the icons */
    static constexpr float kScrollRoom = 13.f;
    struct Geometry {
        float name_w, preview_x, preview_w, used_x, used_w, icons_x;
    };
    Geometry geometry() const {
        Geometry g;
        const float w = (float) m_size.x();
        g.icons_x = w - 3.f * kIcon - kScrollRoom;
        g.used_w = 30.f;
        g.used_x = g.icons_x - g.used_w - 2.f;
        g.name_w = std::clamp(w * 0.34f, 80.f, 160.f);
        g.preview_x = g.name_w + 2.f;
        g.preview_w = g.used_x - g.preview_x - 2.f;
        return g;
    }
    Part part_at(const Vector2i &local, int &row) const {
        row = -1;
        if (local.y() < 0 || local.x() < 0 || local.x() >= m_size.x())
            return Part::None;
        const int r = local.y() / kRowH;
        if (r >= (int) rows.size())
            return Part::None;
        row = r;
        const Geometry g = geometry();
        const float lx = (float) local.x();
        if (lx >= g.icons_x) {
            const int k = std::min(2, (int) ((lx - g.icons_x) / kIcon));
            if (k > 0 && rows[(size_t) r].fixed)
                return Part::Row;
            return k == 0 ? Part::Edit : k == 1 ? Part::Merge : Part::Remove;
        }
        return Part::Row;
    }

    int m_hover = -1;
    Part m_hover_part = Part::None;
    double m_last_click = -1.0;
    int m_last_row = -1;
};

/* What changed between two type specs */
uint32_t char_diff(const CharStyle &a, const CharStyle &b) {
    using namespace pagemade;
    uint32_t d = 0;
    if (a.family != b.family) d |= kCharFamily;
    if (a.face != b.face) d |= kCharFace;
    if (a.bold != b.bold) d |= kCharBold;
    if (a.italic != b.italic) d |= kCharItalic;
    if (a.size != b.size) d |= kCharSize;
    if (a.leading != b.leading) d |= kCharLeading;
    if (a.underline != b.underline) d |= kCharUnderline;
    if (a.strike != b.strike) d |= kCharStrike;
    if (a.caps != b.caps) d |= kCharCaps;
    if (a.tracking != b.tracking) d |= kCharTracking;
    if (a.hscale != b.hscale) d |= kCharHScale;
    if (a.baseline_shift != b.baseline_shift) d |= kCharBaseline;
    if (a.kerning != b.kerning) d |= kCharKerning;
    if (a.ligatures != b.ligatures) d |= kCharLigatures;
    if (a.no_break != b.no_break) d |= kCharNoBreak;
    return d;
}

/* "Body text 2" beside "Body text": a stem that is another style's name */
bool near_duplicate(const std::string &name, const std::vector<std::string> &names) {
    const size_t sp = name.find_last_of(' ');
    if (sp == std::string::npos || sp + 1 >= name.size())
        return false;
    for (size_t i = sp + 1; i < name.size(); ++i)
        if (!std::isdigit((unsigned char) name[i]))
            return false;
    const std::string stem = name.substr(0, sp);
    return std::find(names.begin(), names.end(), stem) != names.end();
}

std::string lower(std::string s) {
    for (char &c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string fmt_pt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(v * 100.f) / 100.f);
    return buf;
}

const char *align_name(pagemade::Align a) {
    switch (a) {
    case pagemade::Align::Left: return "left";
    case pagemade::Align::Center: return "centered";
    case pagemade::Align::Right: return "right";
    case pagemade::Align::ForceJustify: return "force justified";
    default: return "justified";
    }
}

const char *const kFieldNames[16] = {"Font", "Face", "Bold", "Italic", "Size", "Leading",
                                     "Color", "Underline", "Strike", "Caps", "Tracking",
                                     "Width", "Baseline", "Kerning", "Ligatures", "No break"};

} // namespace

/* ---- State ----------------------------------------------------------------- */

struct StylesPanel::State {
    StylesPanel *self = nullptr;
    ComposerView *view = nullptr;
    pagemade::FontLibrary *fonts = nullptr;
    bool character = false;
    std::string selected_name;           // the highlighted style
    bool in_use_only = false;
    std::string filter;

    /* Previews by style name, with the definition they were set from */
    std::map<std::string, std::pair<StyleDef, Preview>> para_previews;
    std::map<std::string, std::pair<CharStyleDef, Preview>> char_previews;

    /* The list page */
    Widget *list_page = nullptr;
    Widget *kind_row = nullptr;
    Button *kind_para = nullptr, *kind_char = nullptr;
    CheckBox *in_use = nullptr;
    LiveTextBox *filter_box = nullptr;
    ScrollPanel *scroll = nullptr;
    StyleList *list = nullptr;
    Widget *action = nullptr;            // merge into / remove: what its text becomes
    Label *action_label = nullptr;
    Widget *action_row = nullptr;
    Dropdown *action_target = nullptr;
    Button *action_ok = nullptr, *action_cancel = nullptr;
    std::vector<std::string> action_targets;   // "" = keep the formatting
    std::string action_style;
    bool action_merge = false;
    Label *summary = nullptr;
    Widget *add_row = nullptr;
    Button *add = nullptr, *add_based = nullptr, *apply = nullptr;

    /* The edit page */
    Widget *edit_page = nullptr;
    Widget *edit_head = nullptr, *name_row = nullptr, *next_row = nullptr, *pages_row = nullptr;
    Label *edit_title = nullptr, *edit_error = nullptr;
    LiveTextBox *name_box = nullptr;
    Dropdown *next = nullptr;
    std::vector<std::string> next_names;     // "" first: the same style
    EditPreview *edit_preview = nullptr;
    Button *page_type = nullptr, *page_para = nullptr;
    TypeSpecsPanel *type_specs = nullptr;
    ScrollPanel *para_scroll = nullptr, *sets_scroll = nullptr;
    ParagraphPanel *para_panel = nullptr;
    Widget *sets = nullptr;
    CheckBox *field_box[16] = {};
    Dropdown *color = nullptr;
    std::vector<pagemade::SwatchId> color_ids;
    Widget *edit_bottom = nullptr;
    Button *edit_ok = nullptr, *edit_cancel = nullptr;
    bool editing = false, edit_new = false, edit_char = false, show_type = true;
    std::string edit_orig;
    StyleDef draft;
    CharStyleDef cdraft;
    Paragraph proxy;                     // the draft, for the Paragraph page's load()
    bool loading = false;
    /* A paragraph style's edits show in the layout as they are made, as
     * one undo step (merged under `live`'s address). Cancel takes it back. */
    StyleDef original;                   // the style as the editor opened it
    StyleDef applied;                    // what the layout shows now
    bool live = false;

    /* What the Type page shows for the draft: a character style's size
     * reads Auto while the style doesn't set it */
    CharStyle shown_type() const {
        if (!edit_char)
            return draft.type;
        CharStyle t = cdraft.type;
        if (!(cdraft.fields & pagemade::kCharSize))
            t.size = 0.f;
        return t;
    }
    void reload_type_page() {
        type_specs->load(shown_type(), false, pagemade::document_font_families(doc()));
    }

    /* Put the draft on the layout (an existing paragraph style; the name
     * waits for OK) */
    void live_apply() {
        if (edit_char || edit_new || loading || !doc().find_style(edit_orig))
            return;
        StyleDef d = draft;
        d.name = edit_orig;
        d.para.name = edit_orig;
        view->set_undo_merge(&live);
        if (view->update_style(edit_orig, d)) {
            live = true;
            if (const StyleDef *now = doc().find_style(edit_orig))
                applied = *now;
        }
        view->set_undo_merge(nullptr);
    }

    /* Cancel: the layout as it was. When something else changed the
     * document since (an undo, typing), the style is put back only if it
     * is still what the editor last applied. */
    void revert_live() {
        if (!live)
            return;
        live = false;
        /* Typing in the layout meanwhile splits the edits into more than
         * one step: the last one goes back, then the style is set back to
         * how it began */
        const bool reverted = view->revert_run(&live);
        if (const StyleDef *now = doc().find_style(edit_orig);
            now && !pagemade::same_style(*now, original) &&
            (reverted || pagemade::same_style(*now, applied)))
            view->update_style(edit_orig, original);
    }

    void cancel_editor() {
        revert_live();
        close_editor();
    }

    const PageDoc &doc() const { return view->document(); }

    /* ---- The list ---------------------------------------------------- */

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        if (character)
            for (const CharStyleDef &d : doc().char_styles)
                out.push_back(d.name);
        else
            for (const StyleDef &d : doc().styles)
                out.push_back(d.name);
        return out;
    }

    std::map<std::string, int> uses() const {
        std::map<std::string, int> n;
        for (const pagemade::StoryEntry &se : doc().stories)
            for (const Paragraph &p : se.story.paragraphs) {
                if (!character) {
                    if (!p.style.name.empty())
                        ++n[p.style.name];
                    continue;
                }
                std::string last;
                for (const Run &r : p.runs) {
                    if (!r.style.char_style.empty() && r.style.char_style != last)
                        ++n[r.style.char_style];
                    last = r.style.char_style;
                }
            }
        return n;
    }

    std::string current_style() const {
        if (character) {
            ComposerView::TypeStyle t;
            if (view->type_style(t) && !t.mix_char_style)
                return t.char_style;
            return {};
        }
        const auto paras = view->selected_paragraphs();
        return paras.empty() ? std::string() : paras.front()->style.name;
    }

    const Preview *preview_of(const std::string &name) {
        if (!fonts)
            return nullptr;
        if (character) {
            const CharStyleDef *d = doc().find_char_style(name);
            if (!d)
                return nullptr;
            auto it = char_previews.find(name);
            if (it != char_previews.end() && pagemade::same_char_style(it->second.first, *d))
                return &it->second.second;
            StyleDef base;
            if (const StyleDef *n = doc().find_style("Normal"))
                base = *n;
            pagemade::Story s = row_story(base, {});
            s.paragraphs.front().runs = {Run{char_over_normal(doc(), *d), kSample}};
            for (Run &r : s.paragraphs.front().runs)
                r.style.leading = 0.f;
            auto &slot = char_previews[name];
            slot = {*d, set_preview(*fonts, s, 0.f, 9.f, 16.f)};
            return &slot.second;
        }
        const StyleDef *d = doc().find_style(name);
        if (!d)
            return nullptr;
        auto it = para_previews.find(name);
        if (it != para_previews.end() && pagemade::same_style(it->second.first, *d) &&
            it->second.first.type.underline == d->type.underline)
            return &it->second.second;
        auto &slot = para_previews[name];
        slot = {*d, set_preview(*fonts, row_story(*d, doc().char_styles), 0.f, 9.f, 16.f)};
        return &slot.second;
    }

    void reload_list() {
        const std::vector<std::string> all = names();
        const auto used = uses();
        const std::string cur = current_style();
        const std::string f = lower(filter);
        list->doc = &doc();
        list->rows.clear();
        list->selected = -1;
        for (const std::string &n : all) {
            const auto u = used.find(n);
            const int count = u == used.end() ? 0 : u->second;
            if (in_use_only && count == 0)
                continue;
            if (!f.empty() && lower(n).find(f) == std::string::npos)
                continue;
            StyleList::Row r;
            r.name = n;
            r.used = count;
            r.current = n == cur;
            r.warn = near_duplicate(n, all);
            r.fixed = !character && n == "Normal";
            r.preview = preview_of(n);
            if (n == selected_name)
                list->selected = (int) list->rows.size();
            list->rows.push_back(std::move(r));
        }
        if (list->selected < 0 && !list->rows.empty()) {
            /* Nothing chosen yet: the caret's style, else the first */
            for (size_t i = 0; i < list->rows.size(); ++i)
                if (list->rows[i].current)
                    list->selected = (int) i;
            if (list->selected < 0)
                list->selected = 0;
            selected_name = list->rows[(size_t) list->selected].name;
        }
        if (list->selected < 0)
            selected_name.clear();
        /* Drop previews of styles that are gone */
        for (auto it = para_previews.begin(); it != para_previews.end();)
            it = doc().find_style(it->first) ? std::next(it) : para_previews.erase(it);
        for (auto it = char_previews.begin(); it != char_previews.end();)
            it = doc().find_char_style(it->first) ? std::next(it) : char_previews.erase(it);
        sync_buttons();
        relayout();
    }

    void sync_buttons() {
        kind_para->set_pushed(!character);
        kind_char->set_pushed(character);
        const bool have = !selected_name.empty();
        add_based->set_enabled(have);
        add_based->set_tooltip(have ? "New style starting from “" + selected_name + "”"
                                    : "Choose a style to start from");
        const bool text = character ? [&] {
            ComposerView::TypeStyle t;
            return view->type_style(t);
        }() : !view->selected_paragraphs().empty();
        apply->set_enabled(have && text);
        apply->set_tooltip(text ? "Set the selected text in the chosen style (or double-click a row)"
                                : "Select some text to apply a style to");
        add->set_tooltip(character
            ? (text ? "New character style from the selected text"
                    : "New character style (italic, to start)")
            : (text ? "New style from the selected paragraph"
                    : "New style, starting from Normal"));
        summary->set_caption(describe(selected_name));
    }

    std::string describe(const std::string &name) const {
        if (name.empty())
            return "";
        if (character) {
            const CharStyleDef *d = doc().find_char_style(name);
            if (!d)
                return "";
            std::string out = name + " — sets ";
            bool any = false;
            for (int b = 0; b < 16; ++b)
                if (d->fields & (1u << b)) {
                    out += std::string(any ? ", " : "") + kFieldNames[b];
                    any = true;
                }
            return any ? out : name + " — sets nothing";
        }
        const StyleDef *d = doc().find_style(name);
        if (!d)
            return "";
        const CharStyle &t = d->type;
        std::string out = name + " — " + t.family;
        if (!t.face.empty())
            out += " " + t.face;
        else if (t.bold || t.italic)
            out += std::string(t.bold ? " Bold" : "") + (t.italic ? " Italic" : "");
        out += " " + fmt_pt(t.size) + "/" + (t.leading > 0.f ? fmt_pt(t.leading) : std::string("auto"));
        out += ", " + std::string(align_name(d->para.align));
        if (d->para.space_before > 0.f)
            out += ", " + fmt_pt(d->para.space_before) + " pt before";
        if (d->para.space_after > 0.f)
            out += ", " + fmt_pt(d->para.space_after) + " pt after";
        if (!d->next.empty())
            out += "; next: " + d->next;
        return out;
    }

    void relayout() {
        if (Screen *s = self->screen())
            if (NVGcontext *ctx = s->nvg_context()) {
                self->perform_layout(ctx);
                s->redraw();
            }
    }

    /* ---- Merge and remove ---------------------------------------------- */

    void begin_action(const std::string &name, bool merge) {
        const auto used = uses();
        const auto u = used.find(name);
        const int count = u == used.end() ? 0 : u->second;
        if (!merge && count == 0) {
            finish(character ? view->merge_char_style(name, "") : view->merge_style(name, ""));
            return;
        }
        action_style = name;
        action_merge = merge;
        action_targets.clear();
        std::vector<std::string> items;
        if (!merge) {
            action_targets.push_back("");
            items.push_back("No style (keep the look)");
        }
        for (const std::string &n : names())
            if (n != name) {
                action_targets.push_back(n);
                items.push_back(n);
            }
        if (items.empty())
            return;
        action_target->set_items(items);
        /* Normal is the usual home for orphaned paragraphs */
        int pick = 0;
        for (size_t i = 0; i < action_targets.size(); ++i)
            if (!merge && action_targets[i] == "Normal")
                pick = (int) i;
        action_target->set_selected_index(pick);
        action_target->set_caption(items[(size_t) pick]);
        const std::string what = character ? (count == 1 ? "run" : "runs")
                                           : (count == 1 ? "paragraph" : "paragraphs");
        action_label->set_caption(merge
            ? "Merge “" + name + "” into another style. Its " + std::to_string(count) + " " +
                  what + " take that style, and “" + name + "” is removed."
            : "Remove “" + name + "”. Its " + std::to_string(count) + " " + what + " become:");
        action_ok->set_caption(merge ? "Merge" : "Remove");
        action->set_visible(true);
        relayout();
    }

    void end_action() {
        action->set_visible(false);
        action_style.clear();
        relayout();
    }

    void run_action() {
        const int i = action_target->selected_index();
        if (action_style.empty() || i < 0 || i >= (int) action_targets.size())
            return end_action();
        const std::string into = action_targets[(size_t) i];
        const std::string from = action_style;
        end_action();
        if (!into.empty())
            selected_name = into;
        finish(character ? view->merge_char_style(from, into) : view->merge_style(from, into));
    }

    void finish(bool) { reload_list(); }

    /* ---- The editor ---------------------------------------------------- */

    void open_editor(const std::string &name, bool is_new, const StyleDef *pdef,
                     const CharStyleDef *cdef) {
        end_action();
        editing = true;
        edit_new = is_new;
        edit_char = character;
        edit_orig = is_new ? std::string() : name;
        if (edit_char)
            cdraft = *cdef;
        else
            draft = *pdef;
        live = false;
        if (!edit_char) {
            original = applied = draft;
            view->break_undo_run();      // this edit is its own undo step
        }
        edit_title->set_caption(std::string(is_new ? "New " : "Edit ") +
                                (edit_char ? "character style" : "paragraph style"));
        name_box->set_value(edit_char ? cdraft.name : draft.name);
        name_box->set_editable(edit_char || is_new || draft.name != "Normal");
        name_box->set_tooltip(!edit_char && !is_new && draft.name == "Normal"
                                  ? "Normal keeps its name" : "The style's name");
        edit_error->set_visible(false);
        next_row->set_visible(!edit_char);
        page_para->set_caption(edit_char ? "Sets" : "Paragraph");
        show_type = true;
        list_page->set_visible(false);
        edit_page->set_visible(true);
        load_editor();
        show_page();
    }

    void close_editor() {
        editing = false;
        edit_page->set_visible(false);
        list_page->set_visible(true);
        if (Screen *s = self->screen())
            s->update_focus(nullptr);
        reload_list();
    }

    void show_page() {
        page_type->set_pushed(show_type);
        page_para->set_pushed(!show_type);
        type_specs->set_visible(show_type);
        para_scroll->set_visible(!show_type && !edit_char);
        sets_scroll->set_visible(!show_type && edit_char);
        relayout();
    }

    /* Fill the pages from the draft */
    void load_editor() {
        loading = true;
        type_specs->set_size_auto(edit_char);
        reload_type_page();
        if (!edit_char) {
            proxy = Paragraph();
            proxy.style = draft.para;
            proxy.runs = {Run{draft.type, "x"}};
            para_panel->load({&proxy}, doc().styles);
            next_names.assign(1, std::string());
            std::vector<std::string> items{"Same style"};
            int pick = 0;
            for (const StyleDef &d : doc().styles) {
                if (d.name == edit_orig && !edit_orig.empty())
                    continue;
                next_names.push_back(d.name);
                items.push_back(d.name);
                if (d.name == draft.next)
                    pick = (int) items.size() - 1;
            }
            next->set_items(items);
            next->set_selected_index(pick);
            next->set_caption(items[(size_t) pick]);
        } else {
            for (int b = 0; b < 16; ++b)
                field_box[b]->set_checked((cdraft.fields & (1u << b)) != 0);
            std::vector<std::string> items{"Not set"};
            color_ids.assign(1, 0);
            int pick = 0;
            for (const pagemade::Swatch &sw : doc().swatches) {
                if (sw.id == pagemade::kNoPaint)
                    continue;
                color_ids.push_back(sw.id);
                items.push_back(sw.name);
                if ((cdraft.fields & pagemade::kCharColor) && sw.id == cdraft.type.swatch)
                    pick = (int) items.size() - 1;
            }
            color->set_items(items);
            color->set_selected_index(pick);
            color->set_caption(items[(size_t) pick]);
        }
        loading = false;
        update_preview();
    }

    void update_preview() {
        edit_preview->doc = &doc();
        if (!fonts)
            return;
        const float w = std::max(60.f, (float) edit_preview->width() - 16.f);
        pagemade::Story s;
        Paragraph p;
        if (edit_char) {
            const CharStyle base = normal_type(doc());
            const CharStyle styled = char_over_normal(doc(), cdraft);
            p.runs = {Run{base, "Set in Normal, "}, Run{styled, "with this style applied"},
                      Run{base, ", and back to Normal again."}};
            if (const StyleDef *n = doc().find_style("Normal"))
                p.style = n->para;
            p.style.align = pagemade::Align::Left;
        } else {
            p.runs = {Run{draft.type, std::string(kSample) + ". Pack my box with five dozen "
                                      "liquor jugs. How vexingly quick daft zebras jump."}};
            pagemade::apply_style(p, draft, doc().char_styles);
            p.style.space_before = 0.f;
        }
        s.paragraphs.push_back(std::move(p));
        edit_preview->pv = set_preview(*fonts, s, w, 8.f, 28.f);
    }

    /* The Type page changed a field: carry it into the draft */
    void type_changed(const CharStyle &before, const CharStyle &after) {
        const uint32_t d = char_diff(before, after);
        if (!d)
            return;
        if (edit_char) {
            uint32_t set = d, unset = 0;
            if ((d & pagemade::kCharSize) && after.size <= 0.f) {
                set &= ~pagemade::kCharSize;     // Auto: the text keeps its size
                unset = pagemade::kCharSize;
            }
            CharStyleDef only{"", after, set};
            pagemade::apply_char_style(cdraft.type, only);
            cdraft.type.char_style = cdraft.name;
            cdraft.fields = (cdraft.fields | set) & ~unset;
            for (int b = 0; b < 16; ++b)
                field_box[b]->set_checked((cdraft.fields & (1u << b)) != 0);
        } else {
            CharStyle &t = draft.type;
            using namespace pagemade;
            if (d & kCharFamily) t.family = after.family;
            if (d & kCharFace) t.face = after.face;
            if (d & kCharBold) t.bold = after.bold;
            if (d & kCharItalic) t.italic = after.italic;
            if (d & kCharSize) t.size = after.size;
            if (d & kCharLeading) t.leading = after.leading;
            if (d & kCharUnderline) t.underline = after.underline;
            if (d & kCharStrike) t.strike = after.strike;
            if (d & kCharCaps) t.caps = after.caps;
            if (d & kCharTracking) t.tracking = after.tracking;
            if (d & kCharHScale) t.hscale = after.hscale;
            if (d & kCharBaseline) t.baseline_shift = after.baseline_shift;
            if (d & kCharKerning) t.kerning = after.kerning;
            if (d & kCharLigatures) t.ligatures = after.ligatures;
            if (d & kCharNoBreak) t.no_break = after.no_break;
        }
        live_apply();
        reload_type_page();
        update_preview();
        if (Screen *s = self->screen())
            s->redraw();
    }

    void para_changed() {
        if (loading)
            return;
        live_apply();
        proxy.style = draft.para;
        para_panel->load({&proxy}, doc().styles);
        update_preview();
        if (Screen *s = self->screen())
            s->redraw();
    }

    void commit_editor() {
        std::string name = name_box->text();
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
        if (name.empty()) {
            edit_error->set_caption("The style needs a name.");
            edit_error->set_visible(true);
            relayout();
            return;
        }
        const bool taken = edit_char ? doc().find_char_style(name) != nullptr
                                     : doc().find_style(name) != nullptr;
        if (taken && name != edit_orig) {
            edit_error->set_caption("There is already a style named “" + name + "”.");
            edit_error->set_visible(true);
            relayout();
            return;
        }
        /* A style removed meanwhile (an undo) comes back as new */
        const bool still = edit_char ? doc().find_char_style(edit_orig) != nullptr
                                     : doc().find_style(edit_orig) != nullptr;
        const bool as_new = edit_new || !still;
        if (edit_char) {
            cdraft.name = name;
            cdraft.type.char_style = name;
            as_new ? view->add_char_style(cdraft) : view->update_char_style(edit_orig, cdraft);
        } else {
            draft.name = name;
            draft.para.name = name;
            /* Joins the live edits' undo step: the rename and the rest */
            view->set_undo_merge(&live);
            as_new ? view->add_style(draft) : view->update_style(edit_orig, draft);
            view->set_undo_merge(nullptr);
            view->break_undo_run();
            live = false;
        }
        selected_name = name;
        close_editor();
    }

    /* Add Style: from the selection, else Normal (or italic, for a
     * character style). Add Based On: a copy of the chosen style. */
    void add_style(bool based_on) {
        const std::vector<std::string> taken = names();
        if (character) {
            CharStyleDef d;
            if (based_on) {
                const CharStyleDef *src = doc().find_char_style(selected_name);
                if (!src)
                    return;
                d = *src;
                d.name = pagemade::unique_style_name(src->name, taken);
            } else {
                ComposerView::TypeStyle t;
                d.name = pagemade::unique_style_name("New style", taken);
                if (view->type_style(t) && !t.char_style.empty() && !t.mix_char_style)
                    if (const CharStyleDef *src = doc().find_char_style(t.char_style))
                        d = *src;
                if (d.fields == 0) {
                    d.type = normal_type(doc());
                    d.type.italic = true;
                    d.fields = pagemade::kCharItalic;
                }
                d.name = pagemade::unique_style_name("New style", taken);
            }
            d.type.char_style = d.name;
            open_editor(d.name, true, nullptr, &d);
            return;
        }
        StyleDef d;
        if (based_on) {
            const StyleDef *src = doc().find_style(selected_name);
            if (!src)
                return;
            d = *src;
            d.name = pagemade::unique_style_name(src->name, taken);
        } else {
            const auto paras = view->selected_paragraphs();
            const std::string name = pagemade::unique_style_name("New style", taken);
            if (!paras.empty())
                d = pagemade::style_from(name, *paras.front());
            else if (const StyleDef *n = doc().find_style("Normal"))
                d = *n;
            d.name = name;
            d.next.clear();
        }
        d.para.name = d.name;
        open_editor(d.name, true, &d, nullptr);
    }
};

/* ---- Construction ---------------------------------------------------------- */

StylesPanel::StylesPanel(Widget *parent, ComposerView *view, pagemade::FontLibrary &fonts,
                         pagemade::FontMenuModel &fonts_menu)
    : ToolPanel(parent), m_state(std::make_unique<State>()) {
    State &st = *m_state;
    st.self = this;
    st.view = view;
    st.fonts = &fonts;

    auto row = [](Widget *parent, int spacing = 6) {
        Widget *w = new Widget(parent);
        auto *fl = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                                  AlignItems::Center, 0, spacing);
        w->set_layout(fl);
        return std::make_pair(w, fl);
    };
    auto grow = [](std::pair<Widget *, FlexLayout *> r, Widget *w) {
        r.second->set_flex_item(w, FlexLayout::FlexItem(1.f, 1.f, 0));
    };

    /* ---- The list page ---- */
    st.list_page = new Widget(this);
    {
        auto kr = row(st.list_page, 4);
        st.kind_row = kr.first;
        st.kind_para = new Button(kr.first, "Paragraph");
        st.kind_char = new Button(kr.first, "Character");
        for (Button *b : {st.kind_para, st.kind_char}) {
            b->set_flags(Button::RadioButton);
            b->set_fixed_size(Vector2i(0, kFieldH));
            grow(kr, b);
        }
        st.kind_para->set_tooltip("Paragraph styles");
        st.kind_char->set_tooltip("Character styles");
        st.kind_para->set_callback([this] { show_kind(false); });
        st.kind_char->set_callback([this] { show_kind(true); });
        st.in_use = new CheckBox(kr.first, "In use", [this](bool on) {
            m_state->in_use_only = on;
            m_state->reload_list();
        });
        st.in_use->set_tooltip("Only the styles some text is set in");
    }
    st.filter_box = new LiveTextBox(st.list_page);
    st.filter_box->set_placeholder("Filter styles");
    st.filter_box->set_fixed_size(Vector2i(0, kFieldH));
    st.filter_box->on_edit = [this](const std::string &t) {
        if (t == m_state->filter)
            return;
        m_state->filter = t;
        m_state->reload_list();
    };
    st.filter_box->set_callback([this](const std::string &t) {
        m_state->filter = t;
        m_state->reload_list();
        return true;
    });

    st.scroll = new ScrollPanel(st.list_page, ScrollPanel::ScrollTypes::Vertical);
    st.list = new StyleList(st.scroll);
    st.list->on_select = [this](int i) {
        State &s = *m_state;
        if (i >= 0 && i < (int) s.list->rows.size())
            s.selected_name = s.list->rows[(size_t) i].name;
        s.end_action();
        s.sync_buttons();
    };
    st.list->on_apply = [this](int i) {
        State &s = *m_state;
        if (i < 0 || i >= (int) s.list->rows.size())
            return;
        const std::string n = s.list->rows[(size_t) i].name;
        s.character ? s.view->apply_char_style(n) : s.view->apply_style(n);
    };
    st.list->on_edit = [this](int i) {
        State &s = *m_state;
        if (i < 0 || i >= (int) s.list->rows.size())
            return;
        const std::string n = s.list->rows[(size_t) i].name;
        s.selected_name = n;
        if (s.character) {
            if (const CharStyleDef *d = s.doc().find_char_style(n)) {
                const CharStyleDef copy = *d;
                s.open_editor(n, false, nullptr, &copy);
            }
        } else if (const StyleDef *d = s.doc().find_style(n)) {
            const StyleDef copy = *d;
            s.open_editor(n, false, &copy, nullptr);
        }
    };
    st.list->on_merge = [this](int i) {
        State &s = *m_state;
        if (i >= 0 && i < (int) s.list->rows.size()) {
            s.selected_name = s.list->rows[(size_t) i].name;
            s.begin_action(s.selected_name, true);
        }
    };
    st.list->on_remove = [this](int i) {
        State &s = *m_state;
        if (i >= 0 && i < (int) s.list->rows.size()) {
            s.selected_name = s.list->rows[(size_t) i].name;
            s.begin_action(s.selected_name, false);
        }
    };

    /* Merge into / remove: what the style's text becomes */
    st.action = new Widget(st.list_page);
    st.action->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 4));
    st.action_label = new Label(st.action, "", "sans", 14);
    {
        auto ar = row(st.action, 4);
        st.action_row = ar.first;
        st.action_target = new Dropdown(ar.first, std::vector<std::string>{"Normal"}, {},
                                        Dropdown::ComboBox, "Normal");
        st.action_target->set_fixed_size(Vector2i(0, kFieldH));
        grow(ar, st.action_target);
        st.action_cancel = new Button(ar.first, "Cancel");
        st.action_cancel->set_fixed_size(Vector2i(70, kFieldH));
        st.action_cancel->set_callback([this] { m_state->end_action(); });
        st.action_ok = new Button(ar.first, "Merge");
        st.action_ok->set_fixed_size(Vector2i(74, kFieldH));
        st.action_ok->set_callback([this] { m_state->run_action(); });
    }
    st.action->set_visible(false);

    st.summary = new Label(st.list_page, "", "sans", 13);
    {
        auto br = row(st.list_page, 4);
        st.add_row = br.first;
        st.add = new Button(br.first, "Add Style", FA_PLUS);
        st.add_based = new Button(br.first, "Based On", FA_CLONE);
        st.apply = new Button(br.first, "Apply");
        for (Button *b : {st.add, st.add_based, st.apply}) {
            b->set_fixed_size(Vector2i(0, 28));
            grow(br, b);
        }
        st.add->set_callback([this] { m_state->add_style(false); });
        st.add_based->set_callback([this] { m_state->add_style(true); });
        st.apply->set_callback([this] {
            State &s = *m_state;
            if (s.selected_name.empty())
                return;
            s.character ? s.view->apply_char_style(s.selected_name)
                        : s.view->apply_style(s.selected_name);
        });
    }

    /* ---- The edit page ---- */
    st.edit_page = new Widget(this);
    st.edit_page->set_visible(false);
    {
        auto hr = row(st.edit_page, 8);
        st.edit_head = hr.first;
        auto *back = new Button(hr.first, "Styles", FA_CHEVRON_LEFT);
        back->set_fixed_size(Vector2i(84, kFieldH));
        back->set_tooltip("Back to the list, without the changes");
        back->set_callback([this] { m_state->cancel_editor(); });
        st.edit_title = new Label(hr.first, "Edit paragraph style", "sans-bold", 15);
        grow(hr, st.edit_title);
    }
    {
        auto nr = row(st.edit_page, 6);
        st.name_row = nr.first;
        auto *l = new Label(nr.first, "Name", "sans-bold");
        l->set_fixed_size(Vector2i(74, 0));
        st.name_box = new LiveTextBox(nr.first);
        st.name_box->set_fixed_size(Vector2i(0, kFieldH));
        st.name_box->set_callback([](const std::string &) { return true; });
        grow(nr, st.name_box);
    }
    {
        auto xr = row(st.edit_page, 6);
        st.next_row = xr.first;
        auto *l = new Label(xr.first, "Next style", "sans-bold");
        l->set_fixed_size(Vector2i(74, 0));
        st.next = new Dropdown(xr.first, std::vector<std::string>{"Same style"}, {},
                               Dropdown::ComboBox, "Same style");
        st.next->set_fixed_size(Vector2i(0, kFieldH));
        st.next->set_tooltip("The style a Return at the end of this paragraph gives the next one");
        st.next->set_selected_callback([this](int i) {
            State &s = *m_state;
            if (s.loading || i < 0 || i >= (int) s.next_names.size())
                return;
            s.draft.next = s.next_names[(size_t) i];
            s.live_apply();
        });
        grow(xr, st.next);
    }
    st.edit_preview = new EditPreview(st.edit_page);
    {
        auto pr = row(st.edit_page, 4);
        st.pages_row = pr.first;
        st.page_type = new Button(pr.first, "Type");
        st.page_para = new Button(pr.first, "Paragraph");
        for (Button *b : {st.page_type, st.page_para}) {
            b->set_flags(Button::RadioButton);
            b->set_fixed_size(Vector2i(0, kFieldH));
            grow(pr, b);
        }
        st.page_type->set_callback([this] {
            m_state->show_type = true;
            m_state->show_page();
        });
        st.page_para->set_callback([this] {
            m_state->show_type = false;
            m_state->show_page();
        });
    }
    st.type_specs = new TypeSpecsPanel(
        st.edit_page, fonts, fonts_menu,
        [this](const CharStyle &before, const CharStyle &after, const void *) {
            m_state->type_changed(before, after);
        });

    st.para_scroll = new ScrollPanel(st.edit_page, ScrollPanel::ScrollTypes::Vertical);
    ParagraphPanel::Hooks hooks;
    hooks.apply = [this](const std::function<void(ParaStyle &)> &fn, const void *) {
        fn(m_state->draft.para);
        m_state->para_changed();
    };
    hooks.align = [this](pagemade::Align a) {
        m_state->draft.para.align = a;
        m_state->para_changed();
    };
    st.para_panel = new ParagraphPanel(st.para_scroll, std::move(hooks));
    st.para_panel->set_style_controls(false);

    /* A character style's fields: which it sets, and its color */
    st.sets_scroll = new ScrollPanel(st.edit_page, ScrollPanel::ScrollTypes::Vertical);
    st.sets = new Widget(st.sets_scroll);
    st.sets->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 8, 8));
    new Label(st.sets, "This style sets only the checked attributes; the rest come from "
                       "the paragraph's own type. Changing a field on the Type page checks it.",
              "sans", 13);
    {
        Widget *grid = new Widget(st.sets);
        grid->set_layout(new GridLayout(Orientation::Horizontal, 2, Alignment::Fill, 0, 6));
        for (int b = 0; b < 16; ++b) {
            st.field_box[b] = new CheckBox(grid, kFieldNames[b], [this, b](bool on) {
                State &s = *m_state;
                if (s.loading)
                    return;
                if (on)
                    s.cdraft.fields |= 1u << b;
                else
                    s.cdraft.fields &= ~(1u << b);
                if ((1u << b) == pagemade::kCharSize) {
                    if (on && !(s.cdraft.type.size > 0.f))
                        s.cdraft.type.size = 12.f;
                    s.reload_type_page();        // Auto, or the size
                }
                s.update_preview();
            });
        }
        auto cr = row(st.sets, 6);
        new Label(cr.first, "Color", "sans-bold");
        st.color = new Dropdown(cr.first, std::vector<std::string>{"Not set"}, {},
                                Dropdown::ComboBox, "Not set");
        st.color->set_fixed_size(Vector2i(0, kFieldH));
        grow(cr, st.color);
        st.color->set_selected_callback([this](int i) {
            State &s = *m_state;
            if (s.loading || i < 0 || i >= (int) s.color_ids.size())
                return;
            if (i == 0) {
                s.cdraft.fields &= ~pagemade::kCharColor;
            } else {
                s.cdraft.fields |= pagemade::kCharColor;
                s.cdraft.type.swatch = s.color_ids[(size_t) i];
                s.cdraft.type.tint = 100.f;
                if (const pagemade::Swatch *sw = s.doc().find_swatch(s.color_ids[(size_t) i]))
                    s.cdraft.type.color = sw->rgb;
            }
            s.field_box[6]->set_checked((s.cdraft.fields & pagemade::kCharColor) != 0);
            s.update_preview();
        });
    }

    st.edit_error = new Label(st.edit_page, "", "sans-bold", 14);
    st.edit_error->set_color(Color(220, 60, 50, 255));
    st.edit_error->set_visible(false);
    {
        auto er = row(st.edit_page, 6);
        st.edit_bottom = er.first;
        Widget *gap = new Widget(er.first);
        grow(er, gap);
        st.edit_cancel = new Button(er.first, "Cancel");
        st.edit_cancel->set_fixed_size(Vector2i(84, 28));
        st.edit_cancel->set_callback([this] { m_state->cancel_editor(); });
        st.edit_ok = new Button(er.first, "OK");
        st.edit_ok->set_fixed_size(Vector2i(84, 28));
        st.edit_ok->set_tooltip("Save the style: text set in it follows (unless it overrides it)");
        st.edit_ok->set_callback([this] { m_state->commit_editor(); });
    }
    st.reload_list();
}

StylesPanel::~StylesPanel() = default;

void StylesPanel::show_kind(bool character) {
    State &st = *m_state;
    if (st.editing)
        st.cancel_editor();
    if (st.character != character) {
        st.character = character;
        st.selected_name.clear();
        st.end_action();
    }
    st.reload_list();
}

void StylesPanel::refresh() {
    State &st = *m_state;
    if (!st.editing)
        st.reload_list();
}

/* The pages lay out by hand: fixed rows at the top and bottom, and the
 * list (or the editor's page) between them takes what is left. */
void StylesPanel::perform_layout(NVGcontext *ctx) {
    State &st = *m_state;
    const int W = width(), H = height();
    const int inner = std::max(40, W - 2 * kMargin);
    for (Widget *page : {st.list_page, st.edit_page}) {
        page->set_position(Vector2i(0, 0));
        page->set_size(Vector2i(W, H));
    }
    auto place = [&](Widget *w, int y, int h) {
        w->set_position(Vector2i(kMargin, y));
        w->set_size(Vector2i(inner, h));
        w->perform_layout(ctx);
    };
    auto pref_h = [&](Widget *w) {
        /* Wrapped labels take the width they will have */
        if (auto *l = dynamic_cast<Label *>(w))
            l->set_fixed_size(Vector2i(inner, 0));
        return std::max(kFieldH, w->preferred_size(ctx).y());
    };

    if (st.list_page->visible()) {
        int y = kMargin;
        for (Widget *w : {st.kind_row, (Widget *) st.filter_box}) {
            const int h = pref_h(w);
            place(w, y, h);
            y += h + kGap;
        }
        int bottom = H - kMargin;
        const int ah = pref_h(st.add_row);
        bottom -= ah;
        place(st.add_row, bottom, ah);
        bottom -= kGap;
        if (!st.summary->caption().empty()) {
            st.summary->set_fixed_size(Vector2i(inner, 0));
            const int sh = std::max(16, st.summary->preferred_size(ctx).y());
            bottom -= sh;
            place(st.summary, bottom, sh);
            bottom -= kGap;
        }
        if (st.action->visible()) {
            st.action_label->set_fixed_size(Vector2i(inner, 0));
            const int h = st.action->preferred_size(ctx).y();
            bottom -= h;
            place(st.action, bottom, h);
            bottom -= kGap;
        }
        place(st.scroll, y, std::max(kRowH, bottom - y));
    }
    if (st.edit_page->visible()) {
        int y = kMargin;
        std::vector<Widget *> top{st.edit_head, st.name_row};
        if (st.next_row->visible())
            top.push_back(st.next_row);
        for (Widget *w : top) {
            const int h = pref_h(w);
            place(w, y, h);
            y += h + kGap;
        }
        place(st.edit_preview, y, 84);
        y += 84 + kGap;
        {
            const int h = pref_h(st.pages_row);
            place(st.pages_row, y, h);
            y += h + kGap;
        }
        int bottom = H - kMargin;
        const int bh = pref_h(st.edit_bottom);
        bottom -= bh;
        place(st.edit_bottom, bottom, bh);
        bottom -= kGap;
        if (st.edit_error->visible()) {
            st.edit_error->set_fixed_size(Vector2i(inner, 0));
            const int eh = std::max(16, st.edit_error->preferred_size(ctx).y());
            bottom -= eh;
            place(st.edit_error, bottom, eh);
            bottom -= kGap;
        }
        const int body = std::max(60, bottom - y);
        /* The Type page brings its own margins and scrolling */
        st.type_specs->set_position(Vector2i(0, y));
        st.type_specs->set_size(Vector2i(W, body));
        st.type_specs->perform_layout(ctx);
        place(st.para_scroll, y, body);
        place(st.sets_scroll, y, body);
        st.update_preview();
    }
}
