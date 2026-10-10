/*
 * pagemade/paragraph_panel.cpp — see paragraph_panel.h.
 */
#include "paragraph_panel.h"

#include <nanogui/button.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/menu.h>
#include <nanogui/opengl.h>
#include <nanogui/theme.h>
#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace nanogui;
using pagemade::Align;
using pagemade::ParaStyle;
using pagemade::Paragraph;
using pagemade::StyleDef;

namespace {

const Color kSelected(56, 116, 226, 255);
const Color kRule(0, 0, 0, 38);

/* ---- Icons ---------------------------------------------------------------- */

void bar(NVGcontext *ctx, float x0, float x1, float y) {
    nvgMoveTo(ctx, x0, y);
    nvgLineTo(ctx, x1, y);
}

void arrow_head(NVGcontext *ctx, float x, float y, float dx, float dy) {
    /* A filled head pointing along (dx, dy), its tip at (x, y). */
    const float s = 2.6f;
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, x, y);
    nvgLineTo(ctx, x - dx * s - dy * s, y - dy * s - dx * s);
    nvgLineTo(ctx, x - dx * s + dy * s, y - dy * s + dx * s);
    nvgClosePath(ctx);
    nvgFill(ctx);
}

/* A capital A in strokes, its apex at the top of (x0, y0)-(x1, y1). */
void letter_a(NVGcontext *ctx, float x0, float y0, float x1, float y1) {
    const float xm = (x0 + x1) * 0.5f, yb = y0 + (y1 - y0) * 0.62f;
    const float t = (yb - y0) / (y1 - y0);
    nvgMoveTo(ctx, x0, y1);
    nvgLineTo(ctx, xm, y0);
    nvgLineTo(ctx, x1, y1);
    bar(ctx, xm + (x0 - xm) * t, xm + (x1 - xm) * t, yb);
}

} // namespace

