/*
 guieditor_json.h -- JSON save/load support for nanogui layouts.

 Uses dict.h for JSON parsing/serialization. The on-disk format is
 the same hierarchical "type/id/children" structure used by client.json
 extended with optional editor-specific fields such as position,
 size, caption, value, etc.
*/

#ifndef GUIEDITOR_JSON_H
#define GUIEDITOR_JSON_H

#include <string>
#include <functional>

class GUIEditor;

namespace nanogui { class Widget; }
typedef struct DictValue DictValue;

namespace guieditor_json {

/// Factory signature used by the generic loader.
/// Implementations should create and return a widget of `type` parented to
/// `parent`. The raw parsed JSON object (`json`) is supplied so that the
/// factory can inspect application-specific fields (e.g. ngserver's
/// "rootWindow" flag). Return nullptr if `type` is unsupported.
using WidgetFactory = std::function<nanogui::Widget*(const std::string& type,
                                                     nanogui::Widget* parent,
                                                     DictValue* json)>;

/// Generic loader: parse JSON file at `path` and build a widget tree
/// underneath `root_parent`, using `factory` to construct each widget.
/// Common fields (id, position, size, fixed_size, layout, caption, value,
/// checked, color, text/label/title aliases) are applied automatically.
bool load_layout_into(nanogui::Widget* root_parent,
                      const std::string& path,
                      const WidgetFactory& factory);

/// Same as load_layout_into but parses an in-memory JSON string.
bool load_layout_from_string(nanogui::Widget* root_parent,
                             const std::string& json_text,
                             const WidgetFactory& factory);

// --- GUIEditor convenience wrappers ---

/// Save the GUIEditor's canvas layout to `path`. Returns true on success.
bool save_layout(GUIEditor* editor, const std::string& path);

/// Replace the GUIEditor's canvas contents with the layout loaded from `path`.
/// Uses GUIEditor::create_widget_by_type as the factory.
bool load_layout(GUIEditor* editor, const std::string& path);

} // namespace guieditor_json

#endif // GUIEDITOR_JSON_H
