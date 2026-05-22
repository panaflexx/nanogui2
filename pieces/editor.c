#include "piece_buf/piece_table.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <ctype.h>
#include "tree-sitter/lib/include/tree_sitter/api.h"

typedef struct {
    PieceTable* pt;
    int cursor_line;
    int cursor_col;
    int screen_top;
    char filename[256];
    bool insert_mode;
    bool running;
    
    /* Clipboard */
    char* clipboard;
    size_t clip_len;

    /* Tree-sitter */
    TSParser* parser;
    TSTree* tree;
} Editor;

/* Forward declarations */
static void editor_init(Editor* ed, const char* filename);
static void editor_free(Editor* ed);
static void draw_status(const Editor* ed);
static void redraw(Editor* ed);
static void scroll_to_cursor(Editor* ed);
static void goto_line(Editor* ed, int target_line);
static size_t get_current_byte_pos(const Editor* ed);
static void move_to_byte_pos(Editor* ed, size_t byte_pos);
static bool is_word_char(char c);
static void move_word_forward(Editor* ed);
static void move_word_backward(Editor* ed);
static void delete_word(Editor* ed);
static void move_cursor(Editor* ed, int dline, int dcol);
static void yank_line(Editor* ed);
static void paste(Editor* ed);
static void handle_normal_mode(Editor* ed, int ch);
static void handle_insert_mode(Editor* ed, int ch);

/* ====================== Syntax Highlighting ====================== */

static bool is_keyword(const char* word) {
    static const char* keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while", "_Bool", "_Complex", "_Imaginary",
        NULL
    };
    for (int i = 0; keywords[i]; ++i) {
        if (strcmp(word, keywords[i]) == 0) return true;
    }
    return false;
}

static void print_highlighted(const char* line, int y) {
    int x = 0;
    const char* p = line;
    char word[128];

    while (*p) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            int len = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && len < 127) {
                word[len++] = *p++;
            }
            word[len] = '\0';

            if (is_keyword(word)) {
                attron(COLOR_PAIR(1));  /* Keywords */
                mvprintw(y, x, "%s", word);
                attroff(COLOR_PAIR(1));
            } else {
                mvprintw(y, x, "%s", word);
            }
            x += len;
        } 
        else if (*p == '"' || *p == '\'') {
            /* String / char literal */
            attron(COLOR_PAIR(2));
            char quote = *p;
            mvprintw(y, x++, "%c", *p++);
            while (*p && *p != quote) {
                if (*p == '\\' && *(p+1)) mvprintw(y, x++, "%c", *p++);
                mvprintw(y, x++, "%c", *p++);
            }
            if (*p) mvprintw(y, x++, "%c", *p++);
            attroff(COLOR_PAIR(2));
        } 
        else if (*p == '/' && *(p+1) == '/') {
            /* Line comment */
            attron(COLOR_PAIR(3));
            while (*p) mvprintw(y, x++, "%c", *p++);
            attroff(COLOR_PAIR(3));
            break;
        } 
        else {
            mvprintw(y, x++, "%c", *p++);
        }
    }
}

/* ====================== Editor Core ====================== */

static void editor_init(Editor* ed, const char* filename) {
    memset(ed, 0, sizeof(Editor));
    ed->pt = filename && *filename ? pt_new_from_file(filename) : pt_new();
    if (!ed->pt) ed->pt = pt_new();

    strncpy(ed->filename, filename && *filename ? filename : "untitled.txt", sizeof(ed->filename)-1);
    ed->cursor_line = 0;
    ed->cursor_col = 0;
    ed->screen_top = 0;
    ed->insert_mode = false;
    ed->running = true;
    ed->clipboard = NULL;
    ed->clip_len = 0;

    /* Tree-sitter */
    ed->parser = ts_parser_new();
    ed->tree = NULL;
    /* TODO: ts_parser_set_language(ed->parser, tree_sitter_c()); when language is linked */
}

static void editor_free(Editor* ed) {
    if (ed->pt) pt_free(ed->pt);
    if (ed->parser) ts_parser_delete(ed->parser);
    if (ed->tree) ts_tree_delete(ed->tree);
    free(ed->clipboard);
    ed->pt = NULL;
    ed->parser = NULL;
    ed->tree = NULL;
    ed->clipboard = NULL;
}