void draw_para_icon(NVGcontext *ctx, ParaIcon icon, float x, float y, float w, float h,
                    const Color &color) {
    /* Drawn in a 20 x 16 box centered in (x, y, w, h). */
    const float ox = std::round(x + (w - 20) * 0.5f), oy = std::round(y + (h - 16) * 0.5f);
    nvgSave(ctx);
    nvgStrokeColor(ctx, color);
    nvgFillColor(ctx, color);
    nvgStrokeWidth(ctx, 1.6f);
    nvgLineCap(ctx, NVG_BUTT);

    auto stroke = [&] { nvgStroke(ctx); };
    /* Five text lines across the box; `len` (0..1) and where each sits. */
    enum Pos { L, C, R };
    auto lines5 = [&](const float *len, const Pos *pos) {
        nvgBeginPath(ctx);
        for (int i = 0; i < 5; ++i) {
            const float lw = 18.f * len[i];
            const float lx = pos[i] == L ? 1.f : pos[i] == C ? 1.f + (18.f - lw) * 0.5f : 19.f - lw;
            bar(ctx, ox + lx, ox + lx + lw, oy + 1.5f + i * 3.25f);
        }
        stroke();
    };
    /* Four lines between x0 and x1 (absolute in the box), optionally with
     * one line starting further in. */
    auto lines4 = [&](float x0, float x1, float y0, float pitch, int inset_row, float inset) {
        nvgBeginPath(ctx);
        for (int i = 0; i < 4; ++i)
            bar(ctx, ox + x0 + (i == inset_row ? inset : 0.f), ox + x1, oy + y0 + i * pitch);
        stroke();
    };

    switch (icon) {
    case ParaIcon::AlignLeft:
    case ParaIcon::AlignCenter:
    case ParaIcon::AlignRight: {
        static const float len[5] = {1.f, 0.62f, 1.f, 0.62f, 0.85f};
        const Pos p = icon == ParaIcon::AlignLeft ? L : icon == ParaIcon::AlignCenter ? C : R;
        const Pos pos[5] = {p, p, p, p, p};
        lines5(len, pos);
        break;
    }
    case ParaIcon::JustifyLeft:
    case ParaIcon::JustifyCenter:
    case ParaIcon::JustifyRight:
    case ParaIcon::JustifyAll: {
        const float last = icon == ParaIcon::JustifyAll ? 1.f : 0.5f;
        const float len[5] = {1.f, 1.f, 1.f, 1.f, last};
        const Pos p = icon == ParaIcon::JustifyCenter ? C : icon == ParaIcon::JustifyRight ? R : L;
        const Pos pos[5] = {L, L, L, L, p};
        lines5(len, pos);
        break;
    }
    case ParaIcon::LineSpacing:
        lines4(8, 19, 2, 4, -1, 0);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, ox + 3, oy + 2.5f);
        nvgLineTo(ctx, ox + 3, oy + 13.5f);
        stroke();
        arrow_head(ctx, ox + 3, oy + 0.5f, 0, -1);
        arrow_head(ctx, ox + 3, oy + 15.5f, 0, 1);
        break;
    case ParaIcon::ExtraSpacing:
        lines4(1, 13, 2, 4, -1, 0);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, ox + 17, oy + 5);
        nvgLineTo(ctx, ox + 17, oy + 11);
        stroke();
        arrow_head(ctx, ox + 17, oy + 3, 0, -1);
        arrow_head(ctx, ox + 17, oy + 13, 0, 1);
        break;
    case ParaIcon::LeftIndent:
        lines4(9, 19, 2, 4, -1, 0);
        nvgBeginPath(ctx);
        bar(ctx, ox + 0.5f, ox + 4, oy + 8);
        nvgMoveTo(ctx, ox + 7, oy + 1);
        nvgLineTo(ctx, ox + 7, oy + 15);
        stroke();
        arrow_head(ctx, ox + 6, oy + 8, 1, 0);
        break;
    case ParaIcon::RightIndent:
        lines4(1, 11, 2, 4, -1, 0);
        nvgBeginPath(ctx);
        bar(ctx, ox + 16, ox + 19.5f, oy + 8);
        nvgMoveTo(ctx, ox + 13, oy + 1);
        nvgLineTo(ctx, ox + 13, oy + 15);
        stroke();
        arrow_head(ctx, ox + 14, oy + 8, -1, 0);
        break;
    case ParaIcon::FirstIndent:
    case ParaIcon::LastIndent: {
        const bool first = icon == ParaIcon::FirstIndent;
        lines4(6, 19, 2, 4, first ? 0 : 3, 5);
        /* A chevron beside the indented line. */
        const float cy = oy + (first ? 2.f : 14.f);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, ox + 1, cy - 2.5f);
        nvgLineTo(ctx, ox + 4, cy);
        nvgLineTo(ctx, ox + 1, cy + 2.5f);
        nvgLineJoin(ctx, NVG_MITER);
        stroke();
        break;
    }
    case ParaIcon::DropCap: {
        /* A large A beside three short lines, a full line below. */
        nvgLineJoin(ctx, NVG_MITER);
        nvgBeginPath(ctx);
        letter_a(ctx, ox + 1, oy + 1, ox + 10, oy + 11);
        bar(ctx, ox + 13, ox + 19, oy + 2);
        bar(ctx, ox + 13, ox + 19, oy + 6);
        bar(ctx, ox + 13, ox + 19, oy + 10.5f);
        bar(ctx, ox + 1, ox + 19, oy + 14.5f);
        stroke();
        break;
    }
    case ParaIcon::DropSize:
        /* A large A and a small one. */
        nvgLineJoin(ctx, NVG_MITER);
        nvgBeginPath(ctx);
        letter_a(ctx, ox + 1, oy + 1, ox + 12, oy + 15);
        letter_a(ctx, ox + 13.5f, oy + 8, ox + 19, oy + 15);
        stroke();
        break;
    case ParaIcon::DropChars:
        /* Two capitals over a bracket: how many characters drop. */
        nvgLineJoin(ctx, NVG_MITER);
        nvgBeginPath(ctx);
        letter_a(ctx, ox + 1, oy + 1, ox + 9, oy + 11);
        letter_a(ctx, ox + 11, oy + 1, ox + 19, oy + 11);
        nvgMoveTo(ctx, ox + 1, oy + 12.5f);
        nvgLineTo(ctx, ox + 1, oy + 15);
        nvgLineTo(ctx, ox + 19, oy + 15);
        nvgLineTo(ctx, ox + 19, oy + 12.5f);
        stroke();
        break;
    case ParaIcon::Orphans:
    case ParaIcon::Widows: {
        /* Two lines against a block's bottom edge (orphans) or top edge
         * (widows), drawn as a bracket. */
        const bool bottom = icon == ParaIcon::Orphans;
        nvgBeginPath(ctx);
        const float e = bottom ? oy + 15.f : oy + 1.f, in = bottom ? oy + 12.f : oy + 4.f;
        nvgMoveTo(ctx, ox + 1, in);
        nvgLineTo(ctx, ox + 1, e);
        nvgLineTo(ctx, ox + 19, e);
        nvgLineTo(ctx, ox + 19, in);
        const float y0 = bottom ? oy + 7.5f : oy + 5.f;
        bar(ctx, ox + 4, ox + 16, y0);
        bar(ctx, ox + 4, ox + 12, y0 + 3.5f);
        nvgLineJoin(ctx, NVG_MITER);
        stroke();
        break;
    }
    case ParaIcon::List:
        /* Three items, each a dot and a line. */
        for (int i = 0; i < 3; ++i) {
            nvgBeginPath(ctx);
            nvgCircle(ctx, ox + 2.5f, oy + 2.5f + i * 5.5f, 1.7f);
            nvgFill(ctx);
        }
        nvgBeginPath(ctx);
        for (int i = 0; i < 3; ++i)
            bar(ctx, ox + 7, ox + 19, oy + 2.5f + i * 5.5f);
        stroke();
        break;
    case ParaIcon::ListStart:
        /* A "1" and the line it starts. */
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, ox + 1.5f, oy + 3.5f);
        nvgLineTo(ctx, ox + 4, oy + 1.5f);
        nvgLineTo(ctx, ox + 4, oy + 14.5f);
        bar(ctx, ox + 1.5f, ox + 6.5f, oy + 14.5f);
        nvgLineJoin(ctx, NVG_MITER);
        stroke();
        lines4(9, 19, 2, 4, -1, 0);
        break;
    case ParaIcon::KeepTogether:
        /* Lines held by a bracket. */
        lines4(7, 19, 2, 4, -1, 0);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, ox + 4.5f, oy + 0.5f);
        nvgLineTo(ctx, ox + 1.5f, oy + 0.5f);
        nvgLineTo(ctx, ox + 1.5f, oy + 15.5f);
        nvgLineTo(ctx, ox + 4.5f, oy + 15.5f);
        nvgLineJoin(ctx, NVG_MITER);
        stroke();
        break;
    case ParaIcon::KeepWithNext:
        /* A heading, an arrow, and the text it stays with. */
        nvgBeginPath(ctx);
        bar(ctx, ox + 1, ox + 12, oy + 1.5f);
        nvgMoveTo(ctx, ox + 6, oy + 4);
        nvgLineTo(ctx, ox + 6, oy + 7);
        bar(ctx, ox + 1, ox + 19, oy + 11);
        bar(ctx, ox + 1, ox + 19, oy + 14.5f);
        stroke();
        arrow_head(ctx, ox + 6, oy + 9, 0, 1);
        break;
    case ParaIcon::StartNewFrame:
        /* Text starting in the corner of a new block. */
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, ox + 1, oy + 15.5f);
        nvgLineTo(ctx, ox + 1, oy + 0.5f);
        nvgLineTo(ctx, ox + 19, oy + 0.5f);
        nvgLineJoin(ctx, NVG_MITER);
        stroke();
        lines4(5, 19, 4.5f, 3.5f, -1, 0);
        break;
    case ParaIcon::SpaceBefore:
    case ParaIcon::SpaceAfter: {
        const bool before = icon == ParaIcon::SpaceBefore;
        nvgBeginPath(ctx);
        for (int i = 0; i < 3; ++i)
            bar(ctx, ox + 2, ox + 18, oy + (before ? 8.f : 1.5f) + i * 3.25f);
        stroke();
        /* The gap, as a bracket on the open side. */
        nvgBeginPath(ctx);
        const float y0 = before ? oy + 0.5f : oy + 15.5f, y1 = before ? oy + 4.5f : oy + 11.5f;
        nvgMoveTo(ctx, ox + 10, y0);
        nvgLineTo(ctx, ox + 10, y1);
        bar(ctx, ox + 6, ox + 14, y0);
        stroke();
        break;
    }
    }
    nvgRestore(ctx);
}

