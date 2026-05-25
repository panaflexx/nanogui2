/*
    src/split.cpp -- Split widget that divides space between two widgets

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/nanogui.h>
#include <nanogui/common.h>
#include <nanogui/screen.h>
#include <nanogui/widget.h>
#include <nanogui/theme.h>
#include <nanogui/cachedwidget.h>

NAMESPACE_BEGIN(nanogui)

#ifdef DEBUG
#define printf(fmt, ...)
#endif

/**
 * \class Split split.h nanogui/split.h
 *
 * \brief Split container widget that divides the space between two child widgets.
 *
 * This widget divides the available space between two child widgets, separated by a draggable
 * bar. The divider can be dragged to adjust the relative size of each widget. Supports both
 * horizontal (side-by-side) and vertical (stacked) arrangements.
 */
class Split : public Widget {
public:
    enum class Orientation {
        Horizontal = 0, ///< Split horizontally (side-by-side)
        Vertical        ///< Split vertically (stacked)
    };

    /// Default constructor
    Split(Widget *parent, Orientation orientation = Orientation::Horizontal)
        : Widget(parent), m_orientation(orientation),
          m_dragPosition(0.5f), m_dragging(false),
          m_minSplitSize(100, 100), m_maxSplitSize(INT_MAX, INT_MAX),
          m_firstWidget(nullptr), m_secondWidget(nullptr) {
    }

    /// Get the current orientation
    Orientation orientation() const { return m_orientation; }

    /// Set the orientation
    void set_orientation(Orientation orientation) {
        m_orientation = orientation;
        perform_layout(screen() ? screen()->nvg_context() : nullptr);
    }

    /// Get the current split position (0.0-1.0)
    float drag_position() const { return m_dragPosition; }

    /// Set the split position (0.0-1.0)
    void set_drag_position(float position) {
        position = std::max(0.0f, std::min(1.0f, position));
        m_dragPosition = position;
        perform_layout(screen() ? screen()->nvg_context() : nullptr);
    }

    /// Set the minimum size for each split panel
    void set_min_size(const Vector2i &minSize) { m_minSplitSize = minSize; }

    /// Set the maximum size for each split panel
    void set_max_size(const Vector2i &maxSize) { m_maxSplitSize = maxSize; }

    /// Get preferred size (override for custom layout)
    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        Vector2i size(0, 0);

        // Handle cases where we might not have children yet
        if (m_firstWidget && m_firstWidget->visible())
            size = m_firstWidget->preferred_size(ctx);

		//printf("preferred_size: steph 1 size=%d %d\n", size.x(), size.y() );

        if (m_secondWidget && m_secondWidget->visible()) {
            Vector2i secondSize = m_secondWidget->preferred_size(ctx);
            if (m_orientation == Orientation::Horizontal) {
                size.x() += secondSize.x();
                // Take the max of heights, not sum
                size.y() = std::max(size.y(), secondSize.y());
            } else {
                size.y() += secondSize.y();
                // Take the max of widths, not sum
                size.x() = std::max(size.x(), secondSize.x());
            }
        }
		//printf("preferred_size: steph 2 size=%d %d\n", size.x(), size.y() );

        // Add space for the drag bar
        if (m_orientation == Orientation::Horizontal) {
            size.x() += 6;
            // Ensure reasonable minimum height
            if (size.y() <= 0 && m_secondWidget)
                size.y() = m_secondWidget->height();
        } else {
            size.y() += 6;
            // Ensure reasonable minimum width
            if (size.x() <= 0 && m_secondWidget)
                size.x() = m_secondWidget->width();
        }

