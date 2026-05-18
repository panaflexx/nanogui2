/*
 json2cpp.cpp -- Command-line utility that reads a guieditor JSON layout
 file (the same format produced by guieditor / consumed by guieditor_json.cpp)
 and emits one or two C++ source files that, when compiled, construct an
 equivalent nanogui widget tree directly in code (i.e. the generated
 output does NOT read JSON at runtime).

 Usage:
   json2cpp [options] <input.json>

 Options:
   -classname  <Name>     C++ class name to generate (default: GuiClass)
   --directory <dir>      Output directory (default: current directory)
   -fullmain              Also emit `mainapp.cpp` with a Screen subclass
                          and main() suitable for building a complete app.
                          Without this flag only `guiclass.cpp` is created.

 Output files (placed in --directory):
   guiclass.h
   guiclass.cpp
   mainapp.cpp           (only with -fullmain)
*/

#include "dict.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Small dict.h helpers (duplicated from guieditor_json.cpp so that this
// utility does not depend on nanogui itself -- it only needs dict.h).
// ---------------------------------------------------------------------------

static DictValue* get_field(DictValue* obj, const char* key) {
    if (!obj || obj->type != DICT_OBJECT) return nullptr;
    return dict_object_get(obj, key);
}

static bool get_string(DictValue* obj, const char* key, std::string& out) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_STRING || !v->string_value) return false;
    out = v->string_value;
    return true;
}

static bool get_number_val(DictValue* v, double& out) {
    if (!v) return false;
    if (v->type == DICT_NUMBER) { out = v->number_value;          return true; }
    if (v->type == DICT_INT64)  { out = (double)v->int64_value;   return true; }
    return false;
}

static bool get_bool(DictValue* obj, const char* key, bool& out) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_BOOL) return false;
    out = v->bool_value != 0;
    return true;
}

static bool get_vec2i(DictValue* obj, const char* key, int& x, int& y) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_ARRAY || v->array_value.length < 2) return false;
    double dx = 0, dy = 0;
    if (!get_number_val(v->array_value.items[0], dx)) return false;
    if (!get_number_val(v->array_value.items[1], dy)) return false;
    x = (int)dx; y = (int)dy;
    return true;
}

static bool get_color4(DictValue* obj, const char* key, int rgba[4]) {
    DictValue* v = get_field(obj, key);
    if (!v || v->type != DICT_ARRAY || v->array_value.length < 4) return false;
    for (int i = 0; i < 4; i++) {
        double n = 0;
        if (!get_number_val(v->array_value.items[i], n)) return false;
        rgba[i] = (int)n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

static std::string escape_cstr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20)
                    out += "?";
                else
                    out += c;
        }
    }
    return out;
}

static std::string sanitize_var(const std::string& id, int counter) {
    std::string v;
    for (char c : id) {
        if (std::isalnum((unsigned char)c) || c == '_') v += (char)std::tolower((unsigned char)c);
    }
    if (v.empty() || std::isdigit((unsigned char)v[0])) v = "w_" + v;
    v += "_" + std::to_string(counter);
    return v;
}

// ---------------------------------------------------------------------------
// Widget type mapping
// ---------------------------------------------------------------------------

struct TypeInfo {
    const char* json_type;        // canonical match
    const char* alt_type;         // alternate match (or nullptr)
    const char* cpp_class;        // nanogui C++ class
    const char* extra_ctor_arg;   // ctor: new C(parent, <extra>) ; nullptr -> none
};

