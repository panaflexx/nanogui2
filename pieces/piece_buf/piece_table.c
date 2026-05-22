/* ==========================================================================
 *  piece_table.c
 *
 *  See piece_table.h for the public API and conventions.
 *
 *  Implementation overview
 *  -----------------------
 *  Storage is a classic piece table.  Two byte buffers exist:
 *    - `original`   : immutable; the (normalized) file contents on load
 *    - `add_buffer` : append-only; new bytes from every edit
 *  The document is a sequence of `Piece` records, each pointing into one
 *  of the buffers.  A `line_offsets` array gives the byte offset of the
 *  start of every line, kept in sync incrementally with every edit.
 *
 *  Every mutating public function routes through one internal primitive,
 *  `replace_bytes()`, which performs:
 *      1. Piece-table surgery (split/erase/insert).
 *      2. Incremental line-index update.
 *      3. doc_length bookkeeping & dirty-flag.
 *      4. Optional PT_Edit population.
 *  Line-based helpers (insert/append/set/delete-line) just translate to
 *  the appropriate byte range and call replace_bytes().
 *
 *  The library itself uses only ISO C11 -- no POSIX, no platform headers.
 * ========================================================================= */
#include "piece_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ==========================================================================
 *  Internal types
 * ========================================================================= */

typedef struct {
    bool   is_original;
    size_t start;          /* offset into original or add_buffer */
    size_t length;
} Piece;

typedef struct {
    Piece*  pieces;
    size_t  piece_count;
    size_t  add_len;       /* snapshot of add_buffer length at the time */
} Snapshot;

struct PieceTable {
    char*       original;
    size_t      original_len;

    char*       add_buffer;
    size_t      add_len;
    size_t      add_capacity;

    Piece*      pieces;
    size_t      piece_count;
    size_t      piece_capacity;

    size_t*     line_offsets;
    size_t      line_count;
    size_t      line_capacity;

    size_t      doc_length;

    PT_EolMode  detected_eol;
    bool        modified;

    Snapshot*   undo_stack;
    size_t      undo_count;
    size_t      undo_capacity;
    Snapshot*   redo_stack;
    size_t      redo_count;
    size_t      redo_capacity;
};

/* ==========================================================================
 *  Forward decls
 * ========================================================================= */

static void   grow_pieces      (PieceTable* pt, size_t min_needed);
static void   grow_add_buffer  (PieceTable* pt, size_t needed);
static void   grow_line_offsets(PieceTable* pt, size_t min_needed);
static void   grow_undo        (PieceTable* pt);
static void   grow_redo        (PieceTable* pt);
static void   save_undo_state  (PieceTable* pt);
static void   clear_redo       (PieceTable* pt);
static void   restore_snapshot (PieceTable* pt, Snapshot* snap);

static void   rebuild_line_index(PieceTable* pt);
static size_t find_line_for_byte(const PieceTable* pt, size_t byte);

static bool   replace_bytes(PieceTable* pt,
                            size_t start, size_t end,
                            const char* text, size_t text_len,
                            PT_Edit* edit);

/* ==========================================================================
 *  Small utilities
 * ========================================================================= */

static char* normalize_eol(const char* in, size_t in_len,
                           size_t* out_len, PT_EolMode* detected_out)
{
    /* Output is at most as long as the input. */
    char* out = (char*)malloc(in_len > 0 ? in_len : 1);
    if (!out) return NULL;

    PT_EolMode detected = PT_EOL_UNIX;
    bool       eol_seen = false;
    size_t     j = 0;

    for (size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '\r') {
            if (!eol_seen) {
                detected = (i + 1 < in_len && in[i + 1] == '\n')
                           ? PT_EOL_WINDOWS : PT_EOL_MAC;
                eol_seen = true;
            }
            out[j++] = '\n';
            if (i + 1 < in_len && in[i + 1] == '\n') i++;
        } else if (c == '\n') {
            if (!eol_seen) { detected = PT_EOL_UNIX; eol_seen = true; }
            out[j++] = '\n';
        } else {
            out[j++] = c;
        }
    }
    *out_len = j;
    *detected_out = detected;
    return out;
}

