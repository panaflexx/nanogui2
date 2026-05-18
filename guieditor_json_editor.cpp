/*
 guieditor_json_editor.cpp -- GUIEditor-specific wrappers around the
 generic guieditor_json API. Builds JSON from the editor's canvas
 (save_layout) and reloads layouts back into the canvas (load_layout).
*/

#include "guieditor_json.h"
#include "guieditor.h"

#include "dict.h"

#include <nanogui/window.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/textbox.h>
#include <nanogui/checkbox.h>
#include <nanogui/slider.h>
#include <nanogui/colorpicker.h>
#include <nanogui/layout.h>

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cerrno>

using namespace nanogui;

namespace {

// ---------- local dict helpers ----------------------------------------------

DictValue* make_vec2i(const Vector2i& v) {
    DictValue* arr = dict_create_array();
    dict_array_append(arr, dict_create_int64(v.x()));
    dict_array_append(arr, dict_create_int64(v.y()));
    return arr;
}

DictValue* make_color(const Color& c) {
    DictValue* arr = dict_create_array();
    dict_array_append(arr, dict_create_int64((int)(c.r() * 255.0f)));
    dict_array_append(arr, dict_create_int64((int)(c.g() * 255.0f)));
    dict_array_append(arr, dict_create_int64((int)(c.b() * 255.0f)));
    dict_array_append(arr, dict_create_int64((int)(c.w() * 255.0f)));
    return arr;
}

std::string layout_to_string(Widget* w) {
    Layout* lay = w->layout();
    if (!lay) return "";
    if (dynamic_cast<GroupLayout*>(lay)) return "GroupLayout";
    if (BoxLayout* bl = dynamic_cast<BoxLayout*>(lay))
        return bl->orientation() == Orientation::Vertical ? "VBoxLayout" : "HBoxLayout";
    if (dynamic_cast<GridLayout*>(lay)) return "GridLayout";
    if (dynamic_cast<FlexLayout*>(lay)) return "FlexLayout";
    return "";
}

// ---------- save -------------------------------------------------------------

DictValue* widget_to_json(GUIEditor* editor, Widget* w);

void append_common(DictValue* obj, GUIEditor* editor, Widget* w) {
    dict_object_set(obj, "id", dict_create_string(w->id().c_str()));
    dict_object_set(obj, "type", dict_create_string(editor->getWidgetTypeName(w).c_str()));
    dict_object_set(obj, "position", make_vec2i(w->position()));
    dict_object_set(obj, "size", make_vec2i(w->size()));
    if (w->fixed_size().x() != 0 || w->fixed_size().y() != 0)
        dict_object_set(obj, "fixed_size", make_vec2i(w->fixed_size()));

    std::string ltype = layout_to_string(w);
    if (!ltype.empty())
        dict_object_set(obj, "layout", dict_create_string(ltype.c_str()));
}

DictValue* widget_to_json(GUIEditor* editor, Widget* w) {
    DictValue* obj = dict_create_object();
    append_common(obj, editor, w);

    std::string type = editor->getWidgetTypeName(w);

    if (type == "Window") {
        if (Window* win = dynamic_cast<Window*>(w))
            dict_object_set(obj, "caption", dict_create_string(win->title().c_str()));
    } else if (type == "Label") {
        if (Label* lbl = dynamic_cast<Label*>(w))
            dict_object_set(obj, "caption", dict_create_string(lbl->caption().c_str()));
    } else if (type == "Button") {
        if (Button* btn = dynamic_cast<Button*>(w))
            dict_object_set(obj, "caption", dict_create_string(btn->caption().c_str()));
    } else if (type == "Text Box") {
        if (TextBox* tb = dynamic_cast<TextBox*>(w))
            dict_object_set(obj, "value", dict_create_string(tb->value().c_str()));
    } else if (type == "Checkbox") {
        if (CheckBox* cb = dynamic_cast<CheckBox*>(w)) {
            dict_object_set(obj, "caption", dict_create_string(cb->caption().c_str()));
            dict_object_set(obj, "checked", dict_create_bool(cb->checked() ? 1 : 0));
        }
    } else if (type == "Slider") {
        if (Slider* sl = dynamic_cast<Slider*>(w))
            dict_object_set(obj, "value", dict_create_number((double)sl->value()));
    } else if (type == "Color Picker") {
        if (ColorPicker* cp = dynamic_cast<ColorPicker*>(w))
            dict_object_set(obj, "color", make_color(cp->color()));
    }

    // Recurse into children for container types only
    bool descend = (type == "Window" || type == "Pane" || type == "Widget" || type == "View");
    if (descend) {
        DictValue* children = dict_create_array();
        for (Widget* c : w->children()) {
            std::string ctype = editor->getWidgetTypeName(c);
            if (ctype == "Widget" && c->children().empty())
                continue;
            dict_array_append(children, widget_to_json(editor, c));
        }
        if (children->array_value.length > 0) {
            dict_object_set(obj, "children", children);
        } else {
            dict_destroy(children);
        }
    }

    return obj;
}

} // namespace

namespace guieditor_json {

bool save_layout(GUIEditor* editor, const std::string& path) {
    if (!editor || !editor->canvas_win) return false;

    DictValue* root = dict_create_object();
    dict_object_set(root, "type", dict_create_string("Canvas"));
    dict_object_set(root, "id", dict_create_string(editor->canvas_win->id().c_str()));
    dict_object_set(root, "size", make_vec2i(editor->canvas_win->size()));

    // Top widget (canvas) layout, if any.
    std::string top_ltype = layout_to_string(editor->canvas_win);
    if (!top_ltype.empty())
        dict_object_set(root, "layout", dict_create_string(top_ltype.c_str()));

    DictValue* children = dict_create_array();
    for (Widget* c : editor->canvas_win->children()) {
        dict_array_append(children, widget_to_json(editor, c));
    }
    dict_object_set(root, "children", children);

    size_t buf_size = 1 << 20; // 1 MB
    std::vector<char> buf(buf_size);
    char* out = dict_serialize_json(root, buf.data(), buf.size(), 1);
    dict_destroy(root);
    if (!out) {
        std::cerr << "[guieditor_json] Serialization failed (buffer too small?)\n";
        return false;
    }

    size_t out_len = std::strlen(out);
    std::cout << "[guieditor_json] Saving " << out_len << " bytes to '" << path << "'\n";

    std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        std::cerr << "[guieditor_json] Could not open '" << path
                  << "' for writing: " << std::strerror(errno) << "\n";
        return false;
    }
    f.write(out, (std::streamsize)out_len);
    if (!f.good()) {
        std::cerr << "[guieditor_json] Write failed: " << std::strerror(errno) << "\n";
        return false;
    }
    f.close();
    std::cout << "[guieditor_json] Saved layout to " << path << "\n";
    return true;
}

bool load_layout(GUIEditor* editor, const std::string& path) {
    if (!editor) return false;
    editor->clear_canvas();
    WidgetFactory factory = [editor](const std::string& type, Widget* parent,
                                     DictValue* /*json*/) -> Widget* {
        return editor->create_widget_by_type(type, parent);
    };
    return load_layout_into(editor->canvas_win, path, factory);
}

} // namespace guieditor_json