static const TypeInfo* lookup_type(const std::string& t) {
    static const TypeInfo table[] = {
        { "Window",       nullptr,       "nanogui::Window",      "\"\"" },
        { "Pane",         "View",        "nanogui::Widget",      nullptr },
        { "Widget",       nullptr,       "nanogui::Widget",      nullptr },
        { "Label",        nullptr,       "nanogui::Label",       "\"\"" },
        { "Button",       nullptr,       "nanogui::Button",      "\"\"" },
        { "Text Box",     "TextBox",     "nanogui::TextBox",     nullptr },
        { "Checkbox",     "CheckBox",    "nanogui::CheckBox",    "\"\"" },
        { "Slider",       nullptr,       "nanogui::Slider",      nullptr },
        { "Color Picker", "ColorPicker", "nanogui::ColorPicker", nullptr },
        { "Dropdown",     nullptr,       "nanogui::Dropdown",    nullptr },
        { nullptr, nullptr, nullptr, nullptr }
    };
    for (const TypeInfo* it = table; it->json_type; ++it) {
        if (t == it->json_type) return it;
        if (it->alt_type && t == it->alt_type) return it;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Code generator
// ---------------------------------------------------------------------------

struct Gen {
    std::ostringstream body;
    int counter = 0;
    int indent_lvl = 2;
    std::string indent() const { return std::string(indent_lvl * 4, ' '); }
};

static void gen_widget(Gen& g, DictValue* obj, const std::string& parent_var);

static void gen_layout(Gen& g, const std::string& var, const std::string& ltype) {
    if (ltype.empty()) return;
    if (ltype == "GroupLayout")
        g.body << g.indent() << var << "->set_layout(new nanogui::GroupLayout());\n";
    else if (ltype == "VBoxLayout")
        g.body << g.indent() << var
               << "->set_layout(new nanogui::BoxLayout(nanogui::Orientation::Vertical));\n";
    else if (ltype == "HBoxLayout")
        g.body << g.indent() << var
               << "->set_layout(new nanogui::BoxLayout(nanogui::Orientation::Horizontal));\n";
    else if (ltype == "GridLayout")
        g.body << g.indent() << var << "->set_layout(new nanogui::GridLayout());\n";
    else if (ltype == "FlexLayout")
        g.body << g.indent() << var << "->set_layout(new nanogui::FlexLayout());\n";
}

static void gen_widget(Gen& g, DictValue* obj, const std::string& parent_var) {
    if (!obj || obj->type != DICT_OBJECT) return;

    std::string type;
    if (!get_string(obj, "type", type)) return;

    const TypeInfo* info = lookup_type(type);
    if (!info) {
        std::cerr << "[json2cpp] Warning: unsupported widget type '" << type << "', skipping\n";
        return;
    }

    std::string id;
    get_string(obj, "id", id);
    std::string var = sanitize_var(id, ++g.counter);

    // Construct the widget. Note: nanogui widgets register themselves
    // with their parent in their constructor, so we don't need any
    // add_child call.
    g.body << g.indent() << "auto* " << var << " = new " << info->cpp_class
           << "(" << parent_var;
    if (info->extra_ctor_arg)
        g.body << ", " << info->extra_ctor_arg;
    g.body << ");\n";

    if (!id.empty())
        g.body << g.indent() << var << "->set_id(\"" << escape_cstr(id) << "\");\n";

    int x = 0, y = 0;
    if (get_vec2i(obj, "position", x, y))
        g.body << g.indent() << var << "->set_position(nanogui::Vector2i(" << x << ", " << y << "));\n";
    if (get_vec2i(obj, "size", x, y))
        g.body << g.indent() << var << "->set_size(nanogui::Vector2i(" << x << ", " << y << "));\n";
    if (get_vec2i(obj, "fixed_size", x, y))
        g.body << g.indent() << var << "->set_fixed_size(nanogui::Vector2i(" << x << ", " << y << "));\n";

    std::string ltype;
    if (get_string(obj, "layout", ltype))
        gen_layout(g, var, ltype);

    // type-specific
    std::string sval;
    bool bval = false;
    double nval = 0;
    int rgba[4];

    if (type == "Window") {
        if (get_string(obj, "caption", sval) || get_string(obj, "title", sval))
            g.body << g.indent() << var << "->set_title(\"" << escape_cstr(sval) << "\");\n";
    } else if (type == "Label") {
        if (get_string(obj, "caption", sval) || get_string(obj, "text", sval))
            g.body << g.indent() << var << "->set_caption(\"" << escape_cstr(sval) << "\");\n";
    } else if (type == "Button") {
        if (get_string(obj, "caption", sval) || get_string(obj, "label", sval))
            g.body << g.indent() << var << "->set_caption(\"" << escape_cstr(sval) << "\");\n";
    } else if (type == "Text Box" || type == "TextBox") {
        if (get_string(obj, "value", sval))
            g.body << g.indent() << var << "->set_value(\"" << escape_cstr(sval) << "\");\n";
    } else if (type == "Checkbox" || type == "CheckBox") {
        if (get_string(obj, "caption", sval))
            g.body << g.indent() << var << "->set_caption(\"" << escape_cstr(sval) << "\");\n";
        if (get_bool(obj, "checked", bval))
            g.body << g.indent() << var << "->set_checked(" << (bval ? "true" : "false") << ");\n";
    } else if (type == "Slider") {
        DictValue* vv = get_field(obj, "value");
        if (get_number_val(vv, nval)) {
            std::ostringstream lit;
            lit.precision(9);
            lit << (float)nval;
            std::string s = lit.str();
            if (s.find('.') == std::string::npos && s.find('e') == std::string::npos
                && s.find('E') == std::string::npos)
                s += ".0";
            g.body << g.indent() << var << "->set_value(" << s << "f);\n";
        }
    } else if (type == "Color Picker" || type == "ColorPicker") {
        if (get_color4(obj, "color", rgba))
            g.body << g.indent() << var << "->set_color(nanogui::Color("
                   << rgba[0] << ", " << rgba[1] << ", " << rgba[2] << ", " << rgba[3] << "));\n";
    }

    // Remember widget by id so users can retrieve it later
    if (!id.empty())
        g.body << g.indent() << "m_widgets[\"" << escape_cstr(id) << "\"] = " << var << ";\n";

    // Recurse children
    DictValue* children = get_field(obj, "children");
    if (children && children->type == DICT_ARRAY && children->array_value.length > 0) {
        g.body << g.indent() << "{\n";
        g.indent_lvl++;
        for (size_t i = 0; i < children->array_value.length; ++i)
            gen_widget(g, children->array_value.items[i], var);
        g.indent_lvl--;
        g.body << g.indent() << "}\n";
    }
}

// ---------------------------------------------------------------------------
// File writing
// ---------------------------------------------------------------------------

static bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[json2cpp] Failed to open '" << path << "' for writing\n";
        return false;
    }
    f << contents;
    return true;
}