/* ==========================================================================
 *  Growth helpers (allocations are fatal on failure)
 * ========================================================================= */

static void grow_pieces(PieceTable* pt, size_t min_needed) {
    if (min_needed <= pt->piece_capacity) return;
    size_t nc = pt->piece_capacity ? pt->piece_capacity * 2 : 128;
    while (nc < min_needed) nc *= 2;
    Piece* np = (Piece*)realloc(pt->pieces, nc * sizeof(Piece));
    if (!np) { fprintf(stderr, "FATAL: OOM in grow_pieces\n"); abort(); }
    pt->pieces = np;
    pt->piece_capacity = nc;
}

static void grow_add_buffer(PieceTable* pt, size_t needed) {
    if (pt->add_len + needed <= pt->add_capacity) return;
    size_t nc = pt->add_capacity ? pt->add_capacity * 2 : 8192;
    while (nc < pt->add_len + needed) nc *= 2;
    char* nb = (char*)realloc(pt->add_buffer, nc);
    if (!nb) { fprintf(stderr, "FATAL: OOM in grow_add_buffer\n"); abort(); }
    pt->add_buffer = nb;
    pt->add_capacity = nc;
}

static void grow_line_offsets(PieceTable* pt, size_t min_needed) {
    if (min_needed <= pt->line_capacity) return;
    size_t nc = pt->line_capacity ? pt->line_capacity * 2 : 1024;
    while (nc < min_needed) nc *= 2;
    size_t* nb = (size_t*)realloc(pt->line_offsets, nc * sizeof(size_t));
    if (!nb) { fprintf(stderr, "FATAL: OOM in grow_line_offsets\n"); abort(); }
    pt->line_offsets = nb;
    pt->line_capacity = nc;
}

static void grow_undo(PieceTable* pt) {
    if (pt->undo_count + 1 <= pt->undo_capacity) return;
    size_t nc = pt->undo_capacity ? pt->undo_capacity * 2 : 32;
    Snapshot* ns = (Snapshot*)realloc(pt->undo_stack, nc * sizeof(Snapshot));
    if (!ns) { fprintf(stderr, "FATAL: OOM in grow_undo\n"); abort(); }
    pt->undo_stack = ns;
    pt->undo_capacity = nc;
}

static void grow_redo(PieceTable* pt) {
    if (pt->redo_count + 1 <= pt->redo_capacity) return;
    size_t nc = pt->redo_capacity ? pt->redo_capacity * 2 : 32;
    Snapshot* ns = (Snapshot*)realloc(pt->redo_stack, nc * sizeof(Snapshot));
    if (!ns) { fprintf(stderr, "FATAL: OOM in grow_redo\n"); abort(); }
    pt->redo_stack = ns;
    pt->redo_capacity = nc;
}

/* ==========================================================================
 *  Piece primitives
 * ========================================================================= */

static size_t find_piece_at_pos(const PieceTable* pt, size_t pos, size_t* off) {
    /* Linear walk.  In practice piece_count stays modest for line-based
     * editing; this could be replaced with a balanced tree if needed. */
    size_t cur = 0;
    for (size_t i = 0; i < pt->piece_count; ++i) {
        const Piece* p = &pt->pieces[i];
        if (pos < cur + p->length) {
            *off = pos - cur;
            return i;
        }
        cur += p->length;
    }
    *off = 0;
    return pt->piece_count;
}

static void split_piece(PieceTable* pt, size_t idx, size_t split_off) {
    if (split_off == 0 || split_off >= pt->pieces[idx].length) return;
    grow_pieces(pt, pt->piece_count + 1);
    memmove(&pt->pieces[idx + 1], &pt->pieces[idx],
            (pt->piece_count - idx) * sizeof(Piece));
    Piece* p = &pt->pieces[idx];
    Piece right = { p->is_original, p->start + split_off, p->length - split_off };
    p->length = split_off;
    pt->pieces[idx + 1] = right;
    pt->piece_count++;
}

/* ==========================================================================
 *  Line index
 * ========================================================================= */

