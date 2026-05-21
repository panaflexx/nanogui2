// piece_table.c
#include "piece_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

static void grow_pieces(PieceTable* pt, size_t min_needed);
static void grow_add_buffer(PieceTable* pt, size_t needed);
static void save_undo_state(PieceTable* pt);

// ---------------------------------------------------------------------------
// Line index helpers
// ---------------------------------------------------------------------------

static void rebuild_line_index(PieceTable* pt) {
    free(pt->line_offsets);
    pt->line_offsets = NULL;
    pt->line_count = 0;

    if (pt->piece_count == 0) return;

    size_t capacity = 1024;
    pt->line_offsets = malloc(capacity * sizeof(size_t));
    if (!pt->line_offsets) return;

    pt->line_offsets[0] = 0;
    pt->line_count = 1;

    size_t pos = 0;
    for (size_t i = 0; i < pt->piece_count; ++i) {
        const Piece* p = &pt->pieces[i];
        const char* src = p->is_original ? pt->original : pt->add_buffer;
        for (size_t j = 0; j < p->length; ++j) {
            char c = src[p->start + j];
            if (c == '\n' || c == '\r') {
                if (pt->line_count >= capacity) {
                    capacity *= 2;
                    pt->line_offsets = realloc(pt->line_offsets, capacity * sizeof(size_t));
                    if (!pt->line_offsets) return;
                }
                pt->line_offsets[pt->line_count++] = pos + j + 1;
                // Skip \r\n
                if (c == '\r' && j + 1 < p->length && src[p->start + j + 1] == '\n') {
                    j++;
                }
            }
        }
        pos += p->length;
    }
}

static void insert_line_offsets(PieceTable* pt, size_t line_idx, size_t new_lines, size_t insert_byte_pos) {
    if (!pt->line_offsets || new_lines == 0) {
        rebuild_line_index(pt);
        return;
    }

    size_t old_count = pt->line_count;
    pt->line_count += new_lines;

    pt->line_offsets = realloc(pt->line_offsets, (pt->line_count + 64) * sizeof(size_t));
    if (!pt->line_offsets) {
        rebuild_line_index(pt);
        return;
    }

    // Make room for the new line offsets
    memmove(&pt->line_offsets[line_idx + new_lines],
            &pt->line_offsets[line_idx],
            (old_count - line_idx) * sizeof(size_t));

    // Insert the new line start positions by scanning only the inserted region
    size_t pos = insert_byte_pos;
    for (size_t i = 0; i < new_lines; ++i) {
        pt->line_offsets[line_idx + i] = pos;

        // Advance to the next line ending
        while (pos < pt->original_len + pt->add_len) {
            char c = (pos < pt->original_len) ? pt->original[pos]
                     : pt->add_buffer[pos - pt->original_len];
            pos++;
            if (c == '\n' || c == '\r') {
                if (c == '\r' && pos < pt->original_len + pt->add_len) {
                    char next = (pos < pt->original_len) ? pt->original[pos]
                                : pt->add_buffer[pos - pt->original_len];
                    if (next == '\n') pos++;
                }
                break;
            }
        }
    }

    // Shift all subsequent line offsets by the total bytes inserted
    size_t total_inserted = pos - insert_byte_pos;
    for (size_t i = line_idx + new_lines; i < pt->line_count; ++i) {
        pt->line_offsets[i] += total_inserted;
    }
}

static void delete_line_offsets(PieceTable* pt, size_t line_idx, size_t lines_removed) {
    if (!pt->line_offsets || lines_removed == 0 || line_idx >= pt->line_count) return;

    size_t start_pos = pt->line_offsets[line_idx];
    size_t end_pos = (line_idx + lines_removed < pt->line_count)
                     ? pt->line_offsets[line_idx + lines_removed]
                     : (pt->original_len + pt->add_len);

    size_t removed_bytes = end_pos - start_pos;

    // Shift offsets down
    memmove(&pt->line_offsets[line_idx],
            &pt->line_offsets[line_idx + lines_removed],
            (pt->line_count - line_idx - lines_removed) * sizeof(size_t));

    pt->line_count -= lines_removed;

    // Adjust remaining offsets
    for (size_t i = line_idx; i < pt->line_count; ++i) {
        pt->line_offsets[i] -= removed_bytes;
    }
}

// ---------------------------------------------------------------------------
// Piece navigation helpers
// ---------------------------------------------------------------------------