static std::string make_header(const std::string& classname) {
    std::ostringstream s;
    std::string guard = classname;
    for (char& c : guard) c = (char)std::toupper((unsigned char)c);
    guard += "_H";

    s << "/* Auto-generated by json2cpp -- DO NOT EDIT BY HAND */\n"
      << "#ifndef " << guard << "\n"
      << "#define " << guard << "\n\n"
      << "#include <nanogui/widget.h>\n"
      << "#include <string>\n"
      << "#include <unordered_map>\n\n"
      << "namespace nanogui { class Widget; }\n\n"
      << "class " << classname << " {\n"
      << "public:\n"
      << "    explicit " << classname << "(nanogui::Widget* parent);\n\n"
      << "    /// Lookup a widget previously created via this class by its id.\n"
      << "    nanogui::Widget* widget(const std::string& id) const {\n"
      << "        auto it = m_widgets.find(id);\n"
      << "        return it == m_widgets.end() ? nullptr : it->second;\n"
      << "    }\n\n"
      << "    nanogui::Widget* root() const { return m_root; }\n\n"
      << "private:\n"
      << "    nanogui::Widget* m_root = nullptr;\n"
      << "    std::unordered_map<std::string, nanogui::Widget*> m_widgets;\n"
      << "};\n\n"
      << "#endif // " << guard << "\n";
    return s.str();
}

static std::string make_source(const std::string& classname,
                               DictValue* root,
                               int canvas_w, int canvas_h) {
    std::ostringstream s;
    s << "/* Auto-generated by json2cpp -- DO NOT EDIT BY HAND */\n"
      << "#include \"guiclass.h\"\n\n"
      << "#include <nanogui/widget.h>\n"
      << "#include <nanogui/window.h>\n"
      << "#include <nanogui/label.h>\n"
      << "#include <nanogui/button.h>\n"
      << "#include <nanogui/textbox.h>\n"
      << "#include <nanogui/checkbox.h>\n"
      << "#include <nanogui/slider.h>\n"
      << "#include <nanogui/colorpicker.h>\n"
      << "#include <nanogui/combobox.h>\n"
      << "#include <nanogui/menu.h>\n"
      << "#include <nanogui/layout.h>\n"
      << "#include <nanogui/common.h>\n\n"
      << classname << "::" << classname << "(nanogui::Widget* parent) {\n"
      << "    m_root = parent;\n";

    if (canvas_w > 0 && canvas_h > 0) {
        s << "    // Original canvas size in the editor was "
          << canvas_w << " x " << canvas_h << "\n";
    }

    Gen g;
    g.indent_lvl = 1;

    // The JSON root is the editor "Canvas". Each of its children becomes a
    // child of `parent`.
    std::string rtype;
    if (root && root->type == DICT_OBJECT && get_string(root, "type", rtype) && rtype == "Canvas") {
        DictValue* children = get_field(root, "children");
        if (children && children->type == DICT_ARRAY) {
            for (size_t i = 0; i < children->array_value.length; ++i)
                gen_widget(g, children->array_value.items[i], "parent");
        }
    } else if (root && root->type == DICT_OBJECT) {
        gen_widget(g, root, "parent");
    } else if (root && root->type == DICT_ARRAY) {
        for (size_t i = 0; i < root->array_value.length; ++i)
            gen_widget(g, root->array_value.items[i], "parent");
    }

    s << g.body.str();
    s << "}\n";
    return s.str();
}