/* ---- Small widgets ---------------------------------------------------------- */

namespace {

/* An icon beside a field. */
class IconLabel : public Widget {
public:
    IconLabel(Widget *parent, ParaIcon icon) : Widget(parent), m_icon(icon) {
        set_fixed_size(Vector2i(24, 24));
    }
    void draw(NVGcontext *ctx) override {
        draw_para_icon(ctx, m_icon, (float) m_pos.x(), (float) m_pos.y(), (float) m_size.x(),
                       (float) m_size.y(),
                       m_enabled ? m_theme->m_text_color : m_theme->m_disabled_text_color);
    }
private:
    ParaIcon m_icon;
};

/* A hairline across the panel between groups. */
class Rule : public Widget {
public:
    explicit Rule(Widget *parent) : Widget(parent) {}
    Vector2i preferred_size(NVGcontext *) const override { return Vector2i(0, 9); }
    void draw(NVGcontext *ctx) override {
        nvgBeginPath(ctx);
        const float y = std::round(m_pos.y() + m_size.y() * 0.5f) + 0.5f;
        bar(ctx, (float) m_pos.x(), (float) (m_pos.x() + m_size.x()), y);
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, kRule);
        nvgStroke(ctx);
    }
};

/* Children in a row against the right edge, at their preferred sizes. */
class RightRow : public Widget {
public:
    RightRow(Widget *parent, int spacing) : Widget(parent), m_spacing(spacing) {}
    Vector2i preferred_size(NVGcontext *ctx) const override {
        int w = 0, h = 0;
        for (Widget *c : m_children) {
            const Vector2i ps = size_of(c, ctx);
            w += ps.x() + (w ? m_spacing : 0);
            h = std::max(h, ps.y());
        }
        return Vector2i(w, h);
    }
    void perform_layout(NVGcontext *ctx) override {
        int x = m_size.x();
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
            const Vector2i ps = size_of(*it, ctx);
            x -= ps.x();
            (*it)->set_position(Vector2i(x, (m_size.y() - ps.y()) / 2));
            (*it)->set_size(ps);
            (*it)->perform_layout(ctx);
            x -= m_spacing;
        }
    }
private:
    static Vector2i size_of(Widget *c, NVGcontext *ctx) {
        const Vector2i fs = c->fixed_size();
        const Vector2i ps = c->preferred_size(ctx);
        return Vector2i(fs.x() ? fs.x() : ps.x(), fs.y() ? fs.y() : ps.y());
    }
    int m_spacing;
};

} // namespace

/* An icon button, lit while its setting holds for the paragraphs: one of
 * the seven alignments, or a keep option (`flag`). */
class ParagraphPanel::IconButton : public Button {
public:
    IconButton(Widget *parent, ParaIcon icon, const char *tip, Align align = Align::Left,
               bool ParaStyle::*flag = nullptr)
        : Button(parent, ""), m_icon(icon), m_align(align), m_flag(flag) {
        set_fixed_size(Vector2i(30, 28));
        set_tooltip(tip);
    }
    Align align() const { return m_align; }
    bool ParaStyle::*flag() const { return m_flag; }
    bool on() const { return m_on; }
    void set_on(bool on) { m_on = on; }