static void draw_status(const Editor* ed) {
    int row = LINES - 1;
    attron(A_REVERSE);
    mvprintw(row, 0, " %s%s | %d/%zu lines | Col %d | %s ",
             ed->filename,
             pt_is_modified(ed->pt) ? " [+]" : "",
             ed->cursor_line + 1,
             pt_line_count(ed->pt),
             ed->cursor_col,
             ed->insert_mode ? "-- INSERT --" : "-- NORMAL --");
    clrtoeol();
    attroff(A_REVERSE);
}

static void redraw(Editor* ed) {
    werase(stdscr);

    int max_lines = LINES - 1;
    size_t total_lines = pt_line_count(ed->pt);

    for (int i = 0; i < max_lines && (size_t)(ed->screen_top + i) < total_lines; ++i) {
        size_t line_idx = (size_t)ed->screen_top + i;
        char buf[2048] = {0};
        pt_get_line(ed->pt, line_idx, buf, sizeof(buf)-1);
        print_highlighted(buf, i);
        clrtoeol();
    }

    /* Show ~ for empty lines */
    for (int i = (int)(total_lines - (size_t)ed->screen_top); i < max_lines; ++i) {
        mvprintw(i, 0, "~");
        clrtoeol();
    }

    draw_status(ed);

    int screen_y = ed->cursor_line - ed->screen_top;
    if (screen_y >= 0 && screen_y < max_lines) {
        size_t line_len = pt_line_length(ed->pt, (size_t)ed->cursor_line);
        int display_col = (ed->cursor_col > (int)line_len) ? (int)line_len : ed->cursor_col;
        move(screen_y, display_col);
    } else {
        move(0, 0);
    }

    refresh();
}

static void scroll_to_cursor(Editor* ed) {
    int max_visible = LINES - 2;
    if (ed->cursor_line < ed->screen_top) {
        ed->screen_top = ed->cursor_line;
    } else if (ed->cursor_line >= ed->screen_top + max_visible) {
        ed->screen_top = ed->cursor_line - max_visible + 1;
    }
    if (ed->screen_top < 0) ed->screen_top = 0;
}

static void goto_line(Editor* ed, int target_line) {
    size_t total = pt_line_count(ed->pt);
    if (total == 0) return;
    if (target_line < 0) target_line = 0;
    if (target_line >= (int)total) target_line = (int)total - 1;

    ed->cursor_line = target_line;
    ed->cursor_col = 0;
    scroll_to_cursor(ed);
}

static size_t get_current_byte_pos(const Editor* ed) {
    return pt_point_to_byte(ed->pt, (PT_Point){(uint32_t)ed->cursor_line, (uint32_t)ed->cursor_col});
}

static void move_to_byte_pos(Editor* ed, size_t byte_pos) {
    PT_Point p = pt_byte_to_point(ed->pt, byte_pos);
    ed->cursor_line = (int)p.line;
    ed->cursor_col = (int)p.column;
    scroll_to_cursor(ed);
}

static bool is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Movement, word, delete, yank, paste, etc. - unchanged from original */

static void move_word_forward(Editor* ed) { /* ... original implementation ... */
    size_t pos = get_current_byte_pos(ed);
    size_t len = pt_byte_length(ed->pt);
    if (pos >= len) return;
    char buf[1];
    bool in_word = false;
    while (pos < len) {
        pt_read(ed->pt, pos, buf, 1);
        if (is_word_char(buf[0])) in_word = true;
        else if (in_word) break;
        pos++;
    }
    while (pos < len) {
        pt_read(ed->pt, pos, buf, 1);
        if (!isspace((unsigned char)buf[0])) break;
        pos++;
    }
    while (pos < len) {
        pt_read(ed->pt, pos, buf, 1);
        if (is_word_char(buf[0])) {
            move_to_byte_pos(ed, pos);
            return;
        }
        pos++;
    }
    move_to_byte_pos(ed, len);
}

static void move_word_backward(Editor* ed) { /* ... original ... */
    size_t pos = get_current_byte_pos(ed);
    if (pos == 0) return;
    char buf[1];
    pos--;
    while (pos > 0) {
        pt_read(ed->pt, pos, buf, 1);
        if (!isspace((unsigned char)buf[0])) break;
        pos--;
    }
    bool in_word = false;
    while (pos > 0) {
        pt_read(ed->pt, pos, buf, 1);
        if (is_word_char(buf[0])) in_word = true;
        else if (in_word) { pos++; break; }
        pos--;
    }
    move_to_byte_pos(ed, pos);
}