static std::string make_mainapp(const std::string& classname,
                                int canvas_w, int canvas_h) {
    int win_w = canvas_w > 0 ? canvas_w : 800;
    int win_h = canvas_h > 0 ? canvas_h : 600;

    std::ostringstream s;
    s << "/* Auto-generated by json2cpp -- DO NOT EDIT BY HAND */\n"
      << "#include \"guiclass.h\"\n\n"
      << "#include <nanogui/opengl.h>\n"
      << "#include <nanogui/screen.h>\n"
      << "#include <nanogui/widget.h>\n"
      << "#include <memory>\n"
      << "#include <iostream>\n\n"
      << "class MainApp : public nanogui::Screen {\n"
      << "public:\n"
      << "    MainApp()\n"
      << "        : nanogui::Screen(nanogui::Vector2i(" << win_w << ", " << win_h
      << "), \"" << classname << "\", true) {\n"
      << "        m_gui = std::make_unique<" << classname << ">(this);\n"
      << "        perform_layout();\n"
      << "    }\n\n"
      << "    virtual bool keyboard_event(int key, int scancode, int action, int modifiers) override {\n"
      << "        if (nanogui::Screen::keyboard_event(key, scancode, action, modifiers))\n"
      << "            return true;\n"
      << "        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {\n"
      << "            set_visible(false);\n"
      << "            return true;\n"
      << "        }\n"
      << "        return false;\n"
      << "    }\n\n"
      << "private:\n"
      << "    std::unique_ptr<" << classname << "> m_gui;\n"
      << "};\n\n"
      << "int main(int /*argc*/, char** /*argv*/) {\n"
      << "    try {\n"
      << "        nanogui::init();\n"
      << "        {\n"
      << "            nanogui::ref<MainApp> app = new MainApp();\n"
      << "            app->draw_all();\n"
      << "            app->set_visible(true);\n"
      << "            nanogui::mainloop(1 / 60.0f * 1000);\n"
      << "        }\n"
      << "        nanogui::shutdown();\n"
      << "    } catch (const std::exception& e) {\n"
      << "        std::cerr << \"Runtime error: \" << e.what() << std::endl;\n"
      << "        return -1;\n"
      << "    }\n"
      << "    return 0;\n"
      << "}\n";
    return s.str();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void print_usage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " [options] <input.json>\n"
        "Options:\n"
        "  -classname  <Name>   C++ class name to generate (default: GuiClass)\n"
        "  --directory <dir>    Output directory (default: .)\n"
        "  -fullmain            Also write mainapp.cpp with main() and a Screen subclass\n"
        "  -h, --help           Show this message\n";
}

int main(int argc, char** argv) {
    std::string input;
    std::string classname = "GuiClass";
    std::string directory = ".";
    bool fullmain = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (a == "-fullmain" || a == "--fullmain") {
            fullmain = true;
        } else if (a == "-classname" || a == "--classname") {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            classname = argv[i];
        } else if (a == "--directory" || a == "-directory" || a == "-d") {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            directory = argv[i];
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "[json2cpp] Unknown option: " << a << "\n";
            print_usage(argv[0]);
            return 1;
        } else {
            if (input.empty()) input = a;
            else {
                std::cerr << "[json2cpp] Unexpected positional argument: " << a << "\n";
                return 1;
            }
        }
    }

    if (input.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Read JSON
    std::ifstream f(input);
    if (!f.is_open()) {
        std::cerr << "[json2cpp] Could not open input file: " << input << "\n";
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    char err[1024];
    DictValue* root = dict_deserialize_json(content.c_str(), content.size(),
                                            content.size(), err, sizeof(err));
    if (!root) {
        std::cerr << "[json2cpp] JSON parse error: " << err << "\n";
        return 1;
    }

    // Determine canvas size (if specified)
    int canvas_w = 0, canvas_h = 0;
    if (root->type == DICT_OBJECT) {
        std::string rtype;
        if (get_string(root, "type", rtype) && rtype == "Canvas")
            get_vec2i(root, "size", canvas_w, canvas_h);
    }

    // Best-effort: assume caller has already created the output directory.
    std::string sep = (!directory.empty() && directory.back() != '/') ? "/" : "";
    std::string hpath = directory + sep + "guiclass.h";
    std::string cpath = directory + sep + "guiclass.cpp";
    std::string mpath = directory + sep + "mainapp.cpp";

    std::string header_src = make_header(classname);
    std::string cpp_src    = make_source(classname, root, canvas_w, canvas_h);

    bool ok = true;
    ok = write_file(hpath, header_src) && ok;
    ok = write_file(cpath, cpp_src)    && ok;

    if (fullmain) {
        std::string main_src = make_mainapp(classname, canvas_w, canvas_h);
        ok = write_file(mpath, main_src) && ok;
    }

    dict_destroy(root);

    if (!ok) {
        std::cerr << "[json2cpp] One or more output files could not be written.\n";
        return 1;
    }

    std::cout << "[json2cpp] Wrote " << hpath << "\n";
    std::cout << "[json2cpp] Wrote " << cpath << "\n";
    if (fullmain)
        std::cout << "[json2cpp] Wrote " << mpath << "\n";
    return 0;
}
