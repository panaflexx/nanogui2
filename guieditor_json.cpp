/*
 guieditor_json.cpp -- JSON save/load support for GUIEditor layouts.

 Uses dict.h for JSON parsing/serialization. The format is the same
 hierarchical "type/id/children" structure used by ngserver.cpp
 (see client.json) extended with editor-specific fields:
   - position: [x, y]
   - size: [w, h]
   - fixed_size: [w, h]
   - caption (Window title / Label / Button / CheckBox)
   - value (TextBox string, Slider float)
   - checked (CheckBox bool)
   - color (ColorPicker [r,g,b,a] in 0..255)
   - items (Dropdown array of strings)
   - layout: "GroupLayout" | "VBoxLayout" | "HBoxLayout" | "GridLayout" | "FlexLayout"
*/

#include "guieditor_json.h"

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
#include <sstream>
#include <iostream>
#include <cstring>
#include <vector>
#include <string>

using namespace nanogui;

namespace {

// ---------- dict.h helpers ---------------------------------------------------

DictValue* get_field(DictValue* obj, const char* key) {
    if (!obj || obj->type != DICT_OBJECT) return nullptr;
    return dict_object_get(obj, key);
}

bool get_string(DictValue* obj, const char* key, std::string& out) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_STRING || !v->string_value) return false;
    out = v->string_value;
    return true;
}

bool get_number(DictValue* v, double& out) {
    if (!v) return false;
    if (v->type == DICT_NUMBER) { out = v->number_value; return true; }
    if (v->type == DICT_INT64)  { out = (double)v->int64_value; return true; }
    return false;
}

bool get_bool(DictValue* obj, const char* key, bool& out) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_BOOL) return false;
    out = v->bool_value != 0;
    return true;
}

bool get_vec2i(DictValue* obj, const char* key, Vector2i& out) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_ARRAY || v->array_value.length < 2) return false;
    double x = 0, y = 0;
    if (!get_number(v->array_value.items[0], x)) return false;
    if (!get_number(v->array_value.items[1], y)) return false;
    out = Vector2i((int)x, (int)y);
    return true;
}

bool get_color(DictValue* obj, const char* key, Color& out) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_ARRAY || v->array_value.length < 4) return false;
    double c[4];
    for (int i = 0; i < 4; i++) {
        if (!get_number(v->array_value.items[i], c[i])) return false;
    }
    out = Color((int)c[0], (int)c[1], (int)c[2], (int)c[3]);
    return true;
}

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

// ---------- layout helpers ---------------------------------------------------

std::string layout_to_string(Widget* w) {
    Layout* lay = w->layout();
    if (!lay) return "";
    if (dynamic_cast<GroupLayout*>(lay))                       return "GroupLayout";
    if (BoxLayout* bl = dynamic_cast<BoxLayout*>(lay))
        return bl->orientation() == Orientation::Vertical ? "VBoxLayout" : "HBoxLayout";
    if (dynamic_cast<GridLayout*>(lay))                        return "GridLayout";
    if (dynamic_cast<FlexLayout*>(lay))                        return "FlexLayout";
    return "";
}

void apply_layout(Widget* w, const std::string& type) {
    if (type.empty()) return;
    if (type == "GroupLayout")      w->set_layout(new GroupLayout());
    else if (type == "VBoxLayout")  w->set_layout(new BoxLayout(Orientation::Vertical));
    else if (type == "HBoxLayout")  w->set_layout(new BoxLayout(Orientation::Horizontal));
    else if (type == "GridLayout")  w->set_layout(new GridLayout());
    else if (type == "FlexLayout")  w->set_layout(new FlexLayout());
}

// ---------- load -------------------------------------------------------------

