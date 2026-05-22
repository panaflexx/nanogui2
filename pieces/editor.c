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
}

static void editor_free(Editor* ed) {
    if (ed->pt) pt_free(ed->pt);
    free(ed->clipboard);
    ed->pt = NULL;
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
        mvprintw(i, 0, "%s", buf);
        clrtoeol();
    }

    for (int i = (int)(total_lines - (size_t)ed->screen_top); i < max_lines; ++i) {
        mvprintw(i, 0, "");
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

static void move_word_forward(Editor* ed) {
    size_t pos = get_current_byte_pos(ed);
    size_t len = pt_byte_length(ed->pt);
    if (pos >= len) return;

    char buf[1];

    /* Skip to end of current word */
    bool in_word = false;
    while (pos < len) {
        pt_read(ed->pt, pos, buf, 1);
        if (is_word_char(buf[0])) {
            in_word = true;
        } else if (in_word) {
            break;
        }
        pos++;
    }

    /* Skip whitespace */
    while (pos < len) {
        pt_read(ed->pt, pos, buf, 1);
        if (!isspace((unsigned char)buf[0])) break;
        pos++;
    }

    /* Find start of next word */
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

static void move_word_backward(Editor* ed) {
    size_t pos = get_current_byte_pos(ed);
    if (pos == 0) return;

    char buf[1];
    pos--;

    /* Skip whitespace backward */
    while (pos > 0) {
        pt_read(ed->pt, pos, buf, 1);
        if (!isspace((unsigned char)buf[0])) break;
        pos--;
    }

    /* Go to start of word */
    bool in_word = false;
    while (pos > 0) {
        pt_read(ed->pt, pos, buf, 1);
        if (is_word_char(buf[0])) {
            in_word = true;
        } else if (in_word) {
            pos++;
            break;
        }
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

    if (pos > start) {
        pt_delete_byte_range(ed->pt, start, pos, &edit);
    }
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

/* ====================== Yank & Paste ====================== */

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

/* ====================== Command handling ====================== */

static void handle_normal_mode(Editor* ed, int ch) {
    PT_Edit edit = {0};

    switch (ch) {
        case 'i':
            ed->insert_mode = true;
            break;

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
            {
                int next = getch();
                if (next == 'y') {
                    yank_line(ed);
                } else {
                    ungetch(next);
                }
            }
            break;

        case 'd':
            {
                int next = getch();
                if (next == 'd') {
                    yank_line(ed);                    /* NEW: yank before delete */
                    pt_delete_line(ed->pt, (size_t)ed->cursor_line, &edit);
                    size_t total_after = pt_line_count(ed->pt);
                    if (ed->cursor_line >= (int)total_after)
                        ed->cursor_line = total_after > 0 ? (int)total_after - 1 : 0;
                    ed->cursor_col = 0;
                } else if (next == 'w') {
                    delete_word(ed);
                } else {
                    ungetch(next);
                }
            }
            scroll_to_cursor(ed);
            break;

        case 'p':
            paste(ed);
            break;

        case 'u':
            pt_undo(ed->pt, &edit);
            if (ed->cursor_line >= (int)pt_line_count(ed->pt)) {
                ed->cursor_line = (int)pt_line_count(ed->pt) - 1;
                if (ed->cursor_line < 0) ed->cursor_line = 0;
            }
            ed->cursor_col = 0;
            scroll_to_cursor(ed);
            break;

        case 'g':
            {
                int next = getch();
                if (next == 'g') goto_line(ed, 0);
                else ungetch(next);
            }
            break;

        case 'G':
            goto_line(ed, (int)pt_line_count(ed->pt) - 1);
            break;

        case 'w':
            move_word_forward(ed);
            break;

        case 'b':
            move_word_backward(ed);
            break;

        case '^':
        case '0':
            ed->cursor_col = 0;
            break;

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
                    int line_num = atoi(cmd);
                    goto_line(ed, line_num - 1);
                } 
                else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "wq") == 0) {
                    if (pt_save_file(ed->pt, ed->filename, PT_EOL_PRESERVE)) {
                        mvprintw(LINES-2, 0, "File saved.");
                    } else {
                        mvprintw(LINES-2, 0, "Save failed!");
                    }
                    clrtoeol();
                    refresh();
                    napms(800);
                    if (strcmp(cmd, "wq") == 0) {
                        ed->running = false;
                    }
                }
                else if (strcmp(cmd, "q!") == 0) {
                    ed->running = false;
                }
                else if (strcmp(cmd, "q") == 0) {
                    if (!pt_is_modified(ed->pt)) {
                        ed->running = false;
                    } else {
                        mvprintw(LINES-2, 0, "File has unsaved changes. Use :q! to force quit.");
                        clrtoeol();
                        refresh();
                        napms(1500);
                    }
                }
            }
            break;

        case 'q':
            if (!pt_is_modified(ed->pt)) ed->running = false;
            else {
                mvprintw(LINES-2, 0, "File has unsaved changes. Use :q! to force quit.");
                clrtoeol();
                refresh();
                napms(1200);
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
    else if (ch == 'p' || ch == 'P') {  /* paste in insert mode too */
        paste(ed);
    }
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");

    Editor ed;
    editor_init(&ed, argc > 1 ? argv[1] : NULL);

    initscr();
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