    void draw(NVGcontext *ctx) override {
        const float x = (float) m_pos.x(), y = (float) m_pos.y();
        const float w = (float) m_size.x(), h = (float) m_size.y();
        Color fill(0, 0, 0, 0);
        if (m_on && m_enabled)
            fill = kSelected;
        else if (m_enabled && (m_pushed || m_mouse_focus))
            fill = Color(0, 0, 0, m_pushed ? 40 : 18);
        if (fill.a() > 0) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 1, y + 1, w - 2, h - 2, 5);
            nvgFillColor(ctx, fill);
            nvgFill(ctx);
        }
        const Color ink = !m_enabled ? m_theme->m_disabled_text_color
                        : m_on       ? Color(255, 255, 255, 255)
                                     : m_theme->m_text_color;
        draw_para_icon(ctx, m_icon, x, y, w, h, ink);
    }

private:
    ParaIcon m_icon;
    Align m_align;
    bool ParaStyle::*m_flag;
    bool m_on = false;
};

/* ---- The panel ---------------------------------------------------------------- */

namespace {

/* The List menu: none, three bullets, five number formats. */
struct ListChoice {
    const char *caption;
    pagemade::ListKind kind;
    const char *bullet;
    pagemade::NumberFormat format;
};
const ListChoice kLists[] = {
    {"None", pagemade::ListKind::None, "", pagemade::NumberFormat::Arabic},
    {"\u2022  Bullet", pagemade::ListKind::Bullet, "\u2022", pagemade::NumberFormat::Arabic},
    {"\u2013  Dash", pagemade::ListKind::Bullet, "\u2013", pagemade::NumberFormat::Arabic},
    {"\u203A  Chevron", pagemade::ListKind::Bullet, "\u203A", pagemade::NumberFormat::Arabic},
    {"1.  Numbers", pagemade::ListKind::Number, "", pagemade::NumberFormat::Arabic},
    {"a.  Letters", pagemade::ListKind::Number, "", pagemade::NumberFormat::LowerAlpha},
    {"A.  Capitals", pagemade::ListKind::Number, "", pagemade::NumberFormat::UpperAlpha},
    {"i.  Roman", pagemade::ListKind::Number, "", pagemade::NumberFormat::LowerRoman},
    {"I.  Roman caps", pagemade::ListKind::Number, "", pagemade::NumberFormat::UpperRoman},
};

int list_index(const ParaStyle &ps) {
    for (int i = 0; i < (int) (sizeof kLists / sizeof kLists[0]); ++i) {
        const ListChoice &c = kLists[i];
        if (ps.list != c.kind)
            continue;
        if (c.kind == pagemade::ListKind::None ||
            (c.kind == pagemade::ListKind::Bullet && ps.bullet == c.bullet) ||
            (c.kind == pagemade::ListKind::Number && ps.number_format == c.format))
            return i;
    }
    return -1;
}

} // namespace

