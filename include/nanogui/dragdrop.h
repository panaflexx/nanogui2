/*
    nanogui/dragdrop.h -- Drag and drop between widgets.

    A source widget starts a drag from its own mouse_drag_event() with
    Screen::start_drag(), handing over the data, a preview (image + label)
    and the actions it allows.  While the button is held the screen draws the
    preview under the cursor and routes the pointer through the widget tree
    like a motion event (so containers such as ZoomScrollPanel convert the
    point for their children):

      target  drag_motion_event()   accept (DragEvent::accept) to show a drop
              drag_leave_event()    the pointer left, or the drag ended
              drag_drop_event()     the button was released over it
      source  drag_finished()       the action performed, DropNone if none

    Escape, or releasing where nothing accepts, cancels.  Files dropped from
    the desktop arrive the same way, as "text/uri-list" with no source.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
/** \file */

#pragma once

#include <nanogui/vector.h>
#include <nanogui/any.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

NAMESPACE_BEGIN(nanogui)

class Widget;

/// What a drop does with the data (a source allows a mask of these).
enum DropAction : int {
    DropNone = 0,
    DropCopy = 1,
    DropMove = 2,
    DropLink = 4
};

/**
 * The dragged data, keyed by MIME type as desktop drag and drop is
 * ("text/uri-list", "text/plain", "application/x-myapp-thing", ...).
 * \ref object carries an in-process value (an asset handle, say) that never
 * leaves the application.
 */
class NANOGUI_EXPORT DragData {
public:
    void set(const std::string &mime, std::string bytes);
    /// The bytes for a type, or nullptr when the drag doesn't offer it.
    const std::string *get(const std::string &mime) const;
    bool has(const std::string &mime) const { return get(mime) != nullptr; }
    std::vector<std::string> formats() const;

    /// Local file paths from "text/uri-list" (file: URIs only, decoded).
    std::vector<std::string> file_paths() const;
    /// "text/uri-list" for local paths (file: URIs, CRLF separated).
    static std::string uri_list(const std::vector<std::string> &paths);

    /* In-process value (an asset handle, say). nanogui::any, not std::any:
     * macOS 10.12's libc++ has no <any>. */
    any object;

private:
    std::vector<std::pair<std::string, std::string>> m_formats;
};

/// What follows the cursor: an image (owned RGBA pixels) and/or a label.
struct DragPreview {
    std::vector<uint8_t> rgba;  ///< straight RGBA, w * h * 4; empty for none
    int w = 0, h = 0;
    std::string label;
    Vector2i hotspot;           ///< cursor position within the image
};

/// Passed down the widget tree while a drag is over the application.
struct NANOGUI_EXPORT DragEvent {
    DragEvent(const DragData &data, int allowed, int modifiers, Widget *source)
        : data(data), allowed(allowed), modifiers(modifiers), source(source) { }

    const DragData &data;
    int allowed;                ///< DropAction mask offered by the source
    int modifiers;
    Widget *source;             ///< nullptr for drags from outside the app

    /* Filled in by the target that accepts */
    Widget *target = nullptr;
    DropAction action = DropNone;
    /// Set by a container that auto-scrolled, so the screen keeps the
    /// motion going while the pointer rests at its edge.
    bool autoscroll = false;

    /// Accept as `preferred` if the source allows it, else the first allowed
    /// action.  Returns false (and changes nothing) if none is allowed.
    bool accept(Widget *w, DropAction preferred = DropCopy);
};

NAMESPACE_END(nanogui)
