/*
    nanogui/split.h -- Split widget that divides space between two widgets

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include <nanogui/opengl.h>
#include <nanogui/nanogui.h>
#include <nanogui/common.h>
#include <nanogui/screen.h>
#include <nanogui/widget.h>
#include <nanogui/theme.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

NAMESPACE_BEGIN(nanogui)

/**
 * \class Split split.h nanogui/split.h
 *
 * \brief Split container widget that divides the space between two child widgets.
 *
 * Divides available space between two children with a draggable divider.
 * Supports horizontal (side-by-side) and vertical (stacked) layouts.
 *
 * By default the divider is stored as a fraction of the available span, so both
 * panes scale when the Split is resized. Call \ref set_keep_size_on_resize(true)
 * to peg the first pane to an absolute pixel size instead; the second pane
 * absorbs all growth/shrink. Dragging the bar still updates that peg.
 */
class Split : public Widget {
public:
    enum class Orientation {
        Horizontal = 0, ///< Side-by-side (first = left)
        Vertical        ///< Stacked (first = top)
    };

    Split(Widget *parent, Orientation orientation = Orientation::Horizontal)
        : Widget(parent), m_orientation(orientation),
          m_dragPosition(0.5f), m_dragging(false), m_dragOffset(0),
          m_minSplitSize(100, 100), m_maxSplitSize(INT_MAX, INT_MAX),
          m_firstWidget(nullptr), m_secondWidget(nullptr),
          m_keepSizeOnResize(false), m_fixedFirstSize(-1) {
        set_cached(true);
    }

    Orientation orientation() const { return m_orientation; }

    void set_orientation(Orientation orientation) {
        m_orientation = orientation;
        if (auto *ctx = nvg())
            perform_layout(ctx);
    }

    /// Fractional divider position in [0, 1] (used when keep-size is off, and for hit-testing).
    float drag_position() const { return m_dragPosition; }

    void set_drag_position(float position) {
        m_dragPosition = std::max(0.0f, std::min(1.0f, position));
        // Seed / update absolute peg from the new fraction if we already know our size.
        int avail = available_space();
        if (avail > 0)
            m_fixedFirstSize = (int)std::lround(m_dragPosition * (float)avail);
        else
            m_fixedFirstSize = -1;
        if (auto *ctx = nvg())
            perform_layout(ctx);
    }

    /**
     * When true, the first pane keeps a fixed pixel width (horizontal) or height
     * (vertical) across parent resizes. The second pane takes the remainder.
     * When false (default), the divider is a percentage of the available span.
     */
    bool keep_size_on_resize() const { return m_keepSizeOnResize; }

    void set_keep_size_on_resize(bool keep) {
        if (m_keepSizeOnResize == keep)
            return;
        m_keepSizeOnResize = keep;
        if (keep) {
            int avail = available_space();
            if (avail > 0)
                m_fixedFirstSize = (int)std::lround(m_dragPosition * (float)avail);
            else
                m_fixedFirstSize = -1;
        }
        if (auto *ctx = nvg())
            perform_layout(ctx);
    }

    /// Absolute first-pane size in pixels along the split axis (-1 if unset).
    int fixed_first_size() const { return m_fixedFirstSize; }

    /// Explicitly set the pegged first-pane size (also enables keep-size semantics for that value).
    void set_fixed_first_size(int pixels) {
        m_fixedFirstSize = std::max(0, pixels);
        int avail = available_space();
        if (avail > 0)
            m_dragPosition = std::max(0.f, std::min(1.f, (float)m_fixedFirstSize / (float)avail));
        if (auto *ctx = nvg())
            perform_layout(ctx);
    }

    /// Minimum size for each split panel (along the split axis; both components used).
    void set_min_size(const Vector2i &minSize) { m_minSplitSize = minSize; }

    /// Maximum size for each split panel (along the split axis).
    void set_max_size(const Vector2i &maxSize) { m_maxSplitSize = maxSize; }

    Vector2i preferred_size(NVGcontext *ctx) const override {
        Vector2i size(0, 0);

        if (m_firstWidget && m_firstWidget->visible())
            size = m_firstWidget->preferred_size(ctx);

        if (m_secondWidget && m_secondWidget->visible()) {
            Vector2i secondSize = m_secondWidget->preferred_size(ctx);
            if (m_orientation == Orientation::Horizontal) {
                size.x() += secondSize.x();
                size.y() = std::max(size.y(), secondSize.y());
            } else {
                size.y() += secondSize.y();
                size.x() = std::max(size.x(), secondSize.x());
            }
        }

        if (m_orientation == Orientation::Horizontal) {
            size.x() += drag_bar_size();
            if (size.y() <= 0 && m_secondWidget)
                size.y() = m_secondWidget->height();
        } else {
            size.y() += drag_bar_size();
            if (size.x() <= 0 && m_secondWidget)
                size.x() = m_secondWidget->width();
        }
        return size;
    }

