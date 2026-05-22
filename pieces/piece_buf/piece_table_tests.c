/* ==========================================================================
 *  piece_table_tests.c
 *
 *  Exercises the entire public API surface of piece_table.h.  Each test
 *  function targets a logical group of functions; a final stress test
 *  cross-checks the piece table against a flat reference buffer over a
 *  long sequence of mixed line + byte-range edits.
 *
 *  Build: ISO C11 only (uses timespec_get from <time.h>).  No POSIX, no
 *  platform-specific headers.
 * ========================================================================= */
#include "piece_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

/* ===== Test harness ===================================================== */

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond) do {                                                   \
    g_checks++;                                                            \
    if (!(cond)) {                                                         \
        g_failures++;                                                      \
        fprintf(stderr, "  CHECK FAILED: %s  (%s:%d)\n",                   \
                #cond, __FILE__, __LINE__);                                \
    }                                                                      \
} while (0)

#define CHECK_STR_EQ(a, b) do {                                            \
    g_checks++;                                                            \
    const char* _a = (a); const char* _b = (b);                            \
    if (!_a || !_b || strcmp(_a, _b) != 0) {                               \
        g_failures++;                                                      \
        fprintf(stderr, "  CHECK FAILED: \"%s\" != \"%s\"  (%s:%d)\n",     \
                _a ? _a : "(null)", _b ? _b : "(null)",                    \
                __FILE__, __LINE__);                                       \
    }                                                                      \
} while (0)

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);            /* C11 standard */
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* Build a flat NUL-terminated copy of the entire piece table for whole-
 * document comparisons. */
static char* pt_to_string(const PieceTable* pt) {
    return pt_dup_byte_range(pt, 0, pt_byte_length(pt), NULL);
}

/* ===== Construction / state ============================================ */

static void test_construction(void) {
    printf("[ construction ]\n");

    /* pt_new -- empty buffer should look like a single empty line. */
    PieceTable* e = pt_new();
    CHECK(e != NULL);
    CHECK(pt_byte_length(e) == 0);
    CHECK(pt_line_count(e) == 1);             /* the empty trailing line  */
    CHECK(pt_detected_eol(e) == PT_EOL_UNIX);
    CHECK(!pt_is_modified(e));
    pt_free(e);

    /* pt_new_from_string + EOL normalisation */
    PieceTable* u = pt_new_from_string("hello\nworld\n", 12);
    CHECK(pt_byte_length(u) == 12);
    CHECK(pt_line_count(u) == 3);
    CHECK(pt_detected_eol(u) == PT_EOL_UNIX);
    pt_free(u);

    PieceTable* w = pt_new_from_string("a\r\nb\r\n", 6);
    CHECK(pt_detected_eol(w) == PT_EOL_WINDOWS);
    CHECK(pt_byte_length(w) == 4);             /* normalised */
    pt_free(w);

    PieceTable* m = pt_new_from_string("a\rb\r", 4);
    CHECK(pt_detected_eol(m) == PT_EOL_MAC);
    CHECK(pt_byte_length(m) == 4);
    pt_free(m);

    /* Defensive: NULL inputs return safely. */
    CHECK(pt_new_from_string(NULL, 0) != NULL);  /* empty, not failure */
    pt_free(pt_new_from_string(NULL, 0));
    CHECK(pt_new_from_file(NULL) == NULL);
    CHECK(pt_new_from_file("/this/path/does/not/exist") == NULL);
    pt_free(NULL);                               /* shouldn't crash */
}

/* ===== Position conversion ============================================= */