/* Largest i with line_offsets[i] <= byte. */
static size_t find_line_for_byte(const PieceTable* pt, size_t byte) {
    if (pt->line_count == 0) return 0;
    size_t lo = 0, hi = pt->line_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pt->line_offsets[mid] <= byte) lo = mid + 1;
        else                                hi = mid;
    }
    /* lo is one past the last entry with value <= byte. */
    return (lo == 0) ? 0 : (lo - 1);
}

static void rebuild_line_index(PieceTable* pt) {
    free(pt->line_offsets);
    pt->line_offsets = NULL;
    pt->line_count = 0;
    pt->line_capacity = 0;
    pt->doc_length = 0;

    for (size_t i = 0; i < pt->piece_count; ++i)
        pt->doc_length += pt->pieces[i].length;

    grow_line_offsets(pt, 1);
    pt->line_offsets[0] = 0;
    pt->line_count = 1;
    if (pt->piece_count == 0) return;

    size_t pos = 0;
    for (size_t i = 0; i < pt->piece_count; ++i) {
        const Piece* p = &pt->pieces[i];
        const char* src = p->is_original ? pt->original : pt->add_buffer;
        for (size_t j = 0; j < p->length; ++j) {
            if (src[p->start + j] == '\n') {
                grow_line_offsets(pt, pt->line_count + 1);
                pt->line_offsets[pt->line_count++] = pos + j + 1;
            }
        }
        pos += p->length;
    }
}

/* Update line_offsets for an arbitrary byte-range replace.
 *
 *  - Entries with line_offsets[i] in (old_start, old_end] are removed
 *    (their preceding '\n' was inside the removed range).
 *  - Entries with line_offsets[i] > old_end are shifted by (new_len - old_len).
 *  - For each '\n' at offset k in the inserted bytes, one new entry is
 *    inserted at value (old_start + k + 1).
 */
static void update_line_index(PieceTable* pt,
                              size_t old_start, size_t old_end,
                              const char* text, size_t new_len)
{
    size_t old_len = old_end - old_start;

    /* Locate slice [first_removed, last_removed_excl) to delete. */
    size_t first_removed, last_removed_excl;
    {
        /* upper_bound(old_start) */
        size_t lo = 0, hi = pt->line_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (pt->line_offsets[mid] <= old_start) lo = mid + 1;
            else                                     hi = mid;
        }
        first_removed = lo;
        /* upper_bound(old_end) starting from first_removed */
        lo = first_removed; hi = pt->line_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (pt->line_offsets[mid] <= old_end) lo = mid + 1;
            else                                   hi = mid;
        }
        last_removed_excl = lo;
    }
    size_t removed = last_removed_excl - first_removed;

    /* Count '\n' in the inserted bytes. */
    size_t added = 0;
    for (size_t i = 0; i < new_len; ++i) if (text[i] == '\n') added++;

    size_t new_total = pt->line_count - removed + added;
    grow_line_offsets(pt, new_total);

    /* Shift the tail (entries after the removed slice). */
    size_t tail_dst = first_removed + added;
    size_t tail_src = last_removed_excl;
    size_t tail_len = pt->line_count - last_removed_excl;
    if (tail_len > 0 && tail_dst != tail_src) {
        memmove(&pt->line_offsets[tail_dst],
                &pt->line_offsets[tail_src],
                tail_len * sizeof(size_t));
    }

    /* Adjust tail entries by net byte delta. */
    if (new_len >= old_len) {
        size_t delta = new_len - old_len;
        if (delta > 0) {
            for (size_t i = tail_dst; i < tail_dst + tail_len; ++i)
                pt->line_offsets[i] += delta;
        }
    } else {
        size_t delta = old_len - new_len;
        for (size_t i = tail_dst; i < tail_dst + tail_len; ++i)
            pt->line_offsets[i] -= delta;
    }

    /* Fill in new entries scanning the inserted bytes. */
    size_t fill = first_removed;
    for (size_t i = 0; i < new_len && fill < first_removed + added; ++i) {
        if (text[i] == '\n')
            pt->line_offsets[fill++] = old_start + i + 1;
    }

    pt->line_count = new_total;
    pt->doc_length = pt->doc_length + new_len - old_len;
}

