// piece_table.h
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    LINE_ENDING_UNIX,      // \n
    LINE_ENDING_WINDOWS,   // \r\n
    LINE_ENDING_MAC,       // \r
    LINE_ENDING_PRESERVE
} LineEnding;

typedef struct {
    bool   is_original;
    size_t start;
    size_t length;
} Piece;

// Undo/Redo snapshot (simple deep copy of pieces)
typedef struct {
    Piece* pieces;
    size_t piece_count;
    size_t add_len;           // snapshot of add_buffer length at that time
} EditSnapshot;

typedef struct {
    char*  original;
    size_t original_len;

    char*  add_buffer;
    size_t add_len;
    size_t add_capacity;

    Piece* pieces;
    size_t piece_count;
    size_t piece_capacity;

    size_t* line_offsets;
    size_t  line_count;

    LineEnding original_line_ending;
    LineEnding save_line_ending;

    // Undo/Redo
    EditSnapshot* undo_stack;
    size_t undo_count;
    size_t undo_capacity;
    EditSnapshot* redo_stack;
    size_t redo_count;
    size_t redo_capacity;
} PieceTable;

bool piece_table_load_file(PieceTable* pt, const char* filename);
bool piece_table_save_file(PieceTable* pt, const char* filename, LineEnding force_eol);

bool piece_table_insert_line(PieceTable* pt, size_t line_idx, const char* text);
bool piece_table_set_line(PieceTable* pt, size_t line_idx, const char* text);
bool piece_table_delete_line(PieceTable* pt, size_t line_idx);

char* piece_table_get_line(PieceTable* pt, size_t line_idx); // caller must free

bool piece_table_undo(PieceTable* pt);
bool piece_table_redo(PieceTable* pt);
bool piece_table_can_undo(const PieceTable* pt);
bool piece_table_can_redo(const PieceTable* pt);
void piece_table_clear_undo(PieceTable* pt);

void piece_table_free(PieceTable* pt);
