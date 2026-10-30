/*
    nanogui/clipboard.h -- system clipboard as a list of MIME strings.

    GLFW's clipboard is one UTF-8 string. Inkscape (and other drawing
    programs) offer the SVG as image/svg+xml, which that string does not
    ask for. This reads those targets on X11 and falls back to the UTF-8
    string everywhere else. Writing goes through glfwSetClipboardFormats,
    a nanogui2 addition to the bundled GLFW.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/
#pragma once

#include <nanogui/common.h>

#include <string>
#include <utility>
#include <vector>

struct GLFWwindow;

NAMESPACE_BEGIN(nanogui)

/* Each entry is a MIME type and its bytes (UTF-8 text for the text
 * targets). Empty when nobody owns the clipboard. The wait is short, and
 * there is no wait at all when the selection has no owner, so a menu can
 * call this. On X11 the targets asked for are image/svg+xml, image/svg,
 * text/xml, text/plain and the UTF-8 string. */
NANOGUI_EXPORT std::vector<std::pair<std::string, std::string>>
read_clipboard(GLFWwindow *window);

/* Put several representations on the clipboard, richest first: MIME type
 * and bytes. A paste in another application takes the first one it
 * understands. A "text/plain;charset=utf-8" entry also answers plain-text
 * requests. X11 and Wayland offer every format; other platforms offer only
 * the text one for now. While this application owns the clipboard,
 * read_clipboard returns these formats without a round trip. */
NANOGUI_EXPORT void
write_clipboard(GLFWwindow *window, const std::vector<std::pair<std::string, std::string>> &formats);

NAMESPACE_END(nanogui)