ParagraphPanel::ParagraphPanel(Widget *parent, Hooks hooks)
    : Widget(parent), m_hooks(std::move(hooks)) {
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 14, 8));

    /* Paragraph [style] */
    Widget *head = new Widget(this);
    head->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 10));
    new Label(head, "Paragraph", "sans-bold");
    m_style = new Dropdown(head, std::vector<std::string>{"Normal"}, {}, Dropdown::ComboBox,
                           "Normal");
    m_style->set_fixed_size(Vector2i(200, 28));
    m_style->set_max_size(Vector2i(200, 28));
    m_style->set_tooltip("Paragraph style. “Name+”: the paragraph differs from its style.");
    m_style->set_selected_callback([this](int i) {
        if (m_loading || i < 0 || i >= (int) m_style_names.size())
            return;
        if (m_hooks.apply_style)
            m_hooks.apply_style(m_style_names[(size_t) i]);
    });

    /* The alignments: three ragged, then four justified. */
    RightRow *aligns = new RightRow(this, 2);
    struct A { ParaIcon icon; Align align; const char *tip; };
    const A all[] = {
        {ParaIcon::AlignLeft, Align::Left, "Align left"},
        {ParaIcon::AlignCenter, Align::Center, "Align center"},
        {ParaIcon::AlignRight, Align::Right, "Align right"},
        {ParaIcon::JustifyLeft, Align::Justify, "Justify, last line left"},
        {ParaIcon::JustifyCenter, Align::JustifyCenter, "Justify, last line centered"},
        {ParaIcon::JustifyRight, Align::JustifyRight, "Justify, last line right"},
        {ParaIcon::JustifyAll, Align::ForceJustify, "Justify all lines (force justify)"},
    };
    for (size_t i = 0; i < 7; ++i) {
        if (i == 3) {
            Widget *gap = new Widget(aligns);
            gap->set_fixed_size(Vector2i(10, 1));
        }
        auto *b = new IconButton(aligns, all[i].icon, all[i].tip, all[i].align);
        b->set_callback([this, b] {
            if (m_hooks.align)
                m_hooks.align(b->align());
        });
        m_align.push_back(b);
    }

    auto grid = [this] {
        Widget *g = new Widget(this);
        auto *gl = new GridLayout(Orientation::Horizontal, 4, Alignment::Middle, 0, 6);
        gl->set_col_alignment(
            {Alignment::Middle, Alignment::Fill, Alignment::Middle, Alignment::Fill});
        g->set_layout(gl);
        return g;
    };
    using P = ParaStyle;

    new Rule(this);
    Widget *spacing = grid();
    add_field(spacing, ParaIcon::LineSpacing,
              "Leading: line spacing, a percent of each line's height", &P::line_spacing, 100.f,
              pagemade::kMinLineSpacing * 100.f, pagemade::kMaxLineSpacing * 100.f, 5.f, "%.0f",
              "%");
    add_field(spacing, ParaIcon::ExtraSpacing, "Extra spacing, added to every line",
              &P::extra_spacing, 1.f, -72.f, 72.f, 0.5f, "%.1f", "pt");

    new Rule(this);
    Widget *indents = grid();
    add_field(indents, ParaIcon::LeftIndent, "Left indent", &P::left_indent, 1.f, 0.f, 1000.f,
              1.f, "%.1f", "pt");
    add_field(indents, ParaIcon::RightIndent, "Right indent", &P::right_indent, 1.f, 0.f,
              1000.f, 1.f, "%.1f", "pt");
    add_field(indents, ParaIcon::FirstIndent,
              "First line indent, from the left indent (negative hangs)", &P::first_indent, 1.f,
              -1000.f, 1000.f, 1.f, "%.1f", "pt");
    add_field(indents, ParaIcon::LastIndent, "Last line indent, from the left indent",
              &P::last_indent, 1.f, -1000.f, 1000.f, 1.f, "%.1f", "pt");

    new Rule(this);
    Widget *space = grid();
    add_field(space, ParaIcon::SpaceBefore, "Space before the paragraph", &P::space_before, 1.f,
              0.f, 1000.f, 1.f, "%.1f", "pt");
    add_field(space, ParaIcon::SpaceAfter, "Space after the paragraph", &P::space_after, 1.f,
              0.f, 1000.f, 1.f, "%.1f", "pt");

    /* Bullets and numbering. Starting a list on a paragraph without
     * indents gives it the usual hanging indent. */
    new Rule(this);
    Widget *list = grid();
    auto *list_icon = new IconLabel(list, ParaIcon::List);
    list_icon->set_tooltip("Bullets and numbering");
    std::vector<std::string> list_items;
    for (const ListChoice &c : kLists)
        list_items.push_back(c.caption);
    m_list = new Dropdown(list, list_items, {}, Dropdown::ComboBox, "None");
    m_list->set_fixed_size(Vector2i(104, 28));
    m_list->set_max_size(Vector2i(104, 28));
    m_list->set_tooltip("Bullets and numbering");
    m_list->set_selected_callback([this](int i) {
        if (m_loading || i < 0 || i >= (int) (sizeof kLists / sizeof kLists[0]))
            return;
        const ListChoice c = kLists[i];
        if (m_hooks.apply)
            m_hooks.apply([c](ParaStyle &ps) {
                if (c.kind != pagemade::ListKind::None && ps.list == pagemade::ListKind::None &&
                    ps.left_indent == 0.f && ps.first_indent == 0.f) {
                    ps.left_indent = 18.f;
                    ps.first_indent = -18.f;
                }
                ps.list = c.kind;
                if (c.kind == pagemade::ListKind::Bullet)
                    ps.bullet = c.bullet;
                if (c.kind == pagemade::ListKind::Number)
                    ps.number_format = c.format;
            }, nullptr);
    });
    add_int_field(list, ParaIcon::ListStart,
                  "Start numbering at (0 = go on from the paragraph before)", &P::list_start, 0,
                  9999, "");

    new Rule(this);
    Widget *drop = grid();
    add_int_field(drop, ParaIcon::DropCap,
                  "Drop cap: how many lines it drops (0 = none)", &P::drop_lines, 0,
                  pagemade::kMaxDropLines, "lines");
    add_field(drop, ParaIcon::DropSize,
              "Drop cap size: 100% spans its lines exactly; larger moves more lines over",
              &P::drop_scale, 1.f, pagemade::kMinDropScale, pagemade::kMaxDropScale, 5.f, "%.0f",
              "%");
    add_int_field(drop, ParaIcon::DropChars, "Drop cap: how many characters are enlarged",
                  &P::drop_chars, 1, pagemade::kMaxDropChars, "chars");

    /* Keep options: the fewest lines left at the bottom of a text block
     * and carried to the top of the next, then the three switches. */
    new Rule(this);
    Widget *keep = grid();
    add_int_field(keep, ParaIcon::Orphans,
                  "Orphans: the fewest lines left at the bottom of a text block (0 = off)",
                  &P::orphans, 0, pagemade::kMaxKeepLines, "lines");
    add_int_field(keep, ParaIcon::Widows,
                  "Widows: the fewest lines carried to the top of the next block (0 = off)",
                  &P::widows, 0, pagemade::kMaxKeepLines, "lines");
    RightRow *switches = new RightRow(this, 2);
    struct F { ParaIcon icon; bool ParaStyle::*flag; const char *tip; };
    const F flags[] = {
        {ParaIcon::KeepTogether, &P::keep_together, "Keep lines together in one text block"},
        {ParaIcon::KeepWithNext, &P::keep_with_next,
         "Keep with next: end in the text block where the next paragraph starts"},
        {ParaIcon::StartNewFrame, &P::start_new_frame,
         "Start in the next text block (Ctrl+Return inside text breaks there too)"},
    };
    for (const F &f : flags) {
        auto *b = new IconButton(switches, f.icon, f.tip, Align::Left, f.flag);
        b->set_callback([this, b] {
            const auto flag = b->flag();
            const bool value = !b->on();
            if (m_hooks.apply)
                m_hooks.apply([flag, value](ParaStyle &ps) { ps.*flag = value; }, nullptr);
        });
        m_flags.push_back(b);
    }

    new Rule(this);
    RightRow *bottom = new RightRow(this, 6);
    m_delete = new Button(bottom, "Remove");
    m_delete->set_fixed_size(Vector2i(84, 28));
    m_delete->set_tooltip("Delete this style (its paragraphs keep their formatting)");
    m_delete->set_callback([this] {
        if (m_hooks.delete_style && !m_current_style.empty())
            m_hooks.delete_style(m_current_style);
    });
    m_new = new Button(bottom, "Add");
    m_new->set_fixed_size(Vector2i(84, 28));
    m_new->set_tooltip("New style from this paragraph (an existing name redefines that style)");
    m_new->set_callback([this] {
        if (m_hooks.new_style)
            m_hooks.new_style();
    });
}