		//printf("preferred_size: final size=%d %d\n", size.x(), size.y() );
        return size;
    }

    /// Perform custom layout (override for custom layout)
    virtual void perform_layout(NVGcontext *ctx) override {
		if (m_children.empty())
			return;
		if (m_children.size() != 2)
			throw std::runtime_error("Split must have two children.");

        m_firstWidget = m_children[0];
        m_secondWidget = m_children[1];

        // Honor Widget::set_fill_parent (grow with parent's content area).
        apply_fill_parent();

        Vector2i areaSize = size();

        // If area is invalid, use parent size
        if (areaSize.x() <= 0 || areaSize.y() <= 0) {
            if (parent()) {
                areaSize = Vector2i(
                    parent()->width() > 0 ? parent()->width() : 800,
                    parent()->height() > 0 ? parent()->height() : 600
                );
                set_size(areaSize);
            } else {
                areaSize = Vector2i(800, 600);
                set_size(areaSize);
            }
        }
		//printf("perform_layout 2\n");

        // Calculate the available space for the divider
        int dragBarSize = 6;
        int availableSpace = (m_orientation == Orientation::Horizontal) ?
            areaSize.x() - dragBarSize : areaSize.y() - dragBarSize;

        if (availableSpace <= 0)
            return;

        // Calculate positions based on drag position
        float relativePos = m_dragPosition;
        int firstSize = std::round(relativePos * availableSpace);

		/*
		printf("perform_layout 3 m_parent size=%d %d\n",
				this->parent()->size().x(),
				this->parent()->size().y()
				);
		*/
        // Apply min/max constraints
        if (m_firstWidget && m_firstWidget->visible()) {
                    Vector2i childMin1 = m_firstWidget->min_size();
                    int splitDim = (m_orientation == Orientation::Horizontal) ? 0 : 1;
                    int dimMin1 = childMin1[splitDim];
                    int minSize = std::max((int)m_minSplitSize[splitDim], dimMin1);
                    int maxSize = std::min((int)m_maxSplitSize[splitDim], areaSize[splitDim]);
                    firstSize = std::max(minSize, std::min(firstSize, maxSize));
			//printf("perform_layout: 4 firstSize=%d\n");
        } else {
			//printf("m_firstWidget = %p \n", m_firstWidget);
		}

        if (m_secondWidget && m_secondWidget->visible()) {
                    Vector2i childMin2 = m_secondWidget->min_size();
                    int splitDim = (m_orientation == Orientation::Horizontal) ? 0 : 1;
                    int dimMin2 = childMin2[splitDim];
                    int secondPanelSize = availableSpace - firstSize;
                    secondPanelSize = std::max(dimMin2, secondPanelSize);
                    if (secondPanelSize + firstSize > availableSpace) {
                        firstSize = availableSpace - secondPanelSize;
                    }

			//printf("perform_layout: 5 firstSize=%d\n");
        } else {
			//printf("m_secondWidget = %p \n", m_firstWidget);
		}

        // Update drag position to reflect constraint adjustments
        m_dragPosition = static_cast<float>(firstSize) / static_cast<float>(availableSpace);

        // Position the first widget
        if (m_firstWidget && m_firstWidget->visible()) {
            Vector2i widgetSize = (m_orientation == Orientation::Horizontal) ?
                Vector2i(firstSize, areaSize.y()) :
                Vector2i(areaSize.x(), firstSize);
            m_firstWidget->set_size(widgetSize);
            m_firstWidget->set_position(Vector2i(0, 0));
            m_firstWidget->perform_layout(ctx);
			//printf("perform_layout: m_firstWidget size=%d %d \n", widgetSize.x(), widgetSize.y());
        }

        // Position the second widget
        if (m_secondWidget && m_secondWidget->visible()) {
            Vector2i widgetSize = (m_orientation == Orientation::Horizontal) ?
                Vector2i(areaSize.x() - firstSize - dragBarSize, areaSize.y()) :
                Vector2i(areaSize.x(), areaSize.y() - firstSize - dragBarSize);

            Vector2i widgetPos = (m_orientation == Orientation::Horizontal) ?
                Vector2i(firstSize + dragBarSize, 0) :
                Vector2i(0, firstSize + dragBarSize);

            m_secondWidget->set_size(widgetSize);
            m_secondWidget->set_position(widgetPos);
            m_secondWidget->perform_layout(ctx);
			//printf("perform_layout: m_secondWidget size=%d %d \n", widgetSize.x(), widgetSize.y());
        }

    }

    /// Draw the widget (including the drag bar)
    virtual void draw(NVGcontext *ctx) override {
        Widget::draw(ctx);  // Draw children

        // Only draw the splitter if we have valid context and both widgets
        if (!ctx || !m_firstWidget || !m_secondWidget)
            return;

        int dragBarSize = 6;  // hotspot / hit-test size — never changes
        int drawWidth = m_theme->m_split_divider_width;
        float visualPosition;

        if (m_orientation == Orientation::Horizontal) {
            visualPosition = m_dragPosition * (m_size.x() - dragBarSize);
            float drawOffset = (dragBarSize - drawWidth) * 0.5f;

            nvgBeginPath(ctx);
            nvgRect(ctx,
                m_pos.x() + visualPosition + drawOffset, m_pos.y(),
                drawWidth, m_size.y());
            nvgFillColor(ctx, m_theme->m_split_divider);
            nvgFill(ctx);
        } else {
            visualPosition = m_dragPosition * (m_size.y() - dragBarSize);
            float drawOffset = (dragBarSize - drawWidth) * 0.5f;

            nvgBeginPath(ctx);
            nvgRect(ctx,
                m_pos.x(), m_pos.y() + visualPosition + drawOffset,
                m_size.x(), drawWidth);
            nvgFillColor(ctx, m_theme->m_split_divider);
            nvgFill(ctx);
        }
    }

    /// Handle mouse button events (to start dragging)
    virtual bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        // Handle mouse release (to end dragging)
		//printf("mouse_button_event\n");

        if (!down && m_dragging && button == GLFW_MOUSE_BUTTON_1) {
            m_dragging = false;
            return true;
        }

        // Handle mouse press (to start dragging)
        if (button == GLFW_MOUSE_BUTTON_1 && down) {
            int dragBarSize = 6;
            float visualPosition;
            // p comes in parent-relative coordinates; convert to Split-local
            Vector2i lp = p - m_pos;

            if (m_orientation == Orientation::Horizontal) {
                visualPosition = m_dragPosition * (m_size.x() - dragBarSize);
                if (lp.x() >= visualPosition && lp.x() <= visualPosition + dragBarSize) {
                    m_dragging = true;
                    m_dragOffset = lp.x() - visualPosition;
                    return true;
                }
            } else {
                visualPosition = m_dragPosition * (m_size.y() - dragBarSize);
                if (lp.y() >= visualPosition && lp.y() <= visualPosition + dragBarSize) {
                    m_dragging = true;
                    m_dragOffset = lp.y() - visualPosition;
                    return true;
                }
            }
        }

        return Widget::mouse_button_event(p, button, down, modifiers);
    }

    /// Handle mouse drag events (to resize panels)
    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
        if (m_dragging) {
            float totalSize;
            float newPosition;
            // p comes in parent-relative coordinates; convert to Split-local
            Vector2i lp = p - m_pos;

            if (m_orientation == Orientation::Horizontal) {
                totalSize = m_size.x() - 6; // Subtract drag bar width
                newPosition = (float)(lp.x() - m_dragOffset) / totalSize;
            } else {
                totalSize = m_size.y() - 6; // Subtract drag bar height
                newPosition = (float)(lp.y() - m_dragOffset) / totalSize;
            }

            // Clamp position between 0 and 1
            newPosition = std::max(0.0f, std::min(1.0f, newPosition));
            m_dragPosition = newPosition;
			//printf("m_dragPosition = %f\n", m_dragPosition);

            // Update layout
            //perform_layout(screen() ? screen()->nvg_context() : nullptr);
			auto screen = dynamic_cast<Screen*>(m_parent->screen());
			if (screen) {
				screen->perform_layout();
				screen->redraw();
			}

            // Invalidate any parent CachedWidget caches
            for (Widget* w = parent(); w != nullptr; w = w->parent()) {
                if (auto* cw = dynamic_cast<CachedWidget*>(w)) {
                    cw->cache_dirty();
                }
            }

            return true;
        }
        return Widget::mouse_drag_event(p, rel, button, modifiers);
    }

	/// Handle mouse motion events (to detect hover over drag bar)
	virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override {
		// First, let the base Widget handle child motion events
		bool handled = Widget::mouse_motion_event(p, rel, button, modifiers);

		// Check if mouse is over the drag bar
		int dragBarSize = 6;
		bool overDragBar = false;
		// p comes in parent-relative coordinates; convert to Split-local
		Vector2i lp = p - m_pos;

		if (m_orientation == Orientation::Horizontal) {
			float visualPosition = m_dragPosition * (m_size.x() - dragBarSize);
			if (lp.x() >= visualPosition && lp.x() <= visualPosition + dragBarSize) {
				overDragBar = true;
			}
		} else {
			float visualPosition = m_dragPosition * (m_size.y() - dragBarSize);
			if (lp.y() >= visualPosition && lp.y() <= visualPosition + dragBarSize) {
				overDragBar = true;
			}
		}

		if (overDragBar) {
			// Optionally change cursor to resize cursor
			set_cursor(Cursor::HResize); // or VResize for vertical
			handled = true;
		} else {
			// Reset cursor when not over drag bar
			set_cursor(Cursor::Arrow);
		}

		return handled;
	}


protected:
    Orientation m_orientation;
    float m_dragPosition;      // Relative position of the drag bar (0.0-1.0)
    bool m_dragging;           // Is the drag bar currently being dragged?
    int m_dragOffset;          // Offset within the drag bar where dragging started
    Vector2i m_minSplitSize;   // Minimum size for each split panel
    Vector2i m_maxSplitSize;   // Maximum size for each split panel
    Widget *m_firstWidget;     // First child widget (left or top)
    Widget *m_secondWidget;    // Second child widget (right or bottom)
};

NAMESPACE_END(nanogui)