/* ==========================================================================
 *  The core mutating primitive
 * ========================================================================= */

static bool replace_bytes(PieceTable* pt,
                          size_t start, size_t end,
                          const char* text, size_t text_len,
                          PT_Edit* edit)
{
    if (!pt) return false;
    if (start > pt->doc_length) start = pt->doc_length;
    if (end   > pt->doc_length) end   = pt->doc_length;
    if (end   < start)          end   = start;

    /* No-op? */
    if (start == end && text_len == 0) {
        if (edit) {
            PT_Point p = pt_byte_to_point(pt, start);
            edit->start_byte = edit->old_end_byte = edit->new_end_byte = start;
            edit->start_point = edit->old_end_point = edit->new_end_point = p;
        }
        return true;
    }

    /* Capture pre-edit point info. */
    PT_Point start_pt   = pt_byte_to_point(pt, start);
    PT_Point old_end_pt = pt_byte_to_point(pt, end);

    save_undo_state(pt);

    /* --- Piece-table surgery --------------------------------------------- */
    size_t start_off = 0;
    size_t start_pc  = find_piece_at_pos(pt, start, &start_off);
    if (start_off > 0) {
        split_piece(pt, start_pc, start_off);
        start_pc++;
    }
    size_t end_off = 0;
    size_t end_pc  = find_piece_at_pos(pt, end, &end_off);
    if (end_off > 0) {
        split_piece(pt, end_pc, end_off);
        end_pc++;
    }

    /* Remove pieces [start_pc, end_pc). */
    size_t remove_count = end_pc - start_pc;
    if (remove_count > 0) {
        memmove(&pt->pieces[start_pc],
                &pt->pieces[end_pc],
                (pt->piece_count - end_pc) * sizeof(Piece));
        pt->piece_count -= remove_count;
    }

    /* Insert a fresh piece for the new text. */
    if (text_len > 0) {
        grow_add_buffer(pt, text_len);
        memcpy(pt->add_buffer + pt->add_len, text, text_len);

        grow_pieces(pt, pt->piece_count + 1);
        memmove(&pt->pieces[start_pc + 1], &pt->pieces[start_pc],
                (pt->piece_count - start_pc) * sizeof(Piece));
        pt->pieces[start_pc] = (Piece){ false, pt->add_len, text_len };
        pt->piece_count++;
        pt->add_len += text_len;
    }

    /* --- Line index ------------------------------------------------------ */
    update_line_index(pt, start, end, text, text_len);

    /* --- Bookkeeping ----------------------------------------------------- */
    pt->modified = true;

    if (edit) {
        edit->start_byte    = start;
        edit->old_end_byte  = end;
        edit->new_end_byte  = start + text_len;
        edit->start_point   = start_pt;
        edit->old_end_point = old_end_pt;
        edit->new_end_point = pt_byte_to_point(pt, edit->new_end_byte);
    }
    return true;
}

/* ==========================================================================
 *  Construction / destruction
 * ========================================================================= */

static PieceTable* pt_alloc_blank(void) {
    PieceTable* pt = (PieceTable*)calloc(1, sizeof(PieceTable));
    if (!pt) return NULL;
    pt->detected_eol = PT_EOL_UNIX;
    rebuild_line_index(pt);   /* sets line_offsets = [0], line_count = 1 */
    return pt;
}

PieceTable* pt_new(void) {
    return pt_alloc_blank();
}

PieceTable* pt_new_from_string(const char* text, size_t len) {
    PieceTable* pt = pt_alloc_blank();
    if (!pt) return NULL;
    if (!text || len == 0) return pt;

    size_t norm_len = 0;
    PT_EolMode detected = PT_EOL_UNIX;
    char* norm = normalize_eol(text, len, &norm_len, &detected);
    if (!norm) { pt_free(pt); return NULL; }

    pt->original     = norm;
    pt->original_len = norm_len;
    pt->detected_eol = detected;

    grow_pieces(pt, 1);
    pt->pieces[0] = (Piece){ true, 0, norm_len };
    pt->piece_count = 1;

    rebuild_line_index(pt);
    pt->modified = false;
    return pt;
}