    void perform_layout(NVGcontext *ctx) override {
        if (m_children.empty())
            return;
        if (m_children.size() != 2)
            throw std::runtime_error("Split must have two children.");

        m_firstWidget = m_children[0];
        m_secondWidget = m_children[1];

        apply_fill_parent();

        Vector2i areaSize = size();
        if (areaSize.x() <= 0 || areaSize.y() <= 0) {
            if (parent()) {
                areaSize = Vector2i(
                    parent()->width() > 0 ? parent()->width() : 800,
                    parent()->height() > 0 ? parent()->height() : 600);
                set_size(areaSize);
            } else {
                areaSize = Vector2i(800, 600);
                set_size(areaSize);
            }
        }

        const int dragBarSize = drag_bar_size();
        const int splitDim = (m_orientation == Orientation::Horizontal) ? 0 : 1;
        int availableSpace = areaSize[splitDim] - dragBarSize;
        if (availableSpace <= 0)
            return;

        // Desired first-pane size: absolute peg or fraction of available span.
        int firstSize;
        if (m_keepSizeOnResize) {
            if (m_fixedFirstSize < 0)
                m_fixedFirstSize = (int)std::lround(m_dragPosition * (float)availableSpace);
            firstSize = m_fixedFirstSize;
        } else {
            firstSize = (int)std::lround(m_dragPosition * (float)availableSpace);
        }

        // Min/max for first pane
        if (m_firstWidget && m_firstWidget->visible()) {
            int dimMin1 = m_firstWidget->min_size()[splitDim];
            int minSize = std::max(m_minSplitSize[splitDim], dimMin1);
            int maxSize = std::min(m_maxSplitSize[splitDim], availableSpace);
            firstSize = std::max(minSize, std::min(firstSize, maxSize));
        }

        // Guarantee second pane minimum when possible
        if (m_secondWidget && m_secondWidget->visible()) {
            int dimMin2 = m_secondWidget->min_size()[splitDim];
            int secondPanelSize = availableSpace - firstSize;
            if (secondPanelSize < dimMin2) {
                firstSize = availableSpace - dimMin2;
                // Re-apply first-pane floor after carving out second min
                if (m_firstWidget && m_firstWidget->visible()) {
                    int dimMin1 = m_firstWidget->min_size()[splitDim];
                    int minSize = std::max(m_minSplitSize[splitDim], dimMin1);
                    firstSize = std::max(minSize, firstSize);
                }
                firstSize = std::max(0, std::min(firstSize, availableSpace));
            }
        }

        // Sync fractional position for hit-testing / drawing.
        // When keep-size is on, do NOT write temporary clamps back into the peg —
        // only user drag / set_fixed_first_size / set_drag_position update the peg.
        // (If the window was too small, firstSize may be < m_fixedFirstSize until it grows.)
        m_dragPosition = availableSpace > 0
            ? (float)firstSize / (float)availableSpace : 0.f;

        if (m_firstWidget && m_firstWidget->visible()) {
            Vector2i widgetSize = (m_orientation == Orientation::Horizontal)
                ? Vector2i(firstSize, areaSize.y())
                : Vector2i(areaSize.x(), firstSize);
            m_firstWidget->set_size(widgetSize);
            m_firstWidget->set_position(Vector2i(0, 0));
            m_firstWidget->perform_layout(ctx);
        }

        if (m_secondWidget && m_secondWidget->visible()) {
            Vector2i widgetSize = (m_orientation == Orientation::Horizontal)
                ? Vector2i(areaSize.x() - firstSize - dragBarSize, areaSize.y())
                : Vector2i(areaSize.x(), areaSize.y() - firstSize - dragBarSize);
            Vector2i widgetPos = (m_orientation == Orientation::Horizontal)
                ? Vector2i(firstSize + dragBarSize, 0)
                : Vector2i(0, firstSize + dragBarSize);
            m_secondWidget->set_size(widgetSize);
            m_secondWidget->set_position(widgetPos);
            m_secondWidget->perform_layout(ctx);
        }
    }