void build_from_json(const guieditor_json::WidgetFactory& factory,
                     DictValue* obj, Widget* parent) {
    if (!obj || obj->type != DICT_OBJECT) return;

    std::string type;
    if (!get_string(obj, "type", type)) return;

    Widget* w = factory(type, parent, obj);
    if (!w) {
        std::cerr << "[guieditor_json] Unknown widget type '" << type << "'\n";
        return;
    }

    std::string id;
    if (get_string(obj, "id", id) && !id.empty())
        w->set_id(id);

    Vector2i pos;
    if (get_vec2i(obj, "position", pos))
        w->set_position(pos);
    Vector2i size;
    if (get_vec2i(obj, "size", size))
        w->set_size(size);
    Vector2i fsize;
    if (get_vec2i(obj, "fixed_size", fsize))
        w->set_fixed_size(fsize);

    std::string ltype;
    if (get_string(obj, "layout", ltype))
        apply_layout(w, ltype);

    // Type-specific properties
    std::string sval;
    bool bval;
    Color cval;
    double nval;

    if (type == "Window") {
        if (Window* win = dynamic_cast<Window*>(w)) {
            if (get_string(obj, "caption", sval) || get_string(obj, "title", sval))
                win->set_title(sval);
        }
    } else if (type == "Label") {
        if (Label* lbl = dynamic_cast<Label*>(w)) {
            if (get_string(obj, "caption", sval) || get_string(obj, "text", sval))
                lbl->set_caption(sval);
        }
    } else if (type == "Button") {
        if (Button* btn = dynamic_cast<Button*>(w)) {
            if (get_string(obj, "caption", sval) || get_string(obj, "label", sval))
                btn->set_caption(sval);
        }
    } else if (type == "Text Box" || type == "TextBox") {
        if (TextBox* tb = dynamic_cast<TextBox*>(w)) {
            if (get_string(obj, "value", sval))
                tb->set_value(sval);
        }
    } else if (type == "Checkbox" || type == "CheckBox") {
        if (CheckBox* cb = dynamic_cast<CheckBox*>(w)) {
            if (get_string(obj, "caption", sval))
                cb->set_caption(sval);
            if (get_bool(obj, "checked", bval))
                cb->set_checked(bval);
        }
    } else if (type == "Slider") {
        if (Slider* sl = dynamic_cast<Slider*>(w)) {
            DictValue* vv = get_field(obj, "value");
            if (get_number(vv, nval))
                sl->set_value((float)nval);
        }
    } else if (type == "Color Picker" || type == "ColorPicker") {
        if (ColorPicker* cp = dynamic_cast<ColorPicker*>(w)) {
            if (get_color(obj, "color", cval))
                cp->set_color(cval);
        }
    }

    // Children
    DictValue* children = get_field(obj, "children");
    if (children && children->type == DICT_ARRAY) {
        for (size_t i = 0; i < children->array_value.length; ++i) {
            build_from_json(factory, children->array_value.items[i], w);
        }
    }
}

bool load_string_with_factory(Widget* root_parent,
                              const std::string& content,
                              const guieditor_json::WidgetFactory& factory) {
    if (content.empty()) {
        std::cerr << "[guieditor_json] Empty JSON content\n";
        return false;
    }
    char err[1024];
    DictValue* root = dict_deserialize_json(content.c_str(), content.size(),
                                            content.size(), err, sizeof(err));
    if (!root) {
        std::cerr << "[guieditor_json] JSON parse error: " << err << "\n";
        return false;
    }

    DictValue* type_val = root->type == DICT_OBJECT ? dict_object_get(root, "type") : nullptr;
    std::string type_str = (type_val && type_val->type == DICT_STRING && type_val->string_value)
                               ? type_val->string_value : "";

    if (type_str == "Canvas") {
        DictValue* children = dict_object_get(root, "children");
        if (children && children->type == DICT_ARRAY) {
            for (size_t i = 0; i < children->array_value.length; ++i) {
                build_from_json(factory, children->array_value.items[i], root_parent);
            }
        }
    } else if (root->type == DICT_OBJECT) {
        build_from_json(factory, root, root_parent);
    } else if (root->type == DICT_ARRAY) {
        for (size_t i = 0; i < root->array_value.length; ++i) {
            build_from_json(factory, root->array_value.items[i], root_parent);
        }
    }

    dict_destroy(root);
    return true;
}

} // namespace

namespace guieditor_json {

bool load_layout_from_string(nanogui::Widget* root_parent,
                             const std::string& json_text,
                             const WidgetFactory& factory) {
    if (!root_parent || !factory) return false;
    return load_string_with_factory(root_parent, json_text, factory);
}

bool load_layout_into(nanogui::Widget* root_parent,
                      const std::string& path,
                      const WidgetFactory& factory) {
    if (!root_parent || !factory) return false;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[guieditor_json] Could not open '" << path << "' for reading\n";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!load_string_with_factory(root_parent, ss.str(), factory))
        return false;
    std::cout << "[guieditor_json] Loaded layout from " << path << "\n";
    return true;
}

} // namespace guieditor_json