PieceTable* pt_new_from_file(const char* filename) {
    if (!filename) return NULL;
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);

    char* raw = (char*)malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (!raw) { fclose(f); return NULL; }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(raw); return NULL; }

    PieceTable* pt = pt_new_from_string(raw, (size_t)sz);
    free(raw);
    return pt;
}

void pt_free(PieceTable* pt) {
    if (!pt) return;
    pt_clear_history(pt);
    free(pt->original);
    free(pt->add_buffer);
    free(pt->pieces);
    free(pt->line_offsets);
    free(pt);
}

bool pt_save_file(PieceTable* pt, const char* filename, PT_EolMode eol) {
    if (!pt || !filename) return false;
    if (eol == PT_EOL_PRESERVE) eol = pt->detected_eol;

    FILE* f = fopen(filename, "wb");
    if (!f) return false;

    const char* sep_unix = "\n";
    const char* sep_win  = "\r\n";
    const char* sep_mac  = "\r";
    const char* sep      = sep_unix;
    size_t      sep_len  = 1;
    if (eol == PT_EOL_WINDOWS) { sep = sep_win; sep_len = 2; }
    else if (eol == PT_EOL_MAC){ sep = sep_mac; sep_len = 1; }

    for (size_t i = 0; i < pt->piece_count; ++i) {
        const Piece* p = &pt->pieces[i];
        const char*  src = p->is_original ? pt->original : pt->add_buffer;
        for (size_t j = 0; j < p->length; ++j) {
            char c = src[p->start + j];
            if (c == '\n') {
                if (fwrite(sep, 1, sep_len, f) != sep_len) { fclose(f); return false; }
            } else {
                if (fputc((unsigned char)c, f) == EOF) { fclose(f); return false; }
            }
        }
    }
    if (fclose(f) != 0) return false;
    pt->modified = false;
    return true;
}

/* ==========================================================================
 *  Document state accessors
 * ========================================================================= */

size_t      pt_line_count    (const PieceTable* pt) { return pt ? pt->line_count : 0; }
size_t      pt_byte_length   (const PieceTable* pt) { return pt ? pt->doc_length : 0; }
PT_EolMode  pt_detected_eol  (const PieceTable* pt) { return pt ? pt->detected_eol : PT_EOL_UNIX; }
bool        pt_is_modified   (const PieceTable* pt) { return pt && pt->modified; }
void        pt_clear_modified(PieceTable* pt)       { if (pt) pt->modified = false; }

/* ==========================================================================
 *  Position conversion
 * ========================================================================= */

size_t pt_line_to_byte(const PieceTable* pt, size_t line) {
    if (!pt) return SIZE_MAX;
    if (line == pt->line_count) return pt->doc_length;
    if (line >  pt->line_count) return SIZE_MAX;
    return pt->line_offsets[line];
}

PT_Point pt_byte_to_point(const PieceTable* pt, size_t byte_offset) {
    PT_Point p = { 0, 0 };
    if (!pt || pt->line_count == 0) return p;
    if (byte_offset > pt->doc_length) byte_offset = pt->doc_length;
    size_t line = find_line_for_byte(pt, byte_offset);
    p.line   = (uint32_t)line;
    p.column = (uint32_t)(byte_offset - pt->line_offsets[line]);
    return p;
}

size_t pt_point_to_byte(const PieceTable* pt, PT_Point point) {
    if (!pt || pt->line_count == 0) return 0;
    if (point.line >= pt->line_count) return pt->doc_length;
    size_t line_start = pt->line_offsets[point.line];
    size_t line_end   = (point.line + 1 < pt->line_count)
                        ? pt->line_offsets[point.line + 1]
                        : pt->doc_length;
    size_t byte = line_start + point.column;
    if (byte > line_end) byte = line_end;
    return byte;
}

size_t pt_line_length(const PieceTable* pt, size_t line) {
    if (!pt || line >= pt->line_count) return 0;
    size_t start = pt->line_offsets[line];
    size_t end   = (line + 1 < pt->line_count) ? pt->line_offsets[line + 1]
                                                : pt->doc_length;
    return end - start;
}