    void draw(NVGcontext *ctx) override {
        Widget::draw(ctx);

        if (!ctx || !m_firstWidget || !m_secondWidget)
            return;

        const int dragBarSize = drag_bar_size();
        const int drawWidth = m_theme ? m_theme->m_split_divider_width : 1;
        float visualPosition = divider_pixel_offset();

        if (m_orientation == Orientation::Horizontal) {
            float drawOffset = (dragBarSize - drawWidth) * 0.5f;
            nvgBeginPath(ctx);
            nvgRect(ctx,
                    m_pos.x() + visualPosition + drawOffset, m_pos.y(),
                    (float)drawWidth, (float)m_size.y());
            nvgFillColor(ctx, m_theme ? m_theme->m_split_divider : Color(128, 255));
            nvgFill(ctx);
        } else {
            float drawOffset = (dragBarSize - drawWidth) * 0.5f;
            nvgBeginPath(ctx);
            nvgRect(ctx,
                    m_pos.x(), m_pos.y() + visualPosition + drawOffset,
                    (float)m_size.x(), (float)drawWidth);
            nvgFillColor(ctx, m_theme ? m_theme->m_split_divider : Color(128, 255));
            nvgFill(ctx);
        }
    }

    bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        if (!down && m_dragging && button == GLFW_MOUSE_BUTTON_1) {
            m_dragging = false;
            return true;
        }

        if (button == GLFW_MOUSE_BUTTON_1 && down) {
            const int dragBarSize = drag_bar_size();
            Vector2i lp = p - m_pos;
            float visualPosition = divider_pixel_offset();

            if (m_orientation == Orientation::Horizontal) {
                if (lp.x() >= visualPosition && lp.x() <= visualPosition + dragBarSize) {
                    m_dragging = true;
                    m_dragOffset = lp.x() - (int)visualPosition;
                    return true;
                }
            } else {
                if (lp.y() >= visualPosition && lp.y() <= visualPosition + dragBarSize) {
                    m_dragging = true;
                    m_dragOffset = lp.y() - (int)visualPosition;
                    return true;
                }
            }
        }

        return Widget::mouse_button_event(p, button, down, modifiers);
    }

    bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (m_dragging) {
            Vector2i lp = p - m_pos;
            int avail = available_space();
            if (avail <= 0)
                return true;

            int firstSize;
            if (m_orientation == Orientation::Horizontal)
                firstSize = lp.x() - m_dragOffset;
            else
                firstSize = lp.y() - m_dragOffset;

            firstSize = std::max(0, std::min(firstSize, avail));
            m_fixedFirstSize = firstSize;
            m_dragPosition = (float)firstSize / (float)avail;

            if (auto *scr = dynamic_cast<Screen *>(m_parent ? m_parent->screen() : nullptr)) {
                scr->perform_layout();
                scr->redraw();
            }

            cache_dirty();
            if (m_firstWidget) m_firstWidget->cache_dirty();
            if (m_secondWidget) m_secondWidget->cache_dirty();
            for (Widget *w = parent(); w != nullptr; w = w->parent()) {
                if (w->cached())
                    w->cache_dirty();
            }
            return true;
        }
        return Widget::mouse_drag_event(p, rel, button, modifiers);
    }

    bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        bool handled = Widget::mouse_motion_event(p, rel, button, modifiers);

        const int dragBarSize = drag_bar_size();
        Vector2i lp = p - m_pos;
        float visualPosition = divider_pixel_offset();
        bool overDragBar = false;

        if (m_orientation == Orientation::Horizontal) {
            if (lp.x() >= visualPosition && lp.x() <= visualPosition + dragBarSize)
                overDragBar = true;
        } else {
            if (lp.y() >= visualPosition && lp.y() <= visualPosition + dragBarSize)
                overDragBar = true;
        }

        if (overDragBar) {
            set_cursor(m_orientation == Orientation::Horizontal ? Cursor::HResize
                                                                : Cursor::VResize);
            handled = true;
        } else {
            set_cursor(Cursor::Arrow);
        }
        return handled;
    }

protected:
    static constexpr int drag_bar_size() { return 6; }

    NVGcontext *nvg() const {
        return screen() ? screen()->nvg_context() : nullptr;
    }

    int available_space() const {
        int span = (m_orientation == Orientation::Horizontal) ? m_size.x() : m_size.y();
        return span - drag_bar_size();
    }

    /// Pixel offset of the divider bar within this Split (local coords).
    /// Uses m_dragPosition, which perform_layout keeps in sync with the laid-out first pane.
    float divider_pixel_offset() const {
        int avail = available_space();
        return avail > 0 ? m_dragPosition * (float)avail : 0.f;
    }

    Orientation m_orientation;
    float m_dragPosition;       ///< Relative divider (0–1)
    bool m_dragging;
    int m_dragOffset;
    Vector2i m_minSplitSize;
    Vector2i m_maxSplitSize;
    Widget *m_firstWidget;
    Widget *m_secondWidget;
    bool m_keepSizeOnResize;    ///< Peg first pane to m_fixedFirstSize on resize
    int m_fixedFirstSize;       ///< Absolute first-pane size in px (-1 = unset)
};

NAMESPACE_END(nanogui)
