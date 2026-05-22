/* ==========================================================================
 *  piece_table.h
 *
 *  A piece-table backed text buffer designed as the storage layer for a
 *  GUI text editor with tree-sitter integration.
 *
 *  Design notes
 *  ------------
 *  - All offsets and lengths are in BYTES.
 *  - PT_Point.column is a byte column within the line, matching the
 *    convention used by tree-sitter's TSPoint.
 *  - In-memory line endings are normalized to '\n'.  The original on-disk
 *    line-ending style is detected on load and can be restored by
 *    pt_save_file().
 *  - Functions that accept (text, text_len) do not require NUL termination
 *    and accept embedded NULs.
 *  - Mutating functions optionally fill in a PT_Edit describing the change
 *    in a form suitable for ts_tree_edit().  Pass NULL to skip.
 *  - Each public mutating call produces exactly one undo entry, including
 *    pt_set_line and pt_replace_range (which are NOT delete-then-insert
 *    at the API level).
 *  - The PieceTable struct is opaque; use the accessor functions.
 *  - Not thread-safe; one PieceTable per thread, or synchronize externally.
 * ========================================================================= */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 *  Export macro
 *
 *  - Define PT_BUILD_SHARED when compiling the library as a shared object/DLL
 *    so its public symbols are exported.
 *  - Define PT_USE_SHARED in clients that link against the shared library on
 *    Windows so the symbols are imported from the DLL.
 *  - With neither defined (the default), PT_API expands to nothing and the
 *    library may be built as a static archive or compiled directly into the
 *    consumer.  This is the configuration the supplied Makefile uses.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(PT_BUILD_SHARED)
#    define PT_API __declspec(dllexport)
#  elif defined(PT_USE_SHARED)
#    define PT_API __declspec(dllimport)
#  else
#    define PT_API
#  endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#  if defined(PT_BUILD_SHARED)
#    define PT_API __attribute__((visibility("default")))
#  else
#    define PT_API
#  endif
#else
#  define PT_API
#endif

typedef struct PieceTable PieceTable;

/* ---- Line-ending mode ---------------------------------------------------- */
typedef enum {
    PT_EOL_UNIX,       /* \n   */
    PT_EOL_WINDOWS,    /* \r\n */
    PT_EOL_MAC,        /* \r   */
    PT_EOL_PRESERVE    /* keep whatever was detected on load          */
} PT_EolMode;

/* ---- (line, column) point ----------------------------------------------- */
typedef struct {
    uint32_t line;
    uint32_t column;       /* byte column within the line */
} PT_Point;

/* ---- Edit descriptor (mirrors the inputs to ts_tree_edit) --------------- */
typedef struct {
    size_t   start_byte;
    size_t   old_end_byte;
    size_t   new_end_byte;
    PT_Point start_point;
    PT_Point old_end_point;
    PT_Point new_end_point;
} PT_Edit;

/* ==========================================================================
 *  Construction / destruction
 * ========================================================================= */

PT_API PieceTable* pt_new            (void);
PT_API PieceTable* pt_new_from_string(const char* text, size_t len);
PT_API PieceTable* pt_new_from_file  (const char* filename);
PT_API void        pt_free           (PieceTable* pt);

PT_API bool        pt_save_file      (PieceTable* pt, const char* filename, PT_EolMode eol);

/* ==========================================================================
 *  Document state
 * ========================================================================= */

PT_API size_t      pt_line_count    (const PieceTable* pt);
PT_API size_t      pt_byte_length   (const PieceTable* pt);
PT_API PT_EolMode  pt_detected_eol  (const PieceTable* pt);

PT_API bool        pt_is_modified   (const PieceTable* pt);
PT_API void        pt_clear_modified(PieceTable* pt);

/* ==========================================================================
 *  Position conversion (O(log N) on line count)
 * ========================================================================= */

/* Byte offset at the start of `line`.  `line == pt_line_count(pt)` returns
 * pt_byte_length(pt).  Returns SIZE_MAX if `line` is otherwise out of range. */
PT_API size_t   pt_line_to_byte  (const PieceTable* pt, size_t line);

/* Inverse of pt_line_to_byte.  byte_offset is clamped to [0, byte_length]. */
PT_API PT_Point pt_byte_to_point (const PieceTable* pt, size_t byte_offset);

