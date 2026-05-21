// piece_table_tests.c
#include "piece_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void generate_text_file(const char* filename, int num_lines) {
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    for (int i = 0; i < num_lines; ++i) {
        fprintf(f, "This is line %d of the large test file.\n", i);
    }
    fclose(f);
    printf("Generated %s (%d lines)\n", filename, num_lines);
}

static void test_basic_operations(void) {
    PieceTable pt = {};
    assert(piece_table_load_file(&pt, "test_5000_unix.txt"));

    assert(piece_table_insert_line(&pt, 0, "NEW FIRST LINE"));
    assert(piece_table_insert_line(&pt, 5, "INSERTED IN MIDDLE"));
    assert(piece_table_set_line(&pt, 10, "REPLACED LINE"));

    char* line = piece_table_get_line(&pt, 0);
    assert(strcmp(line, "NEW FIRST LINE\n") == 0);
    free(line);

    assert(piece_table_delete_line(&pt, 5));
    assert(piece_table_can_undo(&pt));

    piece_table_undo(&pt);
    piece_table_redo(&pt);

    piece_table_free(&pt);
    printf("Basic operations test passed.\n");
}

static void test_random_edits(PieceTable *pt) {
    printf("Performing 5000 random edits on %zu lines...\n", pt->line_count);
	int insert = 0, delete = 0, set = 0;

    for (int i = 0; i < 5000; ++i) {
        size_t idx = rand() % (pt->line_count + 1);
        int op = rand() % 3;
        if (op == 0) {
            piece_table_insert_line(pt, idx, "+++ RANDOM INSERT +++");
			insert++;
        } else if (op == 1 && pt->line_count > 0) {
            piece_table_delete_line(pt, idx % pt->line_count);
			delete++;
        } else if (pt->line_count > 0) {
            piece_table_set_line(pt, idx % pt->line_count, "=== RANDOM SET ===");
			set++;
        }
    }
    printf("Random edits completed successfully. insert=%d delete=%d set=%d Final line count: %zu\n",
			insert, delete, set, pt->line_count);
}

int main(void) {
    srand(time(NULL));
    printf("=== PieceTable Unit & Performance Tests ===\n\n");

    generate_text_file("test_5000_unix.txt", 5000);
    generate_text_file("test_20000_win.txt", 20000);
    generate_text_file("test_50000_mac.txt", 50000);

    // Performance load
    PieceTable pt = {};
    double start = get_time_ms();
    assert(piece_table_load_file(&pt, "test_50000_mac.txt"));
    double load_time = get_time_ms() - start;
    printf("Load time: %.2f ms (%zu lines)\n", load_time, pt.line_count);

    // Basic tests
    test_basic_operations();

    start = get_time_ms();
    // Heavy random edits
    test_random_edits(&pt);
    double editing_time = get_time_ms() - start;
    printf("5k edits: %.2f ms\n", editing_time);

    // Final performance insert
    start = get_time_ms();
    for (int i = 0; i < 100; ++i) {
        piece_table_insert_line(&pt, rand() % pt.line_count, "+++ PERFORMANCE TEST LINE +++");
    }
    double insert_time = get_time_ms() - start;
    printf("Inserted 100 lines: %.2f ms\n", insert_time);

	piece_table_save_file(&pt, "edited.txt", LINE_ENDING_PRESERVE);

    piece_table_free(&pt);

    printf("\nAll tests completed successfully.\n");
    return 0;
}