/* ==========================================================================
 *  Read access (all built on top of pt_read)
 * ========================================================================= */

size_t pt_read(const PieceTable* pt, size_t byte_offset,
               void* out, size_t max_bytes)
{
    if (!pt || !out || max_bytes == 0) return 0;
    if (byte_offset >= pt->doc_length) return 0;

    size_t remaining = pt->doc_length - byte_offset;
    if (max_bytes > remaining) max_bytes = remaining;

    char*  dst = (char*)out;
    size_t written = 0;

    /* Find the piece containing byte_offset, then walk forward. */
    size_t off_in_piece = 0;
    size_t pi = find_piece_at_pos(pt, byte_offset, &off_in_piece);

    while (pi < pt->piece_count && written < max_bytes) {
        const Piece* p   = &pt->pieces[pi];
        const char*  src = p->is_original ? pt->original : pt->add_buffer;
        size_t avail = p->length - off_in_piece;
        size_t want  = max_bytes - written;
        size_t take  = avail < want ? avail : want;
        memcpy(dst + written, src + p->start + off_in_piece, take);
        written += take;
        off_in_piece = 0;
        pi++;
    }
    return written;
}

size_t pt_get_byte_range(const PieceTable* pt,
                         size_t start_byte, size_t end_byte,
                         char* out, size_t out_cap)
{
    if (!pt) return 0;
    if (start_byte > pt->doc_length) start_byte = pt->doc_length;
    if (end_byte   > pt->doc_length) end_byte   = pt->doc_length;
    if (end_byte   < start_byte)     end_byte   = start_byte;
    size_t needed = end_byte - start_byte;

    if (out && out_cap > 0) {
        size_t to_copy = needed < out_cap - 1 ? needed : out_cap - 1;
        pt_read(pt, start_byte, out, to_copy);
        out[to_copy] = '\0';
    }
    return needed;
}

