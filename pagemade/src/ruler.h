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
#include <vector>

#include "composer/story.h"

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

    bool mouse_motion_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel,
                            int button, int modifiers) override;

    /* The story editor's top ruler: the kind of tab a click adds (the
     * corner square shows it and cycles it) */
    pagemade::TabAlign new_tab() const { return m_new_tab; }
    void cycle_new_tab();

private:
    /* Shift as it is now: the screen only refreshes its modifiers on clicks */
    bool shift_down(int modifiers) const;

    /* Story mode: the indent and tab markers. Positions in points from the
     * column's left edge; `zero` and `scale` map them to the parent's
     * coordinates, as draw() does. */
    enum class Mark { None, First, Left, Right, Tab };
    bool story_map(float &zero, float &scale) const;
    Mark mark_at(const nanogui::Vector2i &p, int &tab) const;
    bool story_press(const nanogui::Vector2i &p, int modifiers);
    void story_drag(const nanogui::Vector2i &p, int modifiers);
    void story_release();
    void apply_tabs(const std::vector<pagemade::TabStop> &tabs);

    ComposerView *m_view = nullptr;
    bool m_horizontal;
    bool m_dragging = false;

    pagemade::TabAlign m_new_tab = pagemade::TabAlign::Left;
    Mark  m_mark = Mark::None;           // the marker being dragged
    int   m_tab = -1;                    // its index in m_tabs
    bool  m_tab_off = false;             // dragged off the ruler: removed
    float m_grab = 0.f;                  // pointer minus marker, in points
    std::vector<pagemade::TabStop> m_tabs;   // the stops as the drag began
    /* The marker shown enlarged: in hand, or settling back after a drop */
    Mark   m_flash = Mark::None;
    int    m_flash_tab = -1;
    double m_flash_t0 = 0.0;
    double m_last_click = -1.0;
    int    m_last_tab = -1;
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
    /* In the story editor the corner square picks the kind of tab */
    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down,
                            int modifiers) override;

private:
    ComposerView *m_view = nullptr;
    Ruler *m_top = nullptr, *m_left = nullptr;
    nanogui::Widget *m_content = nullptr;
    bool m_shown = true;
    int m_inset_left = 0, m_inset_right = 0;
};