void ParagraphPanel::draw(NVGcontext *ctx) {
    Color bg = m_theme->m_window_popup;
    bg.a() = 1.f;
    const float cr = std::max(10.f, (float) m_theme->m_window_corner_radius - 2.f);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 1.f, m_pos.y() + 1.f, m_size.x() - 2.f, m_size.y() - 2.f,
                   cr - 1.f);
    nvgFillColor(ctx, bg);
    nvgFill(ctx);
    Widget::draw(ctx);
}

void ParagraphPanel::add_field(Widget *grid, ParaIcon icon, const char *tip,
                               float ParaStyle::*member, float scale, float lo, float hi,
                               float step, const char *format, const char *units) {
    auto *label = new IconLabel(grid, icon);
    label->set_tooltip(tip);
    auto *box = new FloatSpin(grid, 0.f, step, lo, hi);
    box->number_format(format);
    box->set_units(units);
    box->set_default_value("");          // left blank (mixed): no change
    box->set_alignment(TextBox::Alignment::Left);
    box->set_fixed_size(Vector2i(104, 28));
    box->set_tooltip(tip);
    const size_t index = m_fields.size();
    m_fields.push_back({box, member, scale, lo, hi});
    box->set_blank_value([this, index] {
        const Field &f = m_fields[index];
        return m_first.*(f.member) * f.scale;
    });
    box->TextBox::set_callback([this, index](const std::string &s) {
        const Field &f = m_fields[index];
        char *end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (end == s.c_str())
            return false;
        const float shown = std::clamp(v, f.lo, f.hi);
        f.box->set_value(shown);
        const float value = shown / f.scale;
        const auto member = f.member;
        if (m_hooks.apply)
            m_hooks.apply([member, value](ParaStyle &ps) { ps.*member = value; },
                          f.box->spinning() ? f.box : nullptr);
        return true;
    });
}

void ParagraphPanel::add_int_field(Widget *grid, ParaIcon icon, const char *tip,
                                   int ParaStyle::*member, int lo, int hi, const char *units) {
    auto *label = new IconLabel(grid, icon);
    label->set_tooltip(tip);
    auto *box = new IntSpin(grid, lo, 1, lo, hi);
    box->set_units(units);
    box->set_default_value("");          // left blank (mixed): no change
    box->set_alignment(TextBox::Alignment::Left);
    box->set_fixed_size(Vector2i(104, 28));
    box->set_tooltip(tip);
    const size_t index = m_int_fields.size();
    m_int_fields.push_back({box, member, lo, hi});
    box->set_blank_value([this, index] { return m_first.*(m_int_fields[index].member); });
    box->TextBox::set_callback([this, index](const std::string &s) {
        const IntField &f = m_int_fields[index];
        char *end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (end == s.c_str())
            return false;
        const int value = (int) std::clamp<long>(v, f.lo, f.hi);
        f.box->set_value(value);
        const auto member = f.member;
        if (m_hooks.apply)
            m_hooks.apply([member, value](ParaStyle &ps) { ps.*member = value; },
                          f.box->spinning() ? f.box : nullptr);
        return true;
    });
}