/* (line, col) -> byte. col is clamped to the line's byte length. */
PT_API size_t   pt_point_to_byte (const PieceTable* pt, PT_Point point);

/* Length of `line` in bytes including any trailing newline.  0 if oor. */
PT_API size_t   pt_line_length   (const PieceTable* pt, size_t line);

/* ==========================================================================
 *  Read access
 * ========================================================================= */

/* Copy a single line (with its trailing '\n' if any) into `out`.  Returns
 * the number of bytes that *would* be written if `out_cap` were large
 * enough.  Always NUL-terminates if out_cap > 0. */
PT_API size_t pt_get_line       (const PieceTable* pt, size_t line,
                                 char* out, size_t out_cap);

/* Allocate & return a malloc'd, NUL-terminated copy of `line` (with its
 * trailing '\n' if any).  Caller frees.  NULL on oor / OOM. */
PT_API char*  pt_dup_line       (const PieceTable* pt, size_t line);

/* Range read by byte offsets.  start/end are clamped to [0, byte_length],
 * end is clamped to be >= start. */
PT_API size_t pt_get_byte_range (const PieceTable* pt,
                                 size_t start_byte, size_t end_byte,
                                 char* out, size_t out_cap);

PT_API char*  pt_dup_byte_range (const PieceTable* pt,
                                 size_t start_byte, size_t end_byte,
                                 size_t* out_len);

/* Range read by (line, col) points. */
PT_API char*  pt_dup_range      (const PieceTable* pt,
                                 PT_Point start, PT_Point end,
                                 size_t* out_len);

/* tree-sitter `TSInput.read`-compatible byte reader.  Reads up to
 * `max_bytes` from `byte_offset` into `out`.  Returns the number of bytes
 * actually read (0 at EOF).  Output is NOT NUL-terminated. */
PT_API size_t pt_read           (const PieceTable* pt, size_t byte_offset,
                                 void* out, size_t max_bytes);

/* ==========================================================================
 *  Line-oriented edits  (the common case)
 *
 *  All of these append a trailing '\n' to `text` automatically, so callers
 *  pass line content without the newline.
 * ========================================================================= */

PT_API bool pt_insert_line  (PieceTable* pt, size_t line,
                             const char* text, size_t text_len, PT_Edit* edit);

PT_API bool pt_append_line  (PieceTable* pt,
                             const char* text, size_t text_len, PT_Edit* edit);

PT_API bool pt_set_line     (PieceTable* pt, size_t line,
                             const char* text, size_t text_len, PT_Edit* edit);

PT_API bool pt_delete_line  (PieceTable* pt, size_t line, PT_Edit* edit);

/* ==========================================================================
 *  Range / point edits  (cut / copy / paste, multi-line edits)
 *
 *  These do NOT auto-append newlines; the caller's text is used verbatim.
 * ========================================================================= */

PT_API bool pt_insert_at_byte     (PieceTable* pt, size_t byte_offset,
                                   const char* text, size_t text_len, PT_Edit* edit);

PT_API bool pt_insert_at_point    (PieceTable* pt, PT_Point at,
                                   const char* text, size_t text_len, PT_Edit* edit);

PT_API bool pt_delete_byte_range  (PieceTable* pt, size_t start_byte, size_t end_byte,
                                   PT_Edit* edit);

PT_API bool pt_delete_range       (PieceTable* pt, PT_Point start, PT_Point end,
                                   PT_Edit* edit);

PT_API bool pt_replace_byte_range (PieceTable* pt, size_t start_byte, size_t end_byte,
                                   const char* text, size_t text_len, PT_Edit* edit);

PT_API bool pt_replace_range      (PieceTable* pt, PT_Point start, PT_Point end,
                                   const char* text, size_t text_len, PT_Edit* edit);

/* ==========================================================================
 *  Undo / redo
 *
 *  Each public mutating call above creates one undo entry.  pt_undo /
 *  pt_redo report the resulting edit in `*edit` so callers can pass it on
 *  to tree-sitter.
 * ========================================================================= */

PT_API bool pt_undo          (PieceTable* pt, PT_Edit* edit);
PT_API bool pt_redo          (PieceTable* pt, PT_Edit* edit);
PT_API bool pt_can_undo      (const PieceTable* pt);
PT_API bool pt_can_redo      (const PieceTable* pt);
PT_API void pt_clear_history (PieceTable* pt);

#ifdef __cplusplus
}
#endif