char* pt_dup_byte_range(const PieceTable* pt,
                        size_t start_byte, size_t end_byte,
                        size_t* out_len)
{
    if (!pt) { if (out_len) *out_len = 0; return NULL; }
    if (start_byte > pt->doc_length) start_byte = pt->doc_length;
    if (end_byte   > pt->doc_length) end_byte   = pt->doc_length;
    if (end_byte   < start_byte)     end_byte   = start_byte;
    size_t n = end_byte - start_byte;

    char* buf = (char*)malloc(n + 1);
    if (!buf) { if (out_len) *out_len = 0; return NULL; }
    if (n > 0) pt_read(pt, start_byte, buf, n);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

char* pt_dup_range(const PieceTable* pt, PT_Point start, PT_Point end,
                   size_t* out_len)
{
    if (!pt) { if (out_len) *out_len = 0; return NULL; }
    size_t a = pt_point_to_byte(pt, start);
    size_t b = pt_point_to_byte(pt, end);
    if (b < a) { size_t t = a; a = b; b = t; }
    return pt_dup_byte_range(pt, a, b, out_len);
}

size_t pt_get_line(const PieceTable* pt, size_t line,
                   char* out, size_t out_cap)
{
    if (!pt || line >= pt->line_count) {
        if (out && out_cap > 0) out[0] = '\0';
        return 0;
    }
    size_t start = pt->line_offsets[line];
    size_t end   = (line + 1 < pt->line_count) ? pt->line_offsets[line + 1]
                                                : pt->doc_length;
    return pt_get_byte_range(pt, start, end, out, out_cap);
}

char* pt_dup_line(const PieceTable* pt, size_t line) {
    if (!pt || line >= pt->line_count) return NULL;
    size_t start = pt->line_offsets[line];
    size_t end   = (line + 1 < pt->line_count) ? pt->line_offsets[line + 1]
                                                : pt->doc_length;
    return pt_dup_byte_range(pt, start, end, NULL);
}

/* ==========================================================================
 *  Line-oriented edits
 * ========================================================================= */

/* Helper: build [text + '\n'] in a stack/heap buffer and call replace_bytes. */
static bool replace_with_text_plus_nl(PieceTable* pt,
                                      size_t start, size_t end,
                                      const char* text, size_t text_len,
                                      PT_Edit* edit)
{
    char stack_buf[1024];
    char* buf = stack_buf;
    size_t total = text_len + 1;
    if (total > sizeof(stack_buf)) {
        buf = (char*)malloc(total);
        if (!buf) return false;
    }
    if (text_len > 0) memcpy(buf, text, text_len);
    buf[text_len] = '\n';

    bool ok = replace_bytes(pt, start, end, buf, total, edit);
    if (buf != stack_buf) free(buf);
    return ok;
}

bool pt_insert_line(PieceTable* pt, size_t line,
                    const char* text, size_t text_len, PT_Edit* edit)
{
    if (!pt) return false;
    if (line > pt->line_count) line = pt->line_count;
    size_t at = (line < pt->line_count) ? pt->line_offsets[line] : pt->doc_length;
    return replace_with_text_plus_nl(pt, at, at, text ? text : "", text_len, edit);
}

bool pt_append_line(PieceTable* pt, const char* text, size_t text_len, PT_Edit* edit)
{
    if (!pt) return false;
    return replace_with_text_plus_nl(pt, pt->doc_length, pt->doc_length,
                                     text ? text : "", text_len, edit);
}

bool pt_set_line(PieceTable* pt, size_t line,
                 const char* text, size_t text_len, PT_Edit* edit)
{
    if (!pt || line >= pt->line_count) return false;
    size_t start = pt->line_offsets[line];
    size_t end   = (line + 1 < pt->line_count) ? pt->line_offsets[line + 1]
                                                : pt->doc_length;
    return replace_with_text_plus_nl(pt, start, end, text ? text : "", text_len, edit);
}

bool pt_delete_line(PieceTable* pt, size_t line, PT_Edit* edit) {
    if (!pt || line >= pt->line_count) return false;
    size_t start = pt->line_offsets[line];
    size_t end   = (line + 1 < pt->line_count) ? pt->line_offsets[line + 1]
                                                : pt->doc_length;
    return replace_bytes(pt, start, end, NULL, 0, edit);
}

/* ==========================================================================
 *  Range / point edits
 * ========================================================================= */

bool pt_insert_at_byte(PieceTable* pt, size_t byte_offset,
                       const char* text, size_t text_len, PT_Edit* edit)
{
    return replace_bytes(pt, byte_offset, byte_offset,
                         text ? text : "", text_len, edit);
}

bool pt_insert_at_point(PieceTable* pt, PT_Point at,
                        const char* text, size_t text_len, PT_Edit* edit)
{
    if (!pt) return false;
    size_t b = pt_point_to_byte(pt, at);
    return pt_insert_at_byte(pt, b, text, text_len, edit);
}

bool pt_delete_byte_range(PieceTable* pt, size_t start, size_t end, PT_Edit* edit) {
    return replace_bytes(pt, start, end, NULL, 0, edit);
}

bool pt_delete_range(PieceTable* pt, PT_Point start, PT_Point end, PT_Edit* edit) {
    if (!pt) return false;
    size_t a = pt_point_to_byte(pt, start);
    size_t b = pt_point_to_byte(pt, end);
    if (b < a) { size_t t = a; a = b; b = t; }
    return replace_bytes(pt, a, b, NULL, 0, edit);
}

bool pt_replace_byte_range(PieceTable* pt, size_t start, size_t end,
                           const char* text, size_t text_len, PT_Edit* edit)
{
    return replace_bytes(pt, start, end, text ? text : "", text_len, edit);
}

bool pt_replace_range(PieceTable* pt, PT_Point start, PT_Point end,
                      const char* text, size_t text_len, PT_Edit* edit)
{
    if (!pt) return false;
    size_t a = pt_point_to_byte(pt, start);
    size_t b = pt_point_to_byte(pt, end);
    if (b < a) { size_t t = a; a = b; b = t; }
    return replace_bytes(pt, a, b, text ? text : "", text_len, edit);
}

/* ==========================================================================
 *  Undo / redo
 * ========================================================================= */

static Snapshot make_snapshot(const PieceTable* pt) {
    Snapshot s = { NULL, pt->piece_count, pt->add_len };
    if (pt->piece_count > 0) {
        s.pieces = (Piece*)malloc(pt->piece_count * sizeof(Piece));
        if (!s.pieces) { fprintf(stderr, "FATAL: OOM in make_snapshot\n"); abort(); }
        memcpy(s.pieces, pt->pieces, pt->piece_count * sizeof(Piece));
    }
    return s;
}

static void restore_snapshot(PieceTable* pt, Snapshot* snap) {
    /* Resize pieces array and copy. */
    grow_pieces(pt, snap->piece_count > 0 ? snap->piece_count : 1);
    if (snap->piece_count > 0)
        memcpy(pt->pieces, snap->pieces, snap->piece_count * sizeof(Piece));
    pt->piece_count = snap->piece_count;
    pt->add_len     = snap->add_len;

    /* Rebuild the line index (and doc_length) from the restored pieces. */
    rebuild_line_index(pt);
}

static void clear_redo(PieceTable* pt) {
    for (size_t i = 0; i < pt->redo_count; ++i) free(pt->redo_stack[i].pieces);
    pt->redo_count = 0;
}

static void save_undo_state(PieceTable* pt) {
    grow_undo(pt);
    pt->undo_stack[pt->undo_count++] = make_snapshot(pt);
    clear_redo(pt);
}

bool pt_can_undo(const PieceTable* pt) { return pt && pt->undo_count > 0; }
bool pt_can_redo(const PieceTable* pt) { return pt && pt->redo_count > 0; }

bool pt_undo(PieceTable* pt, PT_Edit* edit) {
    if (!pt_can_undo(pt)) return false;

    /* Save current state to redo. */
    grow_redo(pt);
    pt->redo_stack[pt->redo_count++] = make_snapshot(pt);

    /* For PT_Edit we describe the change AFTER undo by comparing with the
     * current (about-to-be-discarded) state. */
    size_t before_len = pt->doc_length;

    /* Pop and restore. */
    pt->undo_count--;
    Snapshot snap = pt->undo_stack[pt->undo_count];
    restore_snapshot(pt, &snap);
    free(snap.pieces);

    pt->modified = true;

    if (edit) {
        /* Coarse description: whole-document change.  Callers that need
         * minimal edits should track them at the call site.  For tree-sitter
         * usage the safe move is to reparse from scratch on undo. */
        edit->start_byte    = 0;
        edit->old_end_byte  = before_len;
        edit->new_end_byte  = pt->doc_length;
        edit->start_point   = (PT_Point){ 0, 0 };
        edit->old_end_point = (PT_Point){ 0, 0 };
        edit->new_end_point = pt_byte_to_point(pt, pt->doc_length);
    }
    return true;
}

bool pt_redo(PieceTable* pt, PT_Edit* edit) {
    if (!pt_can_redo(pt)) return false;

    grow_undo(pt);
    pt->undo_stack[pt->undo_count++] = make_snapshot(pt);

    size_t before_len = pt->doc_length;

    pt->redo_count--;
    Snapshot snap = pt->redo_stack[pt->redo_count];
    restore_snapshot(pt, &snap);
    free(snap.pieces);

    pt->modified = true;

    if (edit) {
        edit->start_byte    = 0;
        edit->old_end_byte  = before_len;
        edit->new_end_byte  = pt->doc_length;
        edit->start_point   = (PT_Point){ 0, 0 };
        edit->old_end_point = (PT_Point){ 0, 0 };
        edit->new_end_point = pt_byte_to_point(pt, pt->doc_length);
    }
    return true;
}

void pt_clear_history(PieceTable* pt) {
    if (!pt) return;
    for (size_t i = 0; i < pt->undo_count; ++i) free(pt->undo_stack[i].pieces);
    for (size_t i = 0; i < pt->redo_count; ++i) free(pt->redo_stack[i].pieces);
    free(pt->undo_stack);
    free(pt->redo_stack);
    pt->undo_stack = pt->redo_stack = NULL;
    pt->undo_count = pt->redo_count = 0;
    pt->undo_capacity = pt->redo_capacity = 0;
}
