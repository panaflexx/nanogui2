/*
 * pagemade/ruler.h — PageMaker's rulers along the top and left of the pages.
 *
 * RulerFrame holds the composer's scroll panel with a ruler on each of those
 * edges and the corner square between them; with the rulers hidden the panel
 * has the whole frame. The rulers read in inches from the current page's
 * top-left, follow the scroll and zoom, and shade the page's extent.
 *
 * Drag off a ruler onto the pages for a ruler guide: the top ruler gives a
 * horizontal guide, the left one a vertical guide. It lands on the page
 * under the pointer, or on every page with Shift. Released back over the
 * rulers, nothing is added.
 */
#pragma once

#include <nanogui/widget.h>

#include <functional>

class ComposerView;

class Ruler : public nanogui::Widget {
public:
    Ruler(nanogui::Widget *parent, bool horizontal);
    void set_view(ComposerView *view) { m_view = view; }

    void draw(NVGcontext *ctx) override;
    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down,
                            int modifiers) override;
    bool mouse_drag_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel,
                          int button, int modifiers) override;

private:
    /* Shift as it is now: the screen only refreshes its modifiers on clicks */
    bool shift_down(int modifiers) const;

    ComposerView *m_view = nullptr;
    bool m_horizontal;
    bool m_dragging = false;
};

class RulerFrame : public nanogui::Widget {
public:
    static constexpr int kThickness = 18;

    explicit RulerFrame(nanogui::Widget *parent);
    /* The scroll panel (a child of this frame) and the view inside it */
    void set_content(nanogui::Widget *content, ComposerView *view);

    bool rulers_shown() const { return m_shown; }
    void set_rulers_shown(bool shown);

    /* Room kept free on each side (for a docked window): the rulers and
     * pages lay out between. */
    void set_side_insets(int left, int right);
    /* Called after every layout, so a docked window can follow */
    std::function<void()> on_layout;

    nanogui::Vector2i preferred_size(NVGcontext *ctx) const override;
    void perform_layout(NVGcontext *ctx) override;
    void draw(NVGcontext *ctx) override;

private:
    Ruler *m_top = nullptr, *m_left = nullptr;
    nanogui::Widget *m_content = nullptr;
    bool m_shown = true;
    int m_inset_left = 0, m_inset_right = 0;
};