static void test_position_conversion(void) {
    printf("[ position conversion ]\n");

    PieceTable* pt = pt_new_from_string("abc\nde\nfghi\n", 12);
    /* "abc\n" [0..4), "de\n" [4..7), "fghi\n" [7..12), phantom [12..12). */
    CHECK(pt_line_count(pt) == 4);

    /* pt_line_to_byte over every valid line index */
    CHECK(pt_line_to_byte(pt, 0) == 0);
    CHECK(pt_line_to_byte(pt, 1) == 4);
    CHECK(pt_line_to_byte(pt, 2) == 7);
    CHECK(pt_line_to_byte(pt, 3) == 12);
    CHECK(pt_line_to_byte(pt, 4) == 12);           /* one-past-end OK */
    CHECK(pt_line_to_byte(pt, 99) == SIZE_MAX);    /* oor */

    /* pt_line_length */
    CHECK(pt_line_length(pt, 0) == 4);
    CHECK(pt_line_length(pt, 1) == 3);
    CHECK(pt_line_length(pt, 2) == 5);
    CHECK(pt_line_length(pt, 3) == 0);
    CHECK(pt_line_length(pt, 99) == 0);

    /* pt_byte_to_point sanity */
    PT_Point p;
    p = pt_byte_to_point(pt, 0);  CHECK(p.line == 0 && p.column == 0);
    p = pt_byte_to_point(pt, 6);  CHECK(p.line == 1 && p.column == 2);
    p = pt_byte_to_point(pt, 12); CHECK(p.line == 3 && p.column == 0);

    /* Round-trip every byte, including byte_length. */
    for (size_t b = 0; b <= pt_byte_length(pt); ++b) {
        PT_Point q = pt_byte_to_point(pt, b);
        CHECK(pt_point_to_byte(pt, q) == b);
    }

    /* Clamping */
    p = pt_byte_to_point(pt, 9999);
    CHECK(p.line == 3 && p.column == 0);
    PT_Point oor = { 99, 99 };
    CHECK(pt_point_to_byte(pt, oor) == pt_byte_length(pt));
    /* column beyond line length is clamped */
    PT_Point past_col = { 1, 99 };
    CHECK(pt_point_to_byte(pt, past_col) == pt_line_to_byte(pt, 2));

    pt_free(pt);
}

/* ===== Reading ========================================================= */

static void test_reading(void) {
    printf("[ reading ]\n");

    PieceTable* pt = pt_new_from_string("alpha\nbeta\ngamma\n", 17);

    /* pt_get_line: exact-fit, oversize, undersize, zero-cap */
    char buf[64];
    size_t need = pt_get_line(pt, 1, buf, sizeof buf);
    CHECK(need == 5);                         /* "beta\n" */
    CHECK_STR_EQ(buf, "beta\n");

    /* undersized buffer truncates but still NUL-terminates */
    char small[4];
    need = pt_get_line(pt, 1, small, sizeof small);
    CHECK(need == 5);
    CHECK(small[3] == '\0');
    CHECK(strlen(small) == 3);                /* "bet" */

    /* zero capacity is a length-query */
    need = pt_get_line(pt, 2, NULL, 0);
    CHECK(need == 6);                         /* "gamma\n" */

    /* out-of-range line returns 0 and writes a NUL when possible */
    buf[0] = 'x';
    need = pt_get_line(pt, 999, buf, sizeof buf);
    CHECK(need == 0);
    CHECK(buf[0] == '\0');

    /* pt_dup_line */
    char* dl = pt_dup_line(pt, 0);
    CHECK_STR_EQ(dl, "alpha\n");
    free(dl);
    CHECK(pt_dup_line(pt, 999) == NULL);

    /* pt_get_byte_range, pt_dup_byte_range */
    need = pt_get_byte_range(pt, 0, 5, buf, sizeof buf);
    CHECK(need == 5);
    CHECK_STR_EQ(buf, "alpha");

    /* clamping */
    need = pt_get_byte_range(pt, 14, 9999, buf, sizeof buf);
    CHECK(need == 3);                         /* "ma\n" */
    CHECK_STR_EQ(buf, "ma\n");

    size_t L = 0;
    char* dr = pt_dup_byte_range(pt, 6, 10, &L);
    CHECK(L == 4);
    CHECK_STR_EQ(dr, "beta");
    free(dr);

    /* pt_dup_range -- swap order works (start > end gets swapped) */
    PT_Point hi = { 1, 4 }, lo = { 0, 1 };
    dr = pt_dup_range(pt, hi, lo, &L);
    CHECK(L == 9);                            /* "lpha\nbet" -> wait: */
    /* 0,1..1,4 spans bytes 1..10 = "lpha\nbeta" (9 bytes) */
    CHECK_STR_EQ(dr, "lpha\nbeta");
    free(dr);

    /* pt_read: full-document streaming round trip */
    size_t total = pt_byte_length(pt);
    char* gold = pt_to_string(pt);
    char acc[64] = {0};
    size_t off = 0, n;
    while ((n = pt_read(pt, off, acc + off, 5)) > 0) off += n;
    CHECK(off == total);
    CHECK(memcmp(acc, gold, total) == 0);

    /* pt_read past EOF returns 0 */
    CHECK(pt_read(pt, total, buf, sizeof buf) == 0);
    CHECK(pt_read(pt, 99999, buf, sizeof buf) == 0);

    free(gold);
    pt_free(pt);
}