static void delete_word(Editor* ed) {
    PT_Edit edit = {0};
    size_t start = get_current_byte_pos(ed);
    size_t pos = start;
    size_t len = pt_byte_length(ed->pt);
    char buf[1];
    while (pos < len) {
        pt_read(ed->pt, pos, buf, 1);
        if (!isspace((unsigned char)buf[0])) break;
        pos++;
    }
    while (pos < len) {
        pt_read(ed->pt, pos, buf, 1);
        if (!is_word_char(buf[0])) break;
        pos++;
    }
    if (pos > start) pt_delete_byte_range(ed->pt, start, pos, &edit);
}

static void move_cursor(Editor* ed, int dline, int dcol) {
    size_t total_lines = pt_line_count(ed->pt);
    if (total_lines == 0) return;
    long new_line = (long)ed->cursor_line + dline;
    if (new_line < 0) new_line = 0;
    if (new_line >= (long)total_lines) new_line = (long)total_lines - 1;
    ed->cursor_line = (int)new_line;

    size_t line_len = pt_line_length(ed->pt, (size_t)ed->cursor_line);
    long new_col = (long)ed->cursor_col + dcol;
    if (new_col < 0) new_col = 0;
    if (new_col > (long)line_len) new_col = (long)line_len;
    ed->cursor_col = (int)new_col;
    scroll_to_cursor(ed);
}

static void yank_line(Editor* ed) {
    free(ed->clipboard);
    ed->clipboard = pt_dup_line(ed->pt, (size_t)ed->cursor_line);
    ed->clip_len = ed->clipboard ? strlen(ed->clipboard) : 0;
    mvprintw(LINES-2, 0, "Line yanked");
    clrtoeol();
    refresh();
    napms(400);
}

static void paste(Editor* ed) {
    if (!ed->clipboard || ed->clip_len == 0) return;
    PT_Edit edit = {0};
    size_t pos = get_current_byte_pos(ed);
    pt_insert_at_byte(ed->pt, pos, ed->clipboard, ed->clip_len, &edit);
    move_to_byte_pos(ed, pos + ed->clip_len);
}