static size_t find_piece_at_pos(const PieceTable* pt, size_t byte_pos, size_t* offset_in_piece) {
    size_t current = 0;
    for (size_t i = 0; i < pt->piece_count; ++i) {
        const Piece* p = &pt->pieces[i];
        if (byte_pos < current + p->length) {
            *offset_in_piece = byte_pos - current;
            return i;
        }
        current += p->length;
    }
    *offset_in_piece = 0;
    return pt->piece_count;
}

static void split_piece(PieceTable* pt, size_t piece_idx, size_t split_offset) {
    if (split_offset == 0 || split_offset >= pt->pieces[piece_idx].length) return;

    grow_pieces(pt, pt->piece_count + 1);
    memmove(&pt->pieces[piece_idx + 1], &pt->pieces[piece_idx],
            (pt->piece_count - piece_idx) * sizeof(Piece));

    Piece* p = &pt->pieces[piece_idx];
    Piece right = {p->is_original, p->start + split_offset, p->length - split_offset};
    p->length = split_offset;
    pt->pieces[piece_idx + 1] = right;
    pt->piece_count++;
}

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

bool piece_table_load_file(PieceTable* pt, const char* filename) {
    if (!pt || !filename) return false;
    memset(pt, 0, sizeof(*pt));

    FILE* f = fopen(filename, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    pt->original = malloc(len + 1);
    if (!pt->original) { fclose(f); return false; }

    fread(pt->original, 1, len, f);
    pt->original[len] = '\0';
    pt->original_len = len;
    fclose(f);

    // Detect line ending
    pt->original_line_ending = LINE_ENDING_UNIX;
    for (size_t i = 0; i < pt->original_len; ++i) {
        if (pt->original[i] == '\r') {
            pt->original_line_ending = (i + 1 < pt->original_len && pt->original[i+1] == '\n')
                ? LINE_ENDING_WINDOWS : LINE_ENDING_MAC;
            break;
        }
    }
    pt->save_line_ending = LINE_ENDING_PRESERVE;

    pt->piece_capacity = 128;
    pt->pieces = malloc(pt->piece_capacity * sizeof(Piece));
    if (!pt->pieces) { piece_table_free(pt); return false; }
    pt->pieces[0] = (Piece){true, 0, pt->original_len};
    pt->piece_count = 1;

    rebuild_line_index(pt);
    return true;
}

bool piece_table_save_file(PieceTable* pt, const char* filename, LineEnding force_eol) {
    if (!pt || !filename) return false;
    LineEnding eol = (force_eol == LINE_ENDING_PRESERVE) ? pt->original_line_ending : force_eol;

    FILE* f = fopen(filename, "wb");
    if (!f) return false;

    for (size_t i = 0; i < pt->piece_count; ++i) {
        const Piece* p = &pt->pieces[i];
        const char* src = p->is_original ? pt->original : pt->add_buffer;
        for (size_t j = 0; j < p->length; ++j) {
            char c = src[p->start + j];
            if (c == '\n') {
                if (eol == LINE_ENDING_WINDOWS) fputc('\r', f);
                else if (eol == LINE_ENDING_MAC) fputc('\r', f);
                fputc('\n', f);
            } else if (c != '\r') {
                fputc(c, f);
            }
        }
    }
    fclose(f);
    return true;
}

bool piece_table_insert_line(PieceTable* pt, size_t line_idx, const char* text) {
    if (!pt || !text) return false;
    if (line_idx > pt->line_count) line_idx = pt->line_count;

    save_undo_state(pt);

    size_t insert_pos = (line_idx < pt->line_count) 
                        ? pt->line_offsets[line_idx] 
                        : (pt->original_len + pt->add_len);

    size_t new_lines = 1;
    for (const char* p = text; *p; ++p) if (*p == '\n') new_lines++;

    size_t text_len = strlen(text);
    grow_add_buffer(pt, text_len + 1);

    char* dst = pt->add_buffer + pt->add_len;
    memcpy(dst, text, text_len);
    dst[text_len] = '\n';                    // Always terminate the new line

    // Split piece if inserting in the middle of one
    size_t offset_in_piece;
    size_t piece_idx = find_piece_at_pos(pt, insert_pos, &offset_in_piece);
    if (piece_idx < pt->piece_count && offset_in_piece > 0) {
        split_piece(pt, piece_idx, offset_in_piece);
        piece_idx++;
    }

    grow_pieces(pt, pt->piece_count + 1);
    memmove(&pt->pieces[piece_idx + 1], &pt->pieces[piece_idx],
            (pt->piece_count - piece_idx) * sizeof(Piece));

    pt->pieces[piece_idx] = (Piece){false, pt->add_len, text_len + 1};
    pt->piece_count++;
    pt->add_len += text_len + 1;

    insert_line_offsets(pt, line_idx, new_lines, insert_pos);
    return true;
}

bool piece_table_delete_line(PieceTable* pt, size_t line_idx) {
    if (!pt || line_idx >= pt->line_count) return false;

    save_undo_state(pt);

    size_t start = pt->line_offsets[line_idx];
    size_t end   = (line_idx + 1 < pt->line_count)
                   ? pt->line_offsets[line_idx + 1]
                   : (pt->original_len + pt->add_len);

    if (start == end) { // empty line, nothing to do
        delete_line_offsets(pt, line_idx, 1);
        return true;
    }

    // === CORRECT BYTE-RANGE DELETE (this was the source of corruption) ===
    size_t start_offset;
    size_t start_piece = find_piece_at_pos(pt, start, &start_offset);

    // Split at start of deletion range
    if (start_offset > 0) {
        split_piece(pt, start_piece, start_offset);
        start_piece++;
    }

    // Split at end of deletion range
    size_t end_offset;
    size_t end_piece = find_piece_at_pos(pt, end, &end_offset);
    if (end_offset > 0) {
        split_piece(pt, end_piece, end_offset);
    }

    // Now remove every piece that lies completely between start_piece and end_piece
    size_t remove_count = end_piece - start_piece;
    if (remove_count > 0) {
        memmove(&pt->pieces[start_piece],
                &pt->pieces[end_piece],
                (pt->piece_count - end_piece) * sizeof(Piece));
        pt->piece_count -= remove_count;
    }

    delete_line_offsets(pt, line_idx, 1);
    return true;
}

bool piece_table_set_line(PieceTable* pt, size_t line_idx, const char* text) {
    if (!pt || line_idx >= pt->line_count) return false;
    if (!piece_table_delete_line(pt, line_idx)) return false;
    return piece_table_insert_line(pt, line_idx, text);
}

char* piece_table_get_line(PieceTable* pt, size_t line_idx) {
    if (!pt || line_idx >= pt->line_count) return strdup("");
    size_t start = pt->line_offsets[line_idx];
    size_t end = (line_idx + 1 < pt->line_count) ? pt->line_offsets[line_idx + 1]
                 : (pt->original_len + pt->add_len);

    size_t len = end - start;
    char* line = malloc(len + 1);
    if (!line) return NULL;

    char* dst = line;
    size_t current = 0;
    for (size_t i = 0; i < pt->piece_count; ++i) {
        const Piece* p = &pt->pieces[i];
        if (current + p->length <= start) {
            current += p->length;
            continue;
        }
        if (current >= end) break;

        size_t from = (p->start > start) ? p->start : start;
        size_t to   = (p->start + p->length < end) ? p->start + p->length : end;
        if (to > from) {
            const char* src = p->is_original ? pt->original : pt->add_buffer;
            size_t copy_len = to - from;
            size_t src_offset = p->start + (from - current);
            memcpy(dst, src + src_offset, copy_len);
            dst += copy_len;
        }
        current += p->length;
    }
    *dst = '\0';
    return line;
}

void piece_table_free(PieceTable* pt) {
    if (!pt) return;
    piece_table_clear_undo(pt);
    free(pt->original);
    free(pt->add_buffer);
    free(pt->pieces);
    free(pt->line_offsets);
    memset(pt, 0, sizeof(*pt));
}

// Undo/Redo

static void grow_pieces(PieceTable* pt, size_t min_needed) {
    if (min_needed <= pt->piece_capacity) return;
    size_t new_cap = pt->piece_capacity ? pt->piece_capacity * 2 : 128;
    while (new_cap < min_needed) new_cap *= 2;
    Piece* new_p = realloc(pt->pieces, new_cap * sizeof(Piece));
    if (!new_p) {
        fprintf(stderr, "FATAL: Out of memory in grow_pieces\n");
        exit(1);
    }
    pt->pieces = new_p;
    pt->piece_capacity = new_cap;
}

static void grow_add_buffer(PieceTable* pt, size_t needed) {
    if (pt->add_len + needed <= pt->add_capacity) return;
    size_t new_cap = pt->add_capacity ? pt->add_capacity * 2 : 8192;
    while (new_cap < pt->add_len + needed) new_cap *= 2;
    char* new_buf = realloc(pt->add_buffer, new_cap);
    if (!new_buf) {
        fprintf(stderr, "FATAL: Out of memory in grow_add_buffer\n");
        exit(1);
    }
    pt->add_buffer = new_buf;
    pt->add_capacity = new_cap;
}

static void grow_undo(PieceTable* pt) {
    if (pt->undo_count + 1 > pt->undo_capacity) {
        size_t new_cap = pt->undo_capacity ? pt->undo_capacity * 2 : 32;
        EditSnapshot* new_s = realloc(pt->undo_stack, new_cap * sizeof(EditSnapshot));
        if (!new_s) return; // fail gracefully
        pt->undo_stack = new_s;
        pt->undo_capacity = new_cap;
    }
}

static void grow_redo(PieceTable* pt) {
    if (pt->redo_count + 1 > pt->redo_capacity) {
        size_t new_cap = pt->redo_capacity ? pt->redo_capacity * 2 : 32;
        EditSnapshot* new_s = realloc(pt->redo_stack, new_cap * sizeof(EditSnapshot));
        if (!new_s) return;
        pt->redo_stack = new_s;
        pt->redo_capacity = new_cap;
    }
}

static void save_undo_state(PieceTable* pt) {
    grow_undo(pt);
    EditSnapshot* snap = &pt->undo_stack[pt->undo_count++];
    snap->pieces = malloc(pt->piece_count * sizeof(Piece));
    if (snap->pieces) {
        memcpy(snap->pieces, pt->pieces, pt->piece_count * sizeof(Piece));
        snap->piece_count = pt->piece_count;
        snap->add_len = pt->add_len;
    } else {
        snap->piece_count = 0;
    }
    // Clear redo on new edit
    for (size_t i = 0; i < pt->redo_count; ++i) free(pt->redo_stack[i].pieces);
    pt->redo_count = 0;
}

static void restore_snapshot(PieceTable* pt, EditSnapshot* snap, bool is_undo) {
    free(pt->pieces);
    pt->pieces = malloc(snap->piece_count * sizeof(Piece));
    if (pt->pieces) {
        memcpy(pt->pieces, snap->pieces, snap->piece_count * sizeof(Piece));
        pt->piece_count = snap->piece_count;
        pt->piece_capacity = snap->piece_count + 64; // some headroom
        pt->add_len = snap->add_len;
    }
    // Rebuild line index after restore
    rebuild_line_index(pt);
}

bool piece_table_undo(PieceTable* pt) {
    if (!piece_table_can_undo(pt)) return false;
    // Save current state to redo
    grow_redo(pt);
    EditSnapshot* redo_snap = &pt->redo_stack[pt->redo_count++];
    redo_snap->pieces = malloc(pt->piece_count * sizeof(Piece));
    if (redo_snap->pieces) {
        memcpy(redo_snap->pieces, pt->pieces, pt->piece_count * sizeof(Piece));
        redo_snap->piece_count = pt->piece_count;
        redo_snap->add_len = pt->add_len;
    }
    // Restore previous state
    pt->undo_count--;
    restore_snapshot(pt, &pt->undo_stack[pt->undo_count], true);
    free(pt->undo_stack[pt->undo_count].pieces);
    return true;
}

bool piece_table_redo(PieceTable* pt) {
    if (!piece_table_can_redo(pt)) return false;
    // Save current to undo
    grow_undo(pt);
    EditSnapshot* undo_snap = &pt->undo_stack[pt->undo_count++];
    undo_snap->pieces = malloc(pt->piece_count * sizeof(Piece));
    if (undo_snap->pieces) {
        memcpy(undo_snap->pieces, pt->pieces, pt->piece_count * sizeof(Piece));
        undo_snap->piece_count = pt->piece_count;
        undo_snap->add_len = pt->add_len;
    }
    // Restore redo
    restore_snapshot(pt, &pt->redo_stack[--pt->redo_count], false);
    free(pt->redo_stack[pt->redo_count].pieces);
    return true;
}

bool piece_table_can_undo(const PieceTable* pt) { return pt->undo_count > 0; }
bool piece_table_can_redo(const PieceTable* pt) { return pt->redo_count > 0; }

void piece_table_clear_undo(PieceTable* pt) {
    for (size_t i = 0; i < pt->undo_count; ++i) free(pt->undo_stack[i].pieces);
    for (size_t i = 0; i < pt->redo_count; ++i) free(pt->redo_stack[i].pieces);
    free(pt->undo_stack);
    free(pt->redo_stack);
    pt->undo_stack = pt->redo_stack = NULL;
    pt->undo_count = pt->redo_count = pt->undo_capacity = pt->redo_capacity = 0;
}
