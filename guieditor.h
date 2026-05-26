/*
 guieditor.h -- Header for GUIEditor class, a professional-grade GUI editor using NanoGUI.

 (C) Roger Davenport 2025

 All rights reserved. Use of this source code is governed by a
 BSD-style license that can be found in the LICENSE.txt file.
*/

#ifndef GUIEDITOR_H
#define GUIEDITOR_H

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/window.h>
#include <nanogui/layout.h>
#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/textbox.h>
#include <nanogui/label.h>
#include <nanogui/combobox.h>
#include <nanogui/colorpicker.h>
#include <nanogui/textarea.h>

#include <vector>
#include <string>
#include <functional>
#include <map>

namespace nanogui {
    class Widget;
    class Window;
    class Button;
    class CheckBox;
    class Label;
    class TextBox;
    class ComboBox;
    class Dropdown;
    class ColorPicker;
    class TextArea;
}

using namespace nanogui;
using std::vector;
using std::string;
using std::function;

// ---------------------------------------------------------------------------
// WidgetDef – one entry in the editor's placeable widget palette.
// The registry (GUIEditor::m_widget_defs) drives widget creation, ID
// generation, type-name lookup, and JSON load/save — all in one place.
// Adding a new widget type only requires adding one WidgetDef entry.
// ---------------------------------------------------------------------------
struct WidgetDef {
    int         icon;         ///< FA_XXX toolbar constant (the registry key)
    std::string type_name;    ///< Canonical name used by the JSON loader
    std::string id_prefix;    ///< Prefix for auto-generated IDs, e.g. "LABEL"
    Vector2i    default_size; ///< Default fixed_size when first placed

    /// Create, size, and initialise a widget parented to `parent`. No position set.
    std::function<Widget*(Widget* parent)> factory;

    /// Return true if the given runtime widget instance is this type.
    std::function<bool(Widget*)>           matches;
};

class GUIEditor : public Screen {
public:
    // -----------------------------------------------------------------------
    // Public state — read by EditorWidget overlays and the JSON layer
    // -----------------------------------------------------------------------
    Widget        *selected_widget  = nullptr;
    int            current_tool     = 0;
    bool           dragging         = false;
    bool           test_mode        = false;   ///< true = run mode, false = edit mode
    Widget        *original_parent  = nullptr;
    Widget        *potential_parent = nullptr;
    Vector2i       drag_start, drag_offset;
    Vector2i       drag_initial_pos;  ///< mouse position at the moment dragging began

    // Group / rubber-band selection
    std::vector<Widget*>        selected_widgets;          ///< all widgets in a group selection
    bool                        rubber_banding    = false;
    Vector2i                    rubber_band_start, rubber_band_end;
    bool                        group_dragging    = false;
    std::map<Widget*, Vector2i> group_initial_positions;   ///< abs position of each widget at drag start
    Vector2i                    group_drag_start;          ///< cursor position when group drag began
    Window        *canvas_win       = nullptr;
    Window        *editor_win       = nullptr;
    Widget        *properties_pane  = nullptr;
    vector<Button*> tool_buttons;

    GUIEditor();

    bool   update_properties();
    string getWidgetTypeName(Widget *widget);
    string generateUniqueId(int icon);

    // Used by the JSON loader
    Widget* create_widget_by_type(const std::string& type, Widget* parent);
    void    clear_canvas();

    bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override;
    bool mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override;
    bool mouse_drag_event(const Vector2i &p, const Vector2i &rel, int button, int modifiers) override;
    bool keyboard_event(int key, int scancode, int action, int modifiers) override;
    bool resize_event(const Vector2i &size) override;
    void draw(NVGcontext *ctx) override;

private:
    // -----------------------------------------------------------------------
    // Snap-to-grid
    // -----------------------------------------------------------------------
    int      snap_grid_size = 0;
    Vector2i snap(const Vector2i &pos);

    // -----------------------------------------------------------------------
    // Widget registry
    // -----------------------------------------------------------------------
    std::vector<WidgetDef> m_widget_defs;
    std::map<int, int>     m_widget_counters;  ///< per-icon placement counters
    void    init_widget_registry();
    Widget* place_widget(int icon, Widget* target, const Vector2i& pos);

    // -----------------------------------------------------------------------
    // Resize handle state
    // -----------------------------------------------------------------------
    bool     resizing          = false;
    int      resize_handle     = -1;
    Vector2i resize_start_pos;
    Vector2i resize_start_size;
    Widget*  find_widget_with_handle(const Vector2i& p);
    Widget*  find_widget_with_handle_recursive(Widget* w, const Vector2i& p);

    // -----------------------------------------------------------------------
    // Properties panel sub-sections
    // -----------------------------------------------------------------------
    CheckBox *test_mode_checkbox = nullptr;
    Button    *test_mode_button   = nullptr;
    Label     *properties_label   = nullptr;
    void add_canvas_properties();
    void add_common_properties();
    void add_type_properties();
    void add_layout_properties();
    void add_dimension_properties();
    void add_animation_properties();
    void add_color_properties();

    // -----------------------------------------------------------------------
    // Layout helpers
    // -----------------------------------------------------------------------
    bool        canHaveLayout(Widget* widget);
    std::string getCurrentLayoutType(Widget* widget);
    int         getLayoutTypeIndex(const std::string& type);
    void        applyLayoutType(Widget* widget, int type_index);
    void        addLayoutSpecificControls(Widget* widget);
    void        addBoxLayoutControls(BoxLayout* layout);
    void        addGridLayoutControls(GridLayout* layout);
    void        addFlexLayoutControls(FlexLayout* layout);
    void        addGroupLayoutControls(GroupLayout* layout);
};

#endif // GUIEDITOR_H