/* ===== Line-oriented edits + PT_Edit descriptor ======================== */

static void test_line_edits(void) {
    printf("[ line edits ]\n");

    PieceTable* pt = pt_new_from_string("a\nb\nc\n", 6);
    pt_clear_modified(pt);
    PT_Edit e;

    /* pt_insert_line */
    CHECK(pt_insert_line(pt, 1, "X", 1, &e));
    CHECK(pt_is_modified(pt));
    CHECK(pt_line_count(pt) == 5);
    CHECK(pt_byte_length(pt) == 8);
    CHECK(e.start_byte == 2 && e.old_end_byte == 2 && e.new_end_byte == 4);
    CHECK(e.start_point.line == 1 && e.start_point.column == 0);
    CHECK(e.new_end_point.line == 2 && e.new_end_point.column == 0);

    char* doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "a\nX\nb\nc\n");
    free(doc);

    /* pt_set_line: line 0 becomes "AA" */
    CHECK(pt_set_line(pt, 0, "AA", 2, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "AA\nX\nb\nc\n");
    CHECK(e.start_byte == 0 && e.old_end_byte == 2 && e.new_end_byte == 3);
    free(doc);

    /* pt_delete_line: drop "X" */
    CHECK(pt_delete_line(pt, 1, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "AA\nb\nc\n");
    CHECK(e.start_byte == 3 && e.old_end_byte == 5 && e.new_end_byte == 3);
    free(doc);

    /* pt_append_line */
    CHECK(pt_append_line(pt, "ZZ", 2, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "AA\nb\nc\nZZ\n");
    free(doc);

    /* OOR / NULL guards */
    CHECK(!pt_set_line(pt, 999, "x", 1, NULL));
    CHECK(!pt_delete_line(pt, 999, NULL));
    CHECK(!pt_insert_line(NULL, 0, "x", 1, NULL));
    CHECK(!pt_append_line(NULL, "x", 1, NULL));

    pt_free(pt);
}

/* ===== Range / point edits ============================================ */

static void test_range_edits(void) {
    printf("[ range / point edits ]\n");

    PieceTable* pt = pt_new_from_string("alpha\nbeta\ngamma\n", 17);
    PT_Edit e;

    /* pt_insert_at_byte */
    CHECK(pt_insert_at_byte(pt, 5, "!!", 2, &e));
    char* doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "alpha!!\nbeta\ngamma\n");
    CHECK(e.start_byte == 5 && e.old_end_byte == 5 && e.new_end_byte == 7);
    free(doc);

    /* pt_insert_at_point */
    PT_Point at = { 1, 0 };
    CHECK(pt_insert_at_point(pt, at, ">>", 2, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "alpha!!\n>>beta\ngamma\n");
    free(doc);

    /* pt_delete_byte_range */
    CHECK(pt_delete_byte_range(pt, 5, 7, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "alpha\n>>beta\ngamma\n");
    CHECK(e.start_byte == 5 && e.old_end_byte == 7 && e.new_end_byte == 5);
    free(doc);

    /* pt_delete_range */
    PT_Point a = { 1, 0 }, b = { 1, 2 };
    CHECK(pt_delete_range(pt, a, b, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "alpha\nbeta\ngamma\n");
    free(doc);

    /* pt_replace_byte_range */
    CHECK(pt_replace_byte_range(pt, 0, 5, "ALPHA", 5, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "ALPHA\nbeta\ngamma\n");
    CHECK(e.start_byte == 0 && e.old_end_byte == 5 && e.new_end_byte == 5);
    free(doc);

    /* pt_replace_range */
    PT_Point r1 = { 1, 0 }, r2 = { 1, 4 };
    CHECK(pt_replace_range(pt, r1, r2, "BETA!", 5, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "ALPHA\nBETA!\ngamma\n");
    free(doc);

    /* Range that swaps points still works. */
    PT_Point hi = { 2, 5 }, lo = { 2, 0 };
    CHECK(pt_delete_range(pt, hi, lo, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "ALPHA\nBETA!\n\n");
    free(doc);

    /* Multi-line replacement inserts newlines and updates line index. */
    PT_Point z1 = { 0, 0 }, z2 = { 2, 0 };
    CHECK(pt_replace_range(pt, z1, z2, "one\ntwo\nthree\n", 14, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "one\ntwo\nthree\n\n");
    CHECK(pt_line_count(pt) == 5);            /* 4 newlines + phantom */
    free(doc);

    pt_free(pt);
}

/* ===== Modified flag =================================================== */

static void test_modified_flag(void) {
    printf("[ modified flag ]\n");

    PieceTable* pt = pt_new_from_string("hi\n", 3);
    CHECK(!pt_is_modified(pt));

    CHECK(pt_insert_line(pt, 0, "X", 1, NULL));
    CHECK(pt_is_modified(pt));
    pt_clear_modified(pt);
    CHECK(!pt_is_modified(pt));

    /* Save clears modified. */
    CHECK(pt_insert_line(pt, 0, "Y", 1, NULL));
    CHECK(pt_is_modified(pt));
    CHECK(pt_save_file(pt, "tmp_mod_check.txt", PT_EOL_UNIX));
    CHECK(!pt_is_modified(pt));
    remove("tmp_mod_check.txt");

    pt_free(pt);
}

/* ===== Undo / redo ===================================================== */

static void test_undo_redo(void) {
    printf("[ undo / redo ]\n");

    PieceTable* pt = pt_new_from_string("a\nb\nc\n", 6);

    CHECK(!pt_can_undo(pt));
    CHECK(!pt_can_redo(pt));

    /* set_line creates exactly one undo entry (was a known regression). */
    CHECK(pt_set_line(pt, 1, "BBB", 3, NULL));
    CHECK(pt_can_undo(pt));
    CHECK(!pt_can_redo(pt));

    char* doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "a\nBBB\nc\n");
    free(doc);

    /* Undo reports a PT_Edit covering the whole document. */
    PT_Edit e;
    CHECK(pt_undo(pt, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "a\nb\nc\n");
    free(doc);
    CHECK(e.start_byte == 0);
    CHECK(e.new_end_byte == pt_byte_length(pt));
    CHECK(pt_can_redo(pt));

    /* Redo brings it back, also with a PT_Edit. */
    CHECK(pt_redo(pt, &e));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "a\nBBB\nc\n");
    free(doc);
    CHECK(e.new_end_byte == pt_byte_length(pt));

    /* New edit clears redo. */
    CHECK(pt_insert_line(pt, 0, "Z", 1, NULL));
    CHECK(!pt_can_redo(pt));

    /* Multi-step undo back to start. */
    while (pt_can_undo(pt)) CHECK(pt_undo(pt, NULL));
    doc = pt_to_string(pt);
    CHECK_STR_EQ(doc, "a\nb\nc\n");
    free(doc);

    /* pt_clear_history wipes both stacks. */
    CHECK(pt_set_line(pt, 0, "QQ", 2, NULL));
    CHECK(pt_can_undo(pt));
    pt_clear_history(pt);
    CHECK(!pt_can_undo(pt));
    CHECK(!pt_can_redo(pt));

    /* undo/redo on empty stacks returns false. */
    CHECK(!pt_undo(pt, NULL));
    CHECK(!pt_redo(pt, NULL));

    pt_free(pt);
}

/* ===== Save / load round-trips ========================================= */

static void test_save_and_eol(void) {
    printf("[ save / EOL conversion ]\n");

    PieceTable* pt = pt_new_from_string("a\nb\n", 4);
    CHECK(pt_save_file(pt, "tmp_unix.txt", PT_EOL_UNIX));
    CHECK(pt_save_file(pt, "tmp_win.txt",  PT_EOL_WINDOWS));
    CHECK(pt_save_file(pt, "tmp_mac.txt",  PT_EOL_MAC));
    pt_free(pt);

    /* Round-trip: detected EOL matches what we wrote. */
    PieceTable* a = pt_new_from_file("tmp_unix.txt");
    PieceTable* b = pt_new_from_file("tmp_win.txt");
    PieceTable* c = pt_new_from_file("tmp_mac.txt");
    CHECK(a && b && c);
    CHECK(pt_detected_eol(a) == PT_EOL_UNIX);
    CHECK(pt_detected_eol(b) == PT_EOL_WINDOWS);
    CHECK(pt_detected_eol(c) == PT_EOL_MAC);

    /* All three should normalise to identical in-memory bytes. */
    char* sa = pt_to_string(a);
    char* sb = pt_to_string(b);
    char* sc = pt_to_string(c);
    CHECK_STR_EQ(sa, sb);
    CHECK_STR_EQ(sb, sc);
    free(sa); free(sb); free(sc);

    /* PT_EOL_PRESERVE writes back what was detected. */
    CHECK(pt_save_file(b, "tmp_preserve.txt", PT_EOL_PRESERVE));
    FILE* f = fopen("tmp_preserve.txt", "rb");
    CHECK(f != NULL);
    char raw[16] = {0};
    size_t got = fread(raw, 1, sizeof raw, f);
    fclose(f);
    CHECK(got == 6);                          /* "a\r\nb\r\n" */
    CHECK(memcmp(raw, "a\r\nb\r\n", 6) == 0);

    pt_free(a); pt_free(b); pt_free(c);
    remove("tmp_unix.txt");
    remove("tmp_win.txt");
    remove("tmp_mac.txt");
    remove("tmp_preserve.txt");
}

/* ===== Stress test: compare against a reference flat buffer =========== */

/* Minimal "Buf" reference implementation: a growable char array. */
typedef struct { char* data; size_t len, cap; } Buf;

static void buf_reserve(Buf* b, size_t n) {
    if (n <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 64;
    while (nc < n) nc *= 2;
    b->data = (char*)realloc(b->data, nc);
    b->cap = nc;
}

static void buf_replace(Buf* b, size_t s, size_t e,
                        const char* t, size_t tl)
{
    if (s > b->len) s = b->len;
    if (e > b->len) e = b->len;
    if (e < s)      e = s;
    size_t new_len = b->len - (e - s) + tl;
    buf_reserve(b, new_len);
    memmove(b->data + s + tl, b->data + e, b->len - e);
    if (tl > 0) memcpy(b->data + s, t, tl);
    b->len = new_len;
}

static size_t buf_line_count(const Buf* b) {
    size_t c = 1;
    for (size_t i = 0; i < b->len; ++i) if (b->data[i] == '\n') c++;
    return c;
}

static void test_stress_against_reference(void) {
    printf("[ stress vs. reference (10000 mixed ops) ]\n");

    const char seed[] =
        "lorem ipsum dolor\nsit amet\nconsectetur\nadipiscing elit\n";
    PieceTable* pt = pt_new_from_string(seed, sizeof seed - 1);
    Buf ref = { NULL, 0, 0 };
    buf_replace(&ref, 0, 0, seed, sizeof seed - 1);

    const char* payloads[] = {
        "X", "abc", "1\n2", "----", "hello world\n",
        "", "\n", "longer payload\nspanning\nlines\n"
    };
    const size_t npayloads = sizeof payloads / sizeof payloads[0];

    for (int i = 0; i < 10000; ++i) {
        size_t doc_len = pt_byte_length(pt);
        CHECK(doc_len == ref.len);
        if (doc_len != ref.len) { fprintf(stderr, "  desync at iter %d\n", i); break; }

        size_t a = (size_t)rand() % (doc_len + 1);
        size_t b = a + (size_t)rand() % (doc_len - a + 1);
        const char* t = payloads[(unsigned)rand() % npayloads];
        size_t tl = strlen(t);

        int op = rand() % 6;
        if (op == 0) {
            pt_insert_at_byte(pt, a, t, tl, NULL);
            buf_replace(&ref, a, a, t, tl);
        } else if (op == 1) {
            pt_delete_byte_range(pt, a, b, NULL);
            buf_replace(&ref, a, b, NULL, 0);
        } else if (op == 2) {
            pt_replace_byte_range(pt, a, b, t, tl, NULL);
            buf_replace(&ref, a, b, t, tl);
        } else if (op == 3) {
            /* Line insert: we know the buffer always ends in \n on lines we
             * insert via this API, so model it the same way in the ref. */
            size_t lc = pt_line_count(pt);
            size_t li = (size_t)rand() % (lc + 1);
            size_t byte = pt_line_to_byte(pt, li);
            pt_insert_line(pt, li, t, tl, NULL);
            char* nl = (char*)malloc(tl + 1);
            memcpy(nl, t, tl); nl[tl] = '\n';
            buf_replace(&ref, byte, byte, nl, tl + 1);
            free(nl);
        } else if (op == 4 && pt_line_count(pt) > 1) {
            size_t li = (size_t)rand() % pt_line_count(pt);
            size_t s = pt_line_to_byte(pt, li);
            size_t e = pt_line_to_byte(pt, li + 1);
            pt_delete_line(pt, li, NULL);
            buf_replace(&ref, s, e, NULL, 0);
        } else if (op == 5 && pt_line_count(pt) > 1) {
            size_t li = (size_t)rand() % pt_line_count(pt);
            size_t s = pt_line_to_byte(pt, li);
            size_t e = pt_line_to_byte(pt, li + 1);
            pt_set_line(pt, li, t, tl, NULL);
            char* nl = (char*)malloc(tl + 1);
            memcpy(nl, t, tl); nl[tl] = '\n';
            buf_replace(&ref, s, e, nl, tl + 1);
            free(nl);
        }
    }

    /* Final equality check */
    CHECK(pt_byte_length(pt) == ref.len);
    CHECK(pt_line_count(pt) == buf_line_count(&ref));
    char* doc = pt_to_string(pt);
    int eq = (memcmp(doc, ref.data, ref.len) == 0);
    CHECK(eq);
    if (!eq) {
        size_t i = 0;
        while (i < ref.len && doc[i] == ref.data[i]) i++;
        fprintf(stderr, "  first mismatch at byte %zu (of %zu)\n", i, ref.len);
    }
    free(doc);

    free(ref.data);
    pt_free(pt);
}

/* ===== Light performance pass ========================================= */

static void generate_text_file(const char* filename, int num_lines) {
    FILE* f = fopen(filename, "wb");
    if (!f) { perror(filename); exit(1); }
    for (int i = 0; i < num_lines; ++i)
        fprintf(f, "This is line %d of the large test file.\n", i);
    fclose(f);
}

static void test_performance(void) {
    printf("[ performance ]\n");
    generate_text_file("test_5000_unix.txt", 5000000);

    double t = now_ms();
    PieceTable* pt = pt_new_from_file("test_5000_unix.txt");
    CHECK(pt != NULL);
    printf("  load: %.2f ms  (%zu lines, %zu bytes)\n",
           now_ms() - t, pt_line_count(pt), pt_byte_length(pt));

    t = now_ms();
    int ins = 0, del = 0, set = 0;
    for (int i = 0; i < 5000; ++i) {
        size_t lc = pt_line_count(pt);
        size_t idx = (size_t)rand() % (lc + 1);
        int op = rand() % 3;
        if (op == 0) {
            pt_insert_line(pt, idx, "+++ RANDOM INSERT +++", 21, NULL); ins++;
        } else if (op == 1 && lc > 0) {
            pt_delete_line(pt, idx % lc, NULL); del++;
        } else if (lc > 0) {
            pt_set_line(pt, idx % lc, "=== RANDOM SET ===", 18, NULL); set++;
        }
    }
    printf("  5000 line edits: %.2f ms  (ins=%d del=%d set=%d, lines=%zu)\n",
           now_ms() - t, ins, del, set, pt_line_count(pt));

    t = now_ms();
    for (int i = 0; i < 5000; ++i) {
        size_t doc = pt_byte_length(pt);
        size_t a = doc ? (size_t)rand() % doc : 0;
        size_t b = a + (doc - a > 0 ? (size_t)rand() % (doc - a) : 0);
        if ((rand() & 1) || a == b)
            pt_insert_at_byte(pt, a, "INS", 3, NULL);
        else
            pt_delete_byte_range(pt, a, b, NULL);
    }
    printf("  5000 byte-range edits: %.2f ms\n", now_ms() - t);

    pt_free(pt);
    //remove("test_5000_unix.txt");
}

/* ===== main ============================================================ */

int main(void) {
    srand((unsigned)time(NULL));
    printf("=== PieceTable API tests ===\n\n");

    test_construction();
    test_position_conversion();
    test_reading();
    test_line_edits();
    test_range_edits();
    test_modified_flag();
    test_undo_redo();
    test_save_and_eol();
    test_stress_against_reference();
    test_performance();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
