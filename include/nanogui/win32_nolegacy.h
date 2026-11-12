/*
 * Force-included on MSVC. windows.h (via rpcndr.h / windef.h) defines
 * near, far and small as macros. This tree uses those words as identifiers.
 * Including windows.h here, then undefining the macros, makes every later
 * #include <windows.h> a no-op so the macros stay gone.
 */
#ifndef NANOGUI_WIN32_NOLEGACY_H
#define NANOGUI_WIN32_NOLEGACY_H

#if defined(_WIN32)
/* Not WIN32_LEAN_AND_MEAN: that drops commdlg.h, and file_dialog() needs
 * OPENFILENAME. NOMINMAX keeps std::min / std::max intact. */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#undef small
#endif

#endif