static void handle_normal_mode(Editor* ed, int ch) {
    PT_Edit edit = {0};
    switch (ch) {
        case 'i': ed->insert_mode = true; break;
        case 'A':
            {
                size_t line_len = pt_line_length(ed->pt, (size_t)ed->cursor_line);
                ed->cursor_col = (line_len > 0) ? (int)line_len - 1 : 0;
                ed->insert_mode = true;
            }
            break;
        case 'o':
            pt_insert_line(ed->pt, (size_t)ed->cursor_line + 1, "", 0, &edit);
            ed->cursor_line++;
            ed->cursor_col = 0;
            ed->insert_mode = true;
            break;
        case 'O':
            pt_insert_line(ed->pt, (size_t)ed->cursor_line, "", 0, &edit);
            ed->cursor_col = 0;
            ed->insert_mode = true;
            break;
        case 'y':
            if (getch() == 'y') yank_line(ed);
            break;
        case 'd':
            {
                int next = getch();
                if (next == 'd') {
                    yank_line(ed);
                    pt_delete_line(ed->pt, (size_t)ed->cursor_line, &edit);
                    size_t total_after = pt_line_count(ed->pt);
                    if (ed->cursor_line >= (int)total_after)
                        ed->cursor_line = total_after > 0 ? (int)total_after - 1 : 0;
                    ed->cursor_col = 0;
                } else if (next == 'w') {
                    delete_word(ed);
                } else ungetch(next);
            }
            scroll_to_cursor(ed);
            break;
        case 'p': paste(ed); break;
        case 'u':
            pt_undo(ed->pt, &edit);
            if (ed->cursor_line >= (int)pt_line_count(ed->pt)) {
                ed->cursor_line = (int)pt_line_count(ed->pt) - 1;
                if (ed->cursor_line < 0) ed->cursor_line = 0;
            }
            ed->cursor_col = 0;
            scroll_to_cursor(ed);
            break;
        case 'g': if (getch() == 'g') goto_line(ed, 0); break;
        case 'G': goto_line(ed, (int)pt_line_count(ed->pt) - 1); break;
        case 'w': move_word_forward(ed); break;
        case 'b': move_word_backward(ed); break;
        case '^': case '0': ed->cursor_col = 0; break;
        case '$':
            {
                size_t line_len = pt_line_length(ed->pt, (size_t)ed->cursor_line);
                ed->cursor_col = (line_len > 0) ? (int)line_len - 1 : 0;
            }
            break;
        case 'h': case KEY_LEFT:  move_cursor(ed, 0, -1); break;
        case 'l': case KEY_RIGHT: move_cursor(ed, 0, 1); break;
        case 'j': case KEY_DOWN:  move_cursor(ed, 1, 0); break;
        case 'k': case KEY_UP:    move_cursor(ed, -1, 0); break;
        case KEY_PPAGE: case 2:   move_cursor(ed, -(LINES - 2), 0); break;
        case KEY_NPAGE: case 6:   move_cursor(ed, LINES - 2, 0); break;
        case 'x':
            if (pt_line_length(ed->pt, (size_t)ed->cursor_line) > 0) {
                size_t byte_pos = get_current_byte_pos(ed);
                pt_delete_byte_range(ed->pt, byte_pos, byte_pos + 1, &edit);
            }
            break;
        case ':':
            {
                char cmd[128] = {0};
                mvprintw(LINES-2, 0, ":");
                clrtoeol();
                echo();
                getnstr(cmd, sizeof(cmd)-1);
                noecho();
                size_t len = strlen(cmd);
                while (len > 0 && isspace((unsigned char)cmd[len-1])) cmd[--len] = '\0';
                if (len > 0 && isdigit((unsigned char)cmd[0])) {
                    goto_line(ed, atoi(cmd) - 1);
                } else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "wq") == 0) {
                    if (pt_save_file(ed->pt, ed->filename, PT_EOL_PRESERVE)) {
                        mvprintw(LINES-2, 0, "File saved.");
                    } else mvprintw(LINES-2, 0, "Save failed!");
                    clrtoeol(); refresh(); napms(800);
                    if (strcmp(cmd, "wq") == 0) ed->running = false;
                } else if (strcmp(cmd, "q!") == 0) {
                    ed->running = false;
                } else if (strcmp(cmd, "q") == 0) {
                    if (!pt_is_modified(ed->pt)) ed->running = false;
                    else {
                        mvprintw(LINES-2, 0, "File has unsaved changes. Use :q! to force quit.");
                        clrtoeol(); refresh(); napms(1500);
                    }
                }
            }
            break;
        case 'q':
            if (!pt_is_modified(ed->pt)) ed->running = false;
            else {
                mvprintw(LINES-2, 0, "File has unsaved changes. Use :q! to force quit.");
                clrtoeol(); refresh(); napms(1200);
            }
            break;
    }
}

static void handle_insert_mode(Editor* ed, int ch) {
    PT_Edit edit = {0};
    if (ch == 27) {  /* ESC */
        ed->insert_mode = false;
        return;
    }
    if (ch == '\n' || ch == KEY_ENTER) {
        pt_insert_line(ed->pt, (size_t)ed->cursor_line + 1, "", 0, &edit);
        ed->cursor_line++;
        ed->cursor_col = 0;
    }
    else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (ed->cursor_col > 0) {
            size_t byte_pos = get_current_byte_pos(ed);
            pt_delete_byte_range(ed->pt, byte_pos - 1, byte_pos, &edit);
            ed->cursor_col--;
        }
    }
    else if (ch >= 32 && ch < 127) {
        char c = (char)ch;
        size_t byte_pos = get_current_byte_pos(ed);
        pt_insert_at_byte(ed->pt, byte_pos, &c, 1, &edit);
        ed->cursor_col++;
    }
    else if (ch == 'p' || ch == 'P') {
        paste(ed);
    }
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");

    Editor ed;
    editor_init(&ed, argc > 1 ? argv[1] : NULL);

    initscr();
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_BLUE,   COLOR_BLACK);   /* keywords */
        init_pair(2, COLOR_GREEN,  COLOR_BLACK);   /* strings */
        init_pair(3, COLOR_CYAN,   COLOR_BLACK);   /* comments */
    }
    raw();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(1);

    while (ed.running) {
        redraw(&ed);
        int ch = getch();

        if (ed.insert_mode) {
            handle_insert_mode(&ed, ch);
        } else {
            handle_normal_mode(&ed, ch);
        }
    }

    endwin();
    editor_free(&ed);
    return 0;
}
