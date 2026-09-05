/*
 * nmail/nmail_toolbar.h — a macOS Mail-style unified toolbar.
 *
 * MailToolbar is a fixed-height flat bar (the frosted material used by
 * MenuBar, plus one hairline separator below) that holds icon buttons
 * arranged in MailToolbarGroup clusters -- e.g. {Reply, Forward} or
 * {Trash, Junk} -- plus a flexible spacer and trailing widgets such as the
 * search box. As in Apple Mail, grouping is expressed with whitespace alone:
 * buttons are plain nanogui::Button instances, transparent until hovered or
 * pressed, with no pill/bevel chrome around each cluster.
 */
#pragma once

#include <nanogui/widget.h>
#include <nanogui/button.h>
#include <nanogui/layout.h>

#include <string>

class MailToolbarGroup;

class MailToolbar : public nanogui::Widget {
public:
    explicit MailToolbar(nanogui::Widget *parent, int height = 60);

    // Starts a new button cluster; add buttons to it via
    // MailToolbarGroup::add_button().
    MailToolbarGroup *add_group();

    // A fixed-width gap between groups.
    nanogui::Widget *add_spacer(int width = 12);

    // A gap that grows to consume free space, pushing whatever follows
    // (e.g. the search box) to the right edge of the bar.
    nanogui::Widget *add_flex_spacer();

    void draw(NVGcontext *ctx) override;

private:
    nanogui::FlexLayout *m_flex = nullptr;
};

// A tightly-spaced cluster of related toolbar buttons. Draws no chrome of
// its own -- the grouping reads from spacing alone (tight within the
// cluster, looser between clusters), matching Apple Mail.
class MailToolbarGroup : public nanogui::Widget {
public:
    explicit MailToolbarGroup(nanogui::Widget *parent);

    // Adds an icon button (transparent until hover/press) to this group and
    // returns it for further configuration (callback, etc).
    nanogui::Button *add_button(int icon, const std::string &tooltip);
};