void ParagraphPanel::load(const std::vector<const Paragraph *> &paras,
                          const std::vector<StyleDef> &styles) {
    m_loading = true;
    const bool any = !paras.empty();
    if (any)
        m_first = paras.front()->style;

    /* The style menu. */
    std::vector<std::string> names;
    for (const StyleDef &s : styles)
        names.push_back(s.name);
    if (names != m_style_names) {
        m_style_names = names;
        m_style->set_items(names);
    }
    m_current_style.clear();
    bool same_name = any;
    for (const Paragraph *p : paras)
        same_name &= p->style.name == paras.front()->style.name;
    std::string caption = "—";      // mixed styles, or nothing selected
    int index = -1;
    if (same_name) {
        const std::string &name = paras.front()->style.name;
        auto it = std::find(m_style_names.begin(), m_style_names.end(), name);
        if (it != m_style_names.end()) {
            m_current_style = name;
            index = (int) (it - m_style_names.begin());
            bool overridden = false;
            for (const StyleDef &s : styles)
                if (s.name == name)
                    for (const Paragraph *p : paras)
                        overridden |= pagemade::style_overridden(*p, s);
            caption = overridden ? name + "+" : name;
        } else {
            caption = "No style";
        }
    }
    m_style->set_selected_index(index);
    m_style->set_caption(caption);
    m_style->set_enabled(any);

    /* Alignment: lit only when every paragraph agrees. */
    bool same_align = any;
    for (const Paragraph *p : paras)
        same_align &= p->style.align == paras.front()->style.align;
    for (IconButton *b : m_align) {
        b->set_enabled(any);
        b->set_on(same_align && b->align() == paras.front()->style.align);
    }
    /* A keep option is lit when every paragraph has it. */
    for (IconButton *b : m_flags) {
        bool all = any;
        for (const Paragraph *p : paras)
            all &= p->style.*(b->flag());
        b->set_enabled(any);
        b->set_on(all);
    }

    for (Field &f : m_fields) {
        f.box->set_enabled(any);
        if (f.box->focused() || f.box->spinning())
            continue;                    // it shows what is being set
        bool same = any;
        for (const Paragraph *p : paras)
            same &= p->style.*(f.member) == paras.front()->style.*(f.member);
        if (same)
            f.box->set_value(paras.front()->style.*(f.member) * f.scale);
        else
            f.box->TextBox::set_value("");
    }

    {
        bool same = any;
        for (const Paragraph *p : paras)
            same &= list_index(p->style) == list_index(paras.front()->style);
        const int li = any ? list_index(paras.front()->style) : -1;
        m_list->set_selected_index(same ? li : -1);
        if (!same)
            m_list->set_caption("\u2014");
        else if (li < 0)
            m_list->set_caption(paras.front()->style.list == pagemade::ListKind::Bullet
                                ? paras.front()->style.bullet : std::string("Custom"));
        m_list->set_enabled(any);
    }

    for (IntField &f : m_int_fields) {
        f.box->set_enabled(any);
        if (f.box->focused() || f.box->spinning())
            continue;
        bool same = any;
        for (const Paragraph *p : paras)
            same &= p->style.*(f.member) == paras.front()->style.*(f.member);
        if (same)
            f.box->set_value(paras.front()->style.*(f.member));
        else
            f.box->TextBox::set_value("");
    }

    m_delete->set_enabled(any && !m_current_style.empty());
    m_new->set_enabled(any);
    m_loading = false;
}

/* ---- The Character panel ------------------------------------------------------ */

/* A square of the text's color. */
class CharacterPanel::Chip : public Widget {
public:
    explicit Chip(Widget *parent) : Widget(parent) { set_fixed_size(Vector2i(22, 22)); }
    void set_color(const pagemade::Color &c, bool known) {
        m_color = c;
        m_known = known;
    }
    void draw(NVGcontext *ctx) override {
        const float x = m_pos.x() + 2.f, y = m_pos.y() + 2.f;
        const float w = m_size.x() - 4.f, h = m_size.y() - 4.f;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x, y, w, h, 4);
        if (m_known && m_enabled) {
            nvgFillColor(ctx, nvgRGBAf(m_color.r, m_color.g, m_color.b, m_color.a));
            nvgFill(ctx);
        }
        nvgStrokeWidth(ctx, 1.f);
        nvgStrokeColor(ctx, Color(0, 0, 0, 90));
        nvgStroke(ctx);
    }
private:
    pagemade::Color m_color;
    bool m_known = false;
};

