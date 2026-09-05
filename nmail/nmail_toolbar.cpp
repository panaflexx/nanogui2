/*
 * nmail/nmail_toolbar.cpp — implementation of nmail_toolbar.h.
 */
#include "nmail_toolbar.h"

#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <nanogui/opengl.h>

#include <algorithm>

using namespace nanogui;

// ---------------------------------------------------------------------------
// MailToolbar
// ---------------------------------------------------------------------------
MailToolbar::MailToolbar(Widget *parent, int height) : Widget(parent) {
    set_min_height(height);
    set_height(height);
    set_min_size(Vector2i(0, height));
    set_height_flex(SizeMode::Fixed);

    // Apple Mail groups buttons purely with whitespace -- no chrome around
    // clusters -- so the gap here is the *between-group* gap; the tighter
    // within-group gap lives on MailToolbarGroup's own BoxLayout.
    m_flex = new FlexLayout(FlexDirection::Row, JustifyContent::FlexStart,
                            AlignItems::Center, 0, 18);
    m_flex->set_padding(14, 0);
    set_layout(m_flex);
}

MailToolbarGroup *MailToolbar::add_group() {
    return new MailToolbarGroup(this);
}

Widget *MailToolbar::add_spacer(int width) {
    Widget *sp = new Widget(this);
    sp->set_min_width(width);
    sp->set_width(width);
    return sp;
}

Widget *MailToolbar::add_flex_spacer() {
    Widget *sp = new Widget(this);
    sp->set_min_size(Vector2i(0, 0));
    m_flex->set_flex_item(sp, FlexLayout::FlexItem(1.0f, 1.0f, 0));
    return sp;
}

void MailToolbar::draw(NVGcontext *ctx) {
    float x = (float)m_pos.x(), y = (float)m_pos.y();
    float w = (float)m_size.x(), h = (float)m_size.y();

    // Flat fill -- Apple's toolbar is a matte vibrancy surface, not a glossy
    // one, so no specular wash here (that's what read as "80s beveling").
    nvgBeginPath(ctx);
    nvgRect(ctx, x, y, w, h);
    nvgFillColor(ctx, m_theme->m_menubar_fill);
    nvgFill(ctx);

    // One hairline separating it from the content below -- the entire
    // "chrome" the unified toolbar look needs.
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, x, y + h - 0.5f);
    nvgLineTo(ctx, x + w, y + h - 0.5f);
    nvgStrokeWidth(ctx, 1.f);
    nvgStrokeColor(ctx, m_theme->m_border_dark);
    nvgStroke(ctx);

    Widget::draw(ctx);
}

// ---------------------------------------------------------------------------
// MailToolbarGroup
// ---------------------------------------------------------------------------
// Deliberately draws nothing. In Apple Mail, a "group" isn't a pill or a
// bevel -- it's just buttons placed close together with clear air between
// clusters. This class exists only to apply that tight inner spacing; the
// looser spacing between groups comes from MailToolbar's own gap.
MailToolbarGroup::MailToolbarGroup(Widget *parent) : Widget(parent) {
    set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 2));
}

Button *MailToolbarGroup::add_button(int icon, const std::string &tooltip) {
    Button *btn = new Button(this, "", icon);
    btn->set_font_size(39);   // 25% smaller than the original flat-toolbar size (52)
    btn->set_tooltip(tooltip);
    // Transparent until hovered/pressed: Apple Mail's toolbar icons are bare
    // glyphs on the bar, only the theme's normal hover/press highlight shows.
    btn->set_transparent(true);
    return btn;
}
