/*
 * pagemade/spinbox.h — number fields for the palettes and dialogs.
 *
 * A SpinBox is a nanogui FloatBox or IntBox set up for quick adjustment:
 * the arrows on its left step it by the increment, as does the scroll wheel
 * over it or a right-drag across it, clamped to its range. Typing a value
 * still works.
 *
 * Two things the plain boxes don't do:
 *  - A box can show a word instead of a number ("Auto" leading) or nothing
 *    (a mixed selection). A spin starts from blank_value then, e.g. the
 *    automatic leading, rather than from 0.
 *  - spinning() is true while a step's callback runs, so the app can fold a
 *    run of steps into one undo step.
 */
#pragma once

#include <nanogui/textbox.h>

#include <cstdlib>
#include <functional>

template <typename Box, typename Scalar> class SpinBox : public Box {
public:
    SpinBox(nanogui::Widget *parent, Scalar value, Scalar increment, Scalar lo, Scalar hi)
        : Box(parent, value) {
        Box::set_editable(true);
        Box::set_spinnable(true);
        Box::set_value_increment(increment);
        Box::set_min_max_values(lo, hi);
        Box::set_value(value);
    }

    /* Where a spin starts when the box doesn't show a number. */
    void set_blank_value(std::function<Scalar()> fn) { m_blank_value = std::move(fn); }
    bool spinning() const { return m_spinning; }

    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down,
                            int modifiers) override {
        const bool spin = down && (button == 1 /* GLFW_MOUSE_BUTTON_2 */ ||
                                   this->spin_area(p) != nanogui::TextBox::SpinArea::None);
        return step([&] { return Box::mouse_button_event(p, button, down, modifiers); }, spin);
    }
    bool mouse_drag_event(const nanogui::Vector2i &p, const nanogui::Vector2i &rel, int button,
                          int modifiers) override {
        return step([&] { return Box::mouse_drag_event(p, rel, button, modifiers); }, false);
    }
    bool scroll_event(const nanogui::Vector2i &p, const nanogui::Vector2f &rel) override {
        return step([&] { return Box::scroll_event(p, rel); }, true);
    }

private:
    bool shows_number() const {
        const std::string &s = nanogui::TextBox::value();
        char *end = nullptr;
        std::strtod(s.c_str(), &end);
        return end != s.c_str();
    }

    template <typename F> bool step(F &&event, bool starting) {
        if (!this->enabled())
            return false;                // nothing to adjust (no selection)
        if (!this->spinnable() || this->focused())
            return event();
        if (starting && m_blank_value && !shows_number())
            Box::set_value(m_blank_value());
        m_spinning = true;
        const bool handled = event();
        m_spinning = false;
        return handled;
    }

    std::function<Scalar()> m_blank_value;
    bool m_spinning = false;
};

using FloatSpin = SpinBox<nanogui::FloatBox<float>, float>;
using IntSpin = SpinBox<nanogui::IntBox<int>, int>;