CharacterPanel::CharacterPanel(Widget *parent, Hooks hooks)
    : Widget(parent), m_hooks(std::move(hooks)) {
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 14, 8));

    Widget *head = new Widget(this);
    head->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 10));
    new Label(head, "Character", "sans-bold");
    m_style = new Dropdown(head, std::vector<std::string>{"None"}, {}, Dropdown::ComboBox, "None");
    m_style->set_fixed_size(Vector2i(200, 28));
    m_style->set_max_size(Vector2i(200, 28));
    m_style->set_tooltip("Character style. “Name+”: the text differs from its style.");
    m_style->set_selected_callback([this](int i) {
        if (m_loading || i < 0 || i > (int) m_style_names.size())
            return;
        if (m_hooks.apply_style)
            m_hooks.apply_style(i == 0 ? std::string() : m_style_names[(size_t) i - 1]);
    });

    new Rule(this);
    Widget *color = new Widget(this);
    color->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 6));
    new Label(color, "Color", "sans-bold");
    m_chip = new Chip(color);
    m_color = new Dropdown(color, std::vector<std::string>{"[Black]"}, {}, Dropdown::ComboBox,
                           "[Black]");
    m_color->set_fixed_size(Vector2i(118, 28));
    m_color->set_max_size(Vector2i(118, 28));
    m_color->set_tooltip("Text color: a swatch of the publication");
    m_color->set_selected_callback([this](int i) {
        if (m_loading || i < 0 || i >= (int) m_swatch_ids.size())
            return;
        if (m_hooks.swatch)
            m_hooks.swatch(m_swatch_ids[(size_t) i]);
    });
    new Label(color, "Tint", "sans-bold");
    m_tint = new FloatSpin(color, 100.f, 5.f, 0.f, 100.f);
    m_tint->number_format("%.0f");
    m_tint->set_units("%");
    m_tint->set_default_value("");
    m_tint->set_alignment(TextBox::Alignment::Left);
    m_tint->set_fixed_size(Vector2i(84, 28));
    m_tint->set_tooltip("Tint: a screen of the swatch over paper");
    m_tint->set_blank_value([this] { return m_first_tint; });
    m_tint->TextBox::set_callback([this](const std::string &s) {
        char *end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (end == s.c_str())
            return false;
        const float t = std::clamp(v, 0.f, 100.f);
        m_tint->set_value(t);
        if (m_hooks.tint)
            m_hooks.tint(t, m_tint->spinning() ? m_tint : nullptr);
        return true;
    });

    new Rule(this);
    RightRow *bottom = new RightRow(this, 6);
    m_delete = new Button(bottom, "Remove");
    m_delete->set_fixed_size(Vector2i(84, 28));
    m_delete->set_tooltip("Delete this character style (its text keeps its formatting)");
    m_delete->set_callback([this] {
        if (m_hooks.delete_style && !m_current.empty())
            m_hooks.delete_style(m_current);
    });
    m_new = new Button(bottom, "Add");
    m_new->set_fixed_size(Vector2i(84, 28));
    m_new->set_tooltip("New character style from the selected text (an existing name "
                       "redefines that style)");
    m_new->set_callback([this] {
        if (m_hooks.new_style)
            m_hooks.new_style();
    });
}

void CharacterPanel::draw(NVGcontext *ctx) {
    Color bg = m_theme->m_window_popup;
    bg.a() = 1.f;
    const float cr = std::max(10.f, (float) m_theme->m_window_corner_radius - 2.f);
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, m_pos.x() + 1.f, m_pos.y() + 1.f, m_size.x() - 2.f, m_size.y() - 2.f,
                   cr - 1.f);
    nvgFillColor(ctx, bg);
    nvgFill(ctx);
    Widget::draw(ctx);
}

void CharacterPanel::load(const State &st, const std::vector<pagemade::CharStyleDef> &styles,
                          const std::vector<pagemade::Swatch> &swatches) {
    m_loading = true;
    const bool any = st.enabled;

    std::vector<std::string> names;
    for (const pagemade::CharStyleDef &d : styles)
        names.push_back(d.name);
    if (names != m_style_names) {
        m_style_names = names;
        std::vector<std::string> items{"None"};
        items.insert(items.end(), names.begin(), names.end());
        m_style->set_items(items);
    }
    m_current.clear();
    if (st.mixed_style) {
        m_style->set_selected_index(-1);
        m_style->set_caption("\u2014");
    } else if (st.style.empty()) {
        m_style->set_selected_index(0);
    } else {
        auto it = std::find(m_style_names.begin(), m_style_names.end(), st.style);
        if (it != m_style_names.end()) {
            m_current = st.style;
            m_style->set_selected_index((int) (it - m_style_names.begin()) + 1);
        } else {
            m_style->set_selected_index(-1);
        }
        m_style->set_caption(st.overridden ? st.style + "+" : st.style);
    }
    m_style->set_enabled(any);

    /* The swatch menu. Text from before swatches shows as RGB. */
    std::vector<std::string> cnames;
    m_swatch_ids.clear();
    for (const pagemade::Swatch &sw : swatches) {
        cnames.push_back(sw.name);
        m_swatch_ids.push_back(sw.id);
    }
    if (cnames != m_color_names) {
        m_color_names = cnames;
        m_color->set_items(cnames);
    }
    int ci = -1;
    for (size_t i = 0; i < m_swatch_ids.size(); ++i)
        if (m_swatch_ids[i] == st.swatch)
            ci = (int) i;
    m_color->set_selected_index(st.mixed_color ? -1 : ci);
    if (st.mixed_color)
        m_color->set_caption("\u2014");
    else if (ci < 0)
        m_color->set_caption(st.shown.r == 0 && st.shown.g == 0 && st.shown.b == 0 ? "[Black]"
                                                                                   : "RGB");
    m_color->set_enabled(any);
    m_chip->set_color(st.shown, !st.mixed_color);
    m_chip->set_enabled(any);

    /* A tint screens a swatch; RGB text has none. */
    m_first_tint = st.tint;
    m_tint->set_enabled(any && !st.mixed_color && st.swatch != 0);
    if (!m_tint->focused() && !m_tint->spinning()) {
        if (st.mixed_tint || st.swatch == 0)
            m_tint->TextBox::set_value("");
        else
            m_tint->set_value(st.tint);
    }

    m_delete->set_enabled(any && !m_current.empty());
    m_new->set_enabled(any);
    m_loading = false;
}
