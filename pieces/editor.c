#include "piece_buf/piece_table.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include "tree-sitter/lib/include/tree_sitter/api.h"

/* ====================== Types ====================== */

typedef enum {
    LANG_C, LANG_CPP, LANG_PYTHON, LANG_GO, LANG_JS, LANG_JSON, LANG_UNKNOWN
} LanguageType;

typedef struct {
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t start_line;
    uint32_t start_col;
    uint32_t end_line;
    uint32_t end_col;
    char     message[128];
    bool     is_error;     /* true = error, false = warning/missing */
} Diagnostic;

typedef struct {
    PieceTable* pt;
    int cursor_line;
    int cursor_col;
    int screen_top;
    char filename[256];
    bool insert_mode;
    bool running;

    /* Clipboard */
    char*  clipboard;
    size_t clip_len;
    bool   clip_is_line;   /* true if yanked with yy / dd */

    /* Tree-sitter */
    TSParser*           parser;
    TSTree*             tree;
    const TSLanguage*   language;
    LanguageType        lang;

    /* Diagnostics */
    Diagnostic* diags;
    size_t      n_diags;
    size_t      cap_diags;

    /* Indentation */
    int  tab_width;
    bool expand_tab;       /* true => spaces, false => '\t' */

    /* Transient status message */
    char status_msg[256];
    int  status_msg_ticks;
} Editor;

/* ====================== Forward decls ====================== */

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

static void move_cursor(Editor* ed, int dline, int dcol);
static void yank_line(Editor* ed);
static void paste_linewise(Editor* ed, bool after);
static void paste_charwise(Editor* ed, bool after);
static void handle_normal_mode(Editor* ed, int ch);
static void handle_insert_mode(Editor* ed, int ch);
static void collect_diagnostics(Editor* ed);
static const Diagnostic* diagnostic_at_byte(const Editor* ed, size_t byte);
static void set_status_msg(Editor* ed, const char* fmt, ...);

/* ====================== Language Detection ====================== */

static LanguageType detect_language(const char* filename) {
    if (!filename) return LANG_UNKNOWN;
    const char* ext = strrchr(filename, '.');
    if (!ext) return LANG_UNKNOWN;
    ext++;

    if (strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0) return LANG_C;
    if (strcmp(ext, "cpp") == 0 || strcmp(ext, "cc") == 0 || strcmp(ext, "cxx") == 0 || strcmp(ext, "hpp") == 0) return LANG_CPP;
    if (strcmp(ext, "py") == 0) return LANG_PYTHON;
    if (strcmp(ext, "go") == 0) return LANG_GO;
    if (strcmp(ext, "js") == 0 || strcmp(ext, "jsx") == 0 || strcmp(ext, "ts") == 0 || strcmp(ext, "tsx") == 0) return LANG_JS;
    if (strcmp(ext, "json") == 0) return LANG_JSON;
    return LANG_UNKNOWN;
}

static const char* language_name(LanguageType lt) {
    switch (lt) {
        case LANG_C: return "c";
        case LANG_CPP: return "cpp";
        case LANG_PYTHON: return "py";
        case LANG_GO: return "go";
        case LANG_JS: return "js";
        case LANG_JSON: return "json";
        default: return "txt";
    }
}

/* ====================== Tree-sitter Reparse + Diagnostics ====================== */

static void reparse_document(Editor* ed) {
    if (!ed->parser || !ed->language) return;

    size_t len = pt_byte_length(ed->pt);
    char* content = pt_dup_byte_range(ed->pt, 0, len, NULL);
    if (!content) return;

    if (ed->tree) ts_tree_delete(ed->tree);
    ed->tree = ts_parser_parse_string(ed->parser, NULL, content, (uint32_t)len);
    free(content);

    collect_diagnostics(ed);
}

static void update_tree_after_edit(Editor* ed, const PT_Edit* edit) {
    if (!ed->tree || !edit) return;

    TSInputEdit ts_edit = {
        .start_byte    = (uint32_t)edit->start_byte,
        .old_end_byte  = (uint32_t)edit->old_end_byte,
        .new_end_byte  = (uint32_t)edit->new_end_byte,
        .start_point   = { edit->start_point.line,   edit->start_point.column },
        .old_end_point = { edit->old_end_point.line, edit->old_end_point.column },
        .new_end_point = { edit->new_end_point.line, edit->new_end_point.column }
    };

    ts_tree_edit(ed->tree, &ts_edit);
    reparse_document(ed);  /* full reparse (fast enough for now) */
}

static void diags_push(Editor* ed, const Diagnostic* d) {
    if (ed->n_diags == ed->cap_diags) {
        size_t nc = ed->cap_diags ? ed->cap_diags * 2 : 32;
        Diagnostic* nb = (Diagnostic*)realloc(ed->diags, nc * sizeof(Diagnostic));
        if (!nb) return;
        ed->diags = nb;
        ed->cap_diags = nc;
    }
    ed->diags[ed->n_diags++] = *d;
}

/* Walk parse tree, collecting ERROR & MISSING nodes as diagnostics. */
static void collect_diagnostics(Editor* ed) {
    ed->n_diags = 0;
    if (!ed->tree) return;

    TSNode root = ts_tree_root_node(ed->tree);
    if (ts_node_is_null(root) || !ts_node_has_error(root)) return;

    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool descend = true;

    while (1) {
        TSNode n = ts_tree_cursor_current_node(&cursor);

        bool is_err   = ts_node_is_error(n);
        bool is_miss  = ts_node_is_missing(n);

        if (is_err || is_miss) {
            Diagnostic d;
            d.start_byte = ts_node_start_byte(n);
            d.end_byte   = ts_node_end_byte(n);
            if (d.end_byte == d.start_byte) d.end_byte = d.start_byte + 1; /* zero-width missing nodes */
            TSPoint sp   = ts_node_start_point(n);
            TSPoint ep   = ts_node_end_point(n);
            d.start_line = sp.row;
            d.start_col  = sp.column;
            d.end_line   = ep.row;
            d.end_col    = ep.column;
            d.is_error   = is_err;

            if (is_miss) {
                const char* t = ts_node_type(n);
                snprintf(d.message, sizeof(d.message),
                         "missing '%s'", t ? t : "?");
            } else {
                /* For ERROR node, surface the parent context */
                TSNode parent = ts_node_parent(n);
                const char* pt_type = (!ts_node_is_null(parent)) ? ts_node_type(parent) : "?";
                snprintf(d.message, sizeof(d.message),
                         "syntax error in %s", pt_type ? pt_type : "?");
            }
            diags_push(ed, &d);

            /* don't descend into pure error subtrees (avoid noisy duplicates) */
            descend = !is_err;
        } else {
            descend = ts_node_has_error(n);  /* only descend where error lives */
        }

        if (descend && ts_tree_cursor_goto_first_child(&cursor)) {
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                ts_tree_cursor_delete(&cursor);
                return;
            }
        }
    }
}

static const Diagnostic* diagnostic_at_byte(const Editor* ed, size_t byte) {
    for (size_t i = 0; i < ed->n_diags; ++i) {
        if (byte >= ed->diags[i].start_byte && byte < ed->diags[i].end_byte) {
            return &ed->diags[i];
        }
    }
    return NULL;
}

/* ====================== Syntax Highlighting ====================== */

/* Color pair indices (defined in main) */
enum {
    CP_DEFAULT    = 0,
    CP_IDENTIFIER = 1,
    CP_STRING     = 2,
    CP_COMMENT    = 3,
    CP_KEYWORD    = 4,
    CP_NUMBER     = 5,
    CP_ERROR      = 6,
    CP_TYPE       = 7,
    CP_PREPROC    = 8,
};

static int color_for_node_type(const char* type) {
    if (!type) return CP_DEFAULT;

    /* strings */
    if (strstr(type, "string") || strstr(type, "char_literal")) return CP_STRING;
    /* comments */
    if (strstr(type, "comment")) return CP_COMMENT;
    /* preprocessor */
    if (strncmp(type, "preproc", 7) == 0 ||
        strncmp(type, "#",       1) == 0) return CP_PREPROC;
    /* numbers */
    if (strstr(type, "number") ||
        strcmp(type, "integer") == 0 ||
        strcmp(type, "float")   == 0 ||
        strcmp(type, "true") == 0 || strcmp(type, "false") == 0 ||
        strcmp(type, "null") == 0) return CP_NUMBER;
    /* types */
    if (strstr(type, "primitive_type") ||
        strstr(type, "type_identifier") ||
        strcmp(type, "type") == 0) return CP_TYPE;
    /* keywords / operators */
    if (strstr(type, "keyword") ||
        strstr(type, "operator") ||
        strcmp(type, "if") == 0 || strcmp(type, "else") == 0 ||
        strcmp(type, "for") == 0 || strcmp(type, "while") == 0 ||
        strcmp(type, "return") == 0 || strcmp(type, "break") == 0 ||
        strcmp(type, "continue") == 0 || strcmp(type, "switch") == 0 ||
        strcmp(type, "case") == 0 || strcmp(type, "default") == 0 ||
        strcmp(type, "struct") == 0 || strcmp(type, "enum") == 0 ||
        strcmp(type, "union") == 0 || strcmp(type, "typedef") == 0 ||
        strcmp(type, "static") == 0 || strcmp(type, "const") == 0 ||
        strcmp(type, "extern") == 0 || strcmp(type, "void") == 0 ||
        strcmp(type, "func") == 0 || strcmp(type, "package") == 0 ||
        strcmp(type, "import") == 0 || strcmp(type, "var") == 0 ||
        strcmp(type, "def") == 0 || strcmp(type, "class") == 0 ||
        strcmp(type, "function") == 0 || strcmp(type, "let") == 0 ||
        strcmp(type, "if_statement") == 0) return CP_KEYWORD;
    /* identifiers / functions / properties */
    if (strstr(type, "identifier") ||
        strstr(type, "function")   ||
        strstr(type, "field")      ||
        strstr(type, "property")) return CP_IDENTIFIER;

    return CP_DEFAULT;
}

/* For each column in the visible line text, decide the color attribute. */
static void build_line_color_map(Editor* ed,
                                 size_t line_start_byte,
                                 const char* line,
                                 size_t line_len,
                                 int* out_colors)
{
    /* default everything */
    for (size_t i = 0; i < line_len; ++i) out_colors[i] = CP_DEFAULT;

    if (!ed->tree || line_len == 0) return;
    TSNode root = ts_tree_root_node(ed->tree);
    if (ts_node_is_null(root)) return;

    for (size_t i = 0; i < line_len; ++i) {
        (void)line;
        TSNode leaf = ts_node_named_descendant_for_byte_range(
            root,
            (uint32_t)(line_start_byte + i),
            (uint32_t)(line_start_byte + i + 1));
        if (!ts_node_is_null(leaf)) {
            const char* type = ts_node_type(leaf);
            out_colors[i] = color_for_node_type(type);
        }
    }
}

/* Render a single line with highlight + diagnostic overlay. */
static void print_highlighted(Editor* ed, const char* line, int y,
                              size_t line_start_byte, size_t line_len)
{
    if (!line) return;
    if (line_len == 0) return;

    /* Strip trailing newline before drawing */
    while (line_len > 0 && (line[line_len-1] == '\n' || line[line_len-1] == '\r')) {
        line_len--;
    }

    if (line_len == 0) return;

    /* fallback (no tree): just print plain */
    if (!ed->tree) {
        mvaddnstr(y, 0, line, (int)line_len);
        return;
    }

    int* colors = (int*)calloc(line_len, sizeof(int));
    if (!colors) {
        mvaddnstr(y, 0, line, (int)line_len);
        return;
    }
    build_line_color_map(ed, line_start_byte, line, line_len, colors);

    int last_attr = -1;
    for (size_t i = 0; i < line_len; ++i) {
        size_t b = line_start_byte + i;
        int attr;
        const Diagnostic* d = diagnostic_at_byte(ed, b);
        if (d) {
            attr = COLOR_PAIR(CP_ERROR) | A_UNDERLINE
                 | (d->is_error ? A_BOLD : 0);
        } else {
            attr = COLOR_PAIR(colors[i]);
        }

        if (attr != last_attr) {
            if (last_attr != -1) attroff(last_attr);
            attron(attr);
            last_attr = attr;
        }
        mvaddch(y, (int)i, (unsigned char)line[i]);
    }
    if (last_attr != -1) attroff(last_attr);
    attrset(A_NORMAL);

    free(colors);
}

/* ====================== Load Tree-sitter Grammar ====================== */

static void load_language(Editor* ed) {
    if (!ed->parser) return;

    ed->lang = detect_language(ed->filename);
    const char* libname = NULL;
    const char* symbol  = NULL;

    switch (ed->lang) {
        case LANG_C:      libname = "./lib/libtree-sitter-c.so";          symbol = "tree_sitter_c"; break;
        case LANG_CPP:    libname = "./lib/libtree-sitter-cpp.so";        symbol = "tree_sitter_cpp"; break;
        case LANG_PYTHON: libname = "./lib/libtree-sitter-python.so";     symbol = "tree_sitter_python"; break;
        case LANG_GO:     libname = "./lib/libtree-sitter-go.so";         symbol = "tree_sitter_go"; break;
        case LANG_JS:     libname = "./lib/libtree-sitter-javascript.so"; symbol = "tree_sitter_javascript"; break;
        case LANG_JSON:   libname = "./lib/libtree-sitter-json.so";       symbol = "tree_sitter_json"; break;
        default: return;
    }

    void* handle = dlopen(libname, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Warning: Failed to load %s: %s\n", libname, dlerror());
        return;
    }

    typedef const TSLanguage* (*GetLang)(void);
    GetLang func = (GetLang)dlsym(handle, symbol);
    if (func) {
        ed->language = func();
        ts_parser_set_language(ed->parser, ed->language);
    }
}

/* ====================== Editor lifecycle ====================== */

static void editor_init(Editor* ed, const char* filename) {
    memset(ed, 0, sizeof(Editor));
    ed->pt = filename && *filename ? pt_new_from_file(filename) : pt_new();
    if (!ed->pt) ed->pt = pt_new();

    strncpy(ed->filename, filename && *filename ? filename : "untitled.txt", sizeof(ed->filename)-1);

    ed->cursor_line = 0;
    ed->cursor_col  = 0;
    ed->screen_top  = 0;
    ed->insert_mode = false;
    ed->running     = true;
    ed->clipboard   = NULL;
    ed->clip_len    = 0;
    ed->clip_is_line= false;

    ed->parser   = ts_parser_new();
    ed->tree     = NULL;
    ed->language = NULL;
    ed->lang     = LANG_UNKNOWN;

    ed->diags     = NULL;
    ed->n_diags   = 0;
    ed->cap_diags = 0;

    ed->tab_width  = 4;
    ed->expand_tab = true;

    ed->status_msg[0]    = '\0';
    ed->status_msg_ticks = 0;

    load_language(ed);
    reparse_document(ed);
}

static void editor_free(Editor* ed) {
    if (ed->pt) pt_free(ed->pt);
    if (ed->parser) ts_parser_delete(ed->parser);
    if (ed->tree) ts_tree_delete(ed->tree);
    free(ed->clipboard);
    free(ed->diags);
}

/* ====================== Status line ====================== */

#include <stdarg.h>
static void set_status_msg(Editor* ed, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->status_msg, sizeof(ed->status_msg), fmt, ap);
    va_end(ap);
    ed->status_msg_ticks = 30; /* keep around for ~30 keypresses */
}

static void draw_status(const Editor* ed) {
    int row = LINES - 1;

    /* Line 2 from bottom: diagnostic / status message */
    int msg_row = LINES - 2;
    move(msg_row, 0);
    clrtoeol();

    /* If cursor is on a diagnostic, show its message */
    size_t cur_byte = pt_point_to_byte(ed->pt,
        (PT_Point){(uint32_t)ed->cursor_line, (uint32_t)ed->cursor_col});
    const Diagnostic* dc = diagnostic_at_byte(ed, cur_byte);
    if (dc) {
        attron(COLOR_PAIR(CP_ERROR) | A_BOLD);
        mvprintw(msg_row, 0, "%s: %s",
                 dc->is_error ? "error" : "warn", dc->message);
        attroff(COLOR_PAIR(CP_ERROR) | A_BOLD);
    } else if (ed->status_msg[0]) {
        mvprintw(msg_row, 0, "%s", ed->status_msg);
    }

    /* Bottom row: status */
    attron(A_REVERSE);
    mvprintw(row, 0, " %s%s | %s | %d/%zu | Col %d | diags:%zu | %s ",
             ed->filename,
             pt_is_modified(ed->pt) ? " [+]" : "",
             language_name(ed->lang),
             ed->cursor_line + 1,
             pt_line_count(ed->pt),
             ed->cursor_col,
             ed->n_diags,
             ed->insert_mode ? "-- INSERT --" : "-- NORMAL --");
    clrtoeol();
    attroff(A_REVERSE);
}

/* ====================== Redraw ====================== */

static void redraw(Editor* ed) {
    werase(stdscr);

    int max_lines = LINES - 2;   /* reserve last 2 rows for status */
    size_t total_lines = pt_line_count(ed->pt);

    for (int i = 0; i < max_lines && (size_t)(ed->screen_top + i) < total_lines; ++i) {
        size_t line_idx = (size_t)ed->screen_top + i;
        char buf[4096] = {0};
        size_t line_start = pt_line_to_byte(ed->pt, line_idx);
        size_t got = pt_get_line(ed->pt, line_idx, buf, sizeof(buf)-1);
        print_highlighted(ed, buf, i, line_start, got);
        clrtoeol();
    }

    for (int i = (int)(total_lines - (size_t)ed->screen_top); i < max_lines; ++i) {
        if (i < 0) continue;
        attron(COLOR_PAIR(CP_COMMENT));
        mvprintw(i, 0, "~");
        attroff(COLOR_PAIR(CP_COMMENT));
        clrtoeol();
    }

    draw_status(ed);

    if (ed->status_msg_ticks > 0) ed->status_msg_ticks--;
    if (ed->status_msg_ticks == 0) ed->status_msg[0] = '\0';

    int screen_y = ed->cursor_line - ed->screen_top;
    if (screen_y >= 0 && screen_y < max_lines) {
        size_t ll = pt_line_length(ed->pt, (size_t)ed->cursor_line);
        /* strip trailing newline from displayed length */
        if (ll > 0) {
            char tail[2] = {0};
            pt_read(ed->pt, pt_line_to_byte(ed->pt, ed->cursor_line) + ll - 1, tail, 1);
            if (tail[0] == '\n') ll--;
        }
        int display_col = (ed->cursor_col > (int)ll) ? (int)ll : ed->cursor_col;
        move(screen_y, display_col);
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
    ed->cursor_col  = 0;
    scroll_to_cursor(ed);
}

static size_t get_current_byte_pos(const Editor* ed) {
    return pt_point_to_byte(ed->pt,
        (PT_Point){(uint32_t)ed->cursor_line, (uint32_t)ed->cursor_col});
}

static void move_to_byte_pos(Editor* ed, size_t byte_pos) {
    PT_Point p = pt_byte_to_point(ed->pt, byte_pos);
    ed->cursor_line = (int)p.line;
    ed->cursor_col  = (int)p.column;
    scroll_to_cursor(ed);
}

static bool is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* ====================== Motion byte helpers ====================== */
/* These return the byte offset reached, without moving the cursor.
 * Used by both motion commands and operator+motion (e.g. `d3w`).      */

static size_t motion_w_byte(Editor* ed, size_t pos, int n) {
    size_t len = pt_byte_length(ed->pt);
    char c;
    for (int k = 0; k < n; k++) {
        if (pos >= len) break;
        pt_read(ed->pt, pos, &c, 1);
        if (is_word_char(c)) {
            while (pos < len) {
                pt_read(ed->pt, pos, &c, 1);
                if (!is_word_char(c)) break;
                pos++;
            }
        } else if (!isspace((unsigned char)c)) {
            /* punctuation cluster */
            while (pos < len) {
                pt_read(ed->pt, pos, &c, 1);
                if (is_word_char(c) || isspace((unsigned char)c)) break;
                pos++;
            }
        }
        while (pos < len) {
            pt_read(ed->pt, pos, &c, 1);
            if (!isspace((unsigned char)c)) break;
            pos++;
        }
    }
    return pos;
}

static size_t motion_b_byte(Editor* ed, size_t pos, int n) {
    char c;
    for (int k = 0; k < n; k++) {
        if (pos == 0) break;
        pos--;
        /* skip whitespace */
        while (pos > 0) {
            pt_read(ed->pt, pos, &c, 1);
            if (!isspace((unsigned char)c)) break;
            pos--;
        }
        pt_read(ed->pt, pos, &c, 1);
        if (isspace((unsigned char)c)) break;
        bool word = is_word_char(c);
        while (pos > 0) {
            char p;
            pt_read(ed->pt, pos - 1, &p, 1);
            if (isspace((unsigned char)p)) break;
            if (is_word_char(p) != word) break;
            pos--;
        }
    }
    return pos;
}

static size_t motion_e_byte(Editor* ed, size_t pos, int n) {
    size_t len = pt_byte_length(ed->pt);
    char c;
    for (int k = 0; k < n; k++) {
        if (pos + 1 >= len) { pos = len; break; }
        pos++;
        /* skip whitespace */
        while (pos < len) {
            pt_read(ed->pt, pos, &c, 1);
            if (!isspace((unsigned char)c)) break;
            pos++;
        }
        if (pos >= len) break;
        pt_read(ed->pt, pos, &c, 1);
        bool word = is_word_char(c);
        while (pos + 1 < len) {
            char nx;
            pt_read(ed->pt, pos + 1, &nx, 1);
            if (isspace((unsigned char)nx)) break;
            if (is_word_char(nx) != word) break;
            pos++;
        }
    }
    return pos;
}

static size_t line_end_no_nl(Editor* ed, int line) {
    if (line < 0) return 0;
    if (line >= (int)pt_line_count(ed->pt)) return pt_byte_length(ed->pt);
    size_t lb = pt_line_to_byte(ed->pt, line);
    size_t ll = pt_line_length(ed->pt, line);
    if (ll > 0) {
        char t = 0;
        pt_read(ed->pt, lb + ll - 1, &t, 1);
        if (t == '\n') ll--;
    }
    return lb + ll;
}

static int first_nonblank_col(Editor* ed, int line) {
    char buf[4096] = {0};
    pt_get_line(ed->pt, line, buf, sizeof(buf)-1);
    int i = 0;
    while (buf[i] == ' ' || buf[i] == '\t') i++;
    return i;
}

static size_t motion_l_byte(Editor* ed, size_t pos, int n) {
    PT_Point p = pt_byte_to_point(ed->pt, pos);
    size_t lend = line_end_no_nl(ed, p.line);
    size_t want = pos + (size_t)n;
    return want > lend ? lend : want;
}

static size_t motion_h_byte(Editor* ed, size_t pos, int n) {
    PT_Point p = pt_byte_to_point(ed->pt, pos);
    size_t lb = pt_line_to_byte(ed->pt, p.line);
    size_t want = (pos > (size_t)n) ? pos - (size_t)n : 0;
    return want < lb ? lb : want;
}

static void move_word_forward(Editor* ed) {
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

static void move_word_backward(Editor* ed) {
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

static void move_cursor(Editor* ed, int dline, int dcol) {
    size_t total_lines = pt_line_count(ed->pt);
    if (total_lines == 0) return;

    long new_line = (long)ed->cursor_line + dline;
    if (new_line < 0) new_line = 0;
    if (new_line >= (long)total_lines) new_line = (long)total_lines - 1;
    ed->cursor_line = (int)new_line;

    size_t line_len = pt_line_length(ed->pt, (size_t)ed->cursor_line);
    /* strip trailing newline */
    if (line_len > 0) {
        size_t lb = pt_line_to_byte(ed->pt, ed->cursor_line);
        char tail[1] = {0};
        pt_read(ed->pt, lb + line_len - 1, tail, 1);
        if (tail[0] == '\n') line_len--;
    }
    long new_col = (long)ed->cursor_col + dcol;
    if (new_col < 0) new_col = 0;
    if (new_col > (long)line_len) new_col = (long)line_len;
    ed->cursor_col = (int)new_col;

    scroll_to_cursor(ed);
}

/* ====================== Yank / Paste ====================== */

static void yank_line(Editor* ed) {
    free(ed->clipboard);
    ed->clipboard = pt_dup_line(ed->pt, (size_t)ed->cursor_line);
    ed->clip_len  = ed->clipboard ? strlen(ed->clipboard) : 0;
    /* ensure trailing newline (for line paste semantics) */
    if (ed->clipboard && (ed->clip_len == 0 || ed->clipboard[ed->clip_len-1] != '\n')) {
        char* nb = (char*)realloc(ed->clipboard, ed->clip_len + 2);
        if (nb) {
            ed->clipboard = nb;
            ed->clipboard[ed->clip_len++] = '\n';
            ed->clipboard[ed->clip_len]   = '\0';
        }
    }
    ed->clip_is_line = true;
    set_status_msg(ed, "1 line yanked");
}

/* Linewise paste: 'p' (below) / 'P' (above) when clip_is_line */
static void paste_linewise(Editor* ed, bool below) {
    if (!ed->clipboard || ed->clip_len == 0) return;
    PT_Edit edit = {0};

    /* Strip trailing newline for pt_insert_line which appends one */
    size_t text_len = ed->clip_len;
    while (text_len > 0 && ed->clipboard[text_len - 1] == '\n') text_len--;

    size_t insert_line = below
        ? (size_t)ed->cursor_line + 1
        : (size_t)ed->cursor_line;

    pt_insert_line(ed->pt, insert_line, ed->clipboard, text_len, &edit);
    update_tree_after_edit(ed, &edit);

    ed->cursor_line = (int)insert_line;
    ed->cursor_col  = 0;
    scroll_to_cursor(ed);
}

/* Charwise paste: paste raw text starting at cursor (or after cursor when 'p'). */
static void paste_charwise(Editor* ed, bool after) {
    if (!ed->clipboard || ed->clip_len == 0) return;
    PT_Edit edit = {0};
    size_t pos = get_current_byte_pos(ed);

    if (after) {
        /* In vim's p, charwise paste places after current cursor */
        size_t line_end = pt_line_to_byte(ed->pt, ed->cursor_line)
                        + pt_line_length(ed->pt, ed->cursor_line);
        if (pos < line_end) pos++;
    }
    pt_insert_at_byte(ed->pt, pos, ed->clipboard, ed->clip_len, &edit);
    update_tree_after_edit(ed, &edit);
    move_to_byte_pos(ed, pos + ed->clip_len - 1);
}

/* ====================== Indentation helpers ====================== */

static void insert_indent(Editor* ed) {
    PT_Edit edit = {0};
    size_t pos = get_current_byte_pos(ed);
    if (ed->expand_tab) {
        int n = ed->tab_width - (ed->cursor_col % ed->tab_width);
        if (n <= 0) n = ed->tab_width;
        char sp[16];
        if (n > (int)sizeof(sp)) n = sizeof(sp);
        memset(sp, ' ', n);
        pt_insert_at_byte(ed->pt, pos, sp, n, &edit);
        update_tree_after_edit(ed, &edit);
        ed->cursor_col += n;
    } else {
        pt_insert_at_byte(ed->pt, pos, "\t", 1, &edit);
        update_tree_after_edit(ed, &edit);
        ed->cursor_col += 1;
    }
}

/* Indent / dedent the current line */
static void shift_line(Editor* ed, int dir) {
    PT_Edit edit = {0};
    size_t lb = pt_line_to_byte(ed->pt, ed->cursor_line);

    if (dir > 0) {
        if (ed->expand_tab) {
            char sp[16];
            int n = ed->tab_width;
            if (n > (int)sizeof(sp)) n = sizeof(sp);
            memset(sp, ' ', n);
            pt_insert_at_byte(ed->pt, lb, sp, n, &edit);
            update_tree_after_edit(ed, &edit);
            ed->cursor_col += n;
        } else {
            pt_insert_at_byte(ed->pt, lb, "\t", 1, &edit);
            update_tree_after_edit(ed, &edit);
            ed->cursor_col += 1;
        }
    } else {
        /* dedent: strip up to tab_width spaces or one '\t' at line start */
        size_t line_len = pt_line_length(ed->pt, ed->cursor_line);
        if (line_len == 0) return;
        char head[16] = {0};
        size_t to_read = line_len < sizeof(head) ? line_len : sizeof(head);
        pt_read(ed->pt, lb, head, to_read);

        size_t n = 0;
        if (head[0] == '\t') {
            n = 1;
        } else {
            while (n < (size_t)ed->tab_width && n < to_read && head[n] == ' ') n++;
        }
        if (n > 0) {
            pt_delete_byte_range(ed->pt, lb, lb + n, &edit);
            update_tree_after_edit(ed, &edit);
            ed->cursor_col = (ed->cursor_col >= (int)n) ? ed->cursor_col - (int)n : 0;
        }
    }
}

static void auto_indent_new_line(Editor* ed) {
    if (ed->cursor_line == 0) return;
    char prev[4096] = {0};
    pt_get_line(ed->pt, (size_t)ed->cursor_line - 1, prev, sizeof(prev)-1);

    int indent = 0;
    while (prev[indent] == ' ' || prev[indent] == '\t') indent++;

    if (indent > 0) {
        PT_Edit edit = {0};
        char* spaces = malloc(indent + 1);
        if (spaces) {
            memcpy(spaces, prev, indent);
            spaces[indent] = '\0';
            size_t pos = get_current_byte_pos(ed);
            pt_insert_at_byte(ed->pt, pos, spaces, indent, &edit);
            update_tree_after_edit(ed, &edit);
            free(spaces);
            ed->cursor_col = indent;
        }
    }
}

/* ====================== Language autopair config ====================== */

/* Returns the matching closing character for `open`, or 0 if none. */
static char autopair_close_for(LanguageType lt, char open) {
    switch (lt) {
        case LANG_C: case LANG_CPP: case LANG_GO: case LANG_JS:
            switch (open) {
                case '(': return ')';
                case '[': return ']';
                case '{': return '}';
                case '"': return '"';
                case '\'': return '\'';
            }
            break;
        case LANG_JSON:
            switch (open) {
                case '{': return '}';
                case '[': return ']';
                case '"': return '"';
            }
            break;
        case LANG_PYTHON:
            switch (open) {
                case '(': return ')';
                case '[': return ']';
                case '{': return '}';
                case '"': return '"';
                case '\'': return '\'';
            }
            break;
        default: break;
    }
    return 0;
}

static bool is_pair_close(char c) { return c==')' || c==']' || c=='}' || c=='"' || c=='\''; }
static char open_for_close(char c) {
    switch (c) { case ')': return '('; case ']': return '['; case '}': return '{';
                 case '"': return '"'; case '\'': return '\''; default: return 0; }
}

static char peek_char_at(Editor* ed, size_t bp) {
    if (bp >= pt_byte_length(ed->pt)) return 0;
    char c = 0;
    pt_read(ed->pt, bp, &c, 1);
    return c;
}

/* ====================== Operator + Motion (d, c, y) ====================== */

/* Stash a charwise range to the clipboard. */
static void clip_save_charwise(Editor* ed, size_t s, size_t e) {
    if (e <= s) return;
    free(ed->clipboard);
    ed->clip_len = e - s;
    ed->clipboard = (char*)malloc(ed->clip_len + 1);
    if (!ed->clipboard) { ed->clip_len = 0; return; }
    pt_read(ed->pt, s, ed->clipboard, ed->clip_len);
    ed->clipboard[ed->clip_len] = '\0';
    ed->clip_is_line = false;
}

/* Stash a linewise range [line_start, line_end_incl] to the clipboard. */
static void clip_save_linewise(Editor* ed, int ls, int le) {
    int tot = (int)pt_line_count(ed->pt);
    if (ls < 0) ls = 0;
    if (le >= tot) le = tot - 1;
    if (le < ls) return;
    size_t sb = pt_line_to_byte(ed->pt, ls);
    size_t eb = (le + 1 < tot) ? pt_line_to_byte(ed->pt, le + 1)
                               : pt_byte_length(ed->pt);
    free(ed->clipboard);
    ed->clip_len = eb - sb;
    ed->clipboard = (char*)malloc(ed->clip_len + 2);
    if (!ed->clipboard) { ed->clip_len = 0; return; }
    pt_read(ed->pt, sb, ed->clipboard, ed->clip_len);
    if (ed->clip_len == 0 || ed->clipboard[ed->clip_len - 1] != '\n') {
        ed->clipboard[ed->clip_len++] = '\n';
    }
    ed->clipboard[ed->clip_len] = '\0';
    ed->clip_is_line = true;
}

/* Delete `count` whole lines starting at `start_line`. */
static void delete_lines(Editor* ed, int start_line, int count) {
    int tot = (int)pt_line_count(ed->pt);
    int le = start_line + count - 1;
    if (le >= tot) le = tot - 1;
    if (le < start_line) return;
    clip_save_linewise(ed, start_line, le);
    for (int i = le; i >= start_line; --i) {
        PT_Edit e = {0};
        pt_delete_line(ed->pt, (size_t)i, &e);
        update_tree_after_edit(ed, &e);
    }
    int tot2 = (int)pt_line_count(ed->pt);
    ed->cursor_line = start_line;
    if (ed->cursor_line >= tot2) ed->cursor_line = tot2 > 0 ? tot2 - 1 : 0;
    if (ed->cursor_line < 0) ed->cursor_line = 0;
    ed->cursor_col = 0;
    scroll_to_cursor(ed);
}

/* Delete a charwise byte range [s, e). Saves to clipboard. */
static void delete_charwise(Editor* ed, size_t s, size_t e) {
    if (e <= s) return;
    clip_save_charwise(ed, s, e);
    PT_Edit edit = {0};
    pt_delete_byte_range(ed->pt, s, e, &edit);
    update_tree_after_edit(ed, &edit);
    move_to_byte_pos(ed, s);
}

/* Execute `d<motion>` with given (already-multiplied) count.
 * `motion_ch` is the second key after `d`. Returns true if handled. */
static bool do_delete_motion(Editor* ed, int motion_ch, int mcount) {
    if (mcount < 1) mcount = 1;
    size_t cur = get_current_byte_pos(ed);

    switch (motion_ch) {
        case 'w': {
            size_t e = motion_w_byte(ed, cur, mcount);
            delete_charwise(ed, cur, e);
            return true;
        }
        case 'e': {
            size_t e = motion_e_byte(ed, cur, mcount);
            /* dE / de is inclusive of the last char */
            if (e < pt_byte_length(ed->pt)) e++;
            delete_charwise(ed, cur, e);
            return true;
        }
        case 'b': {
            size_t s = motion_b_byte(ed, cur, mcount);
            delete_charwise(ed, s, cur);
            return true;
        }
        case 'l': case ' ': {
            size_t e = motion_l_byte(ed, cur, mcount);
            delete_charwise(ed, cur, e);
            return true;
        }
        case 'h': {
            size_t s = motion_h_byte(ed, cur, mcount);
            delete_charwise(ed, s, cur);
            return true;
        }
        case '$': {
            size_t e = line_end_no_nl(ed, ed->cursor_line);
            delete_charwise(ed, cur, e);
            return true;
        }
        case '0': {
            size_t s = pt_line_to_byte(ed->pt, ed->cursor_line);
            delete_charwise(ed, s, cur);
            return true;
        }
        case '^': {
            size_t s = pt_line_to_byte(ed->pt, ed->cursor_line)
                     + first_nonblank_col(ed, ed->cursor_line);
            if (s < cur) delete_charwise(ed, s, cur);
            else         delete_charwise(ed, cur, s);
            return true;
        }
        case 'j':
            /* dj: delete current line and `mcount` lines below */
            delete_lines(ed, ed->cursor_line, 1 + mcount);
            return true;
        case 'k': {
            /* dk: delete current line and `mcount` lines above */
            int sl = ed->cursor_line - mcount;
            if (sl < 0) sl = 0;
            delete_lines(ed, sl, ed->cursor_line - sl + 1);
            return true;
        }
        case 'G':
            /* dG: delete from current line to last line */
            delete_lines(ed, ed->cursor_line,
                         (int)pt_line_count(ed->pt) - ed->cursor_line);
            return true;
        case 'g': {
            int next = getch();
            if (next == 'g') {
                /* dgg: delete from first line through current line */
                delete_lines(ed, 0, ed->cursor_line + 1);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

/* Read an optional motion-count from getch() stream, returning the
 * count and stashing the non-digit character in *out_ch. If no digit
 * was found, count = 1 and the first read char is in *out_ch. */
static int read_motion_count(int* out_ch) {
    int ch = getch();
    int count = 0;
    if (ch >= '1' && ch <= '9') {
        while (ch >= '0' && ch <= '9') {
            count = count * 10 + (ch - '0');
            ch = getch();
        }
    }
    *out_ch = ch;
    return count > 0 ? count : 1;
}

/* ====================== Key Handlers ====================== */

static void handle_normal_mode(Editor* ed, int ch) {
    PT_Edit edit = {0};

    /* Parse leading [1-9][0-9]* count prefix. */
    int count = 1;
    if (ch >= '1' && ch <= '9') {
        count = 0;
        while (ch >= '0' && ch <= '9') {
            count = count * 10 + (ch - '0');
            ch = getch();
        }
        if (count == 0) count = 1;
    }

    switch (ch) {
        case 'i': ed->insert_mode = true; break;
        case 'a':
            {
                size_t ll = pt_line_length(ed->pt, ed->cursor_line);
                /* strip trailing newline */
                if (ll > 0) {
                    char t[1]={0};
                    pt_read(ed->pt, pt_line_to_byte(ed->pt, ed->cursor_line)+ll-1, t, 1);
                    if (t[0]=='\n') ll--;
                }
                if (ed->cursor_col < (int)ll) ed->cursor_col++;
                ed->insert_mode = true;
            } break;
        case 'A':
            {
                size_t ll = pt_line_length(ed->pt, ed->cursor_line);
                if (ll > 0) {
                    char t[1]={0};
                    pt_read(ed->pt, pt_line_to_byte(ed->pt, ed->cursor_line)+ll-1, t, 1);
                    if (t[0]=='\n') ll--;
                }
                ed->cursor_col = (int)ll;
                ed->insert_mode = true;
            } break;
        case 'I':
            {
                /* move to first non-blank */
                char buf[4096]={0};
                pt_get_line(ed->pt, ed->cursor_line, buf, sizeof(buf)-1);
                int i=0; while (buf[i]==' '||buf[i]=='\t') i++;
                ed->cursor_col=i;
                ed->insert_mode = true;
            } break;
        case 'o':
            pt_insert_line(ed->pt, (size_t)ed->cursor_line + 1, "", 0, &edit);
            update_tree_after_edit(ed, &edit);
            ed->cursor_line++;
            ed->cursor_col = 0;
            auto_indent_new_line(ed);
            ed->insert_mode = true;
            break;
        case 'O':
            pt_insert_line(ed->pt, (size_t)ed->cursor_line, "", 0, &edit);
            update_tree_after_edit(ed, &edit);
            ed->cursor_col = 0;
            ed->insert_mode = true;
            break;
        case 'y':
            if (getch() == 'y') yank_line(ed);
            break;
        case 'd':
            {
                /* Read optional inner count, then motion character.
                 * Supports: dd, dw, db, de, dh, dl, dj, dk, d$, d0, d^,
                 *           dG, dgg, and counted variants: d5l, d2w, 3dd. */
                int next;
                int inner = read_motion_count(&next);
                int total = count * inner;

                if (next == 'd') {
                    delete_lines(ed, ed->cursor_line, total);
                } else if (!do_delete_motion(ed, next, total)) {
                    /* unknown motion: discard the key */
                }
            }
            scroll_to_cursor(ed);
            break;
        case 'c':
            {
                /* Change: like delete but enter insert mode after. */
                int next;
                int inner = read_motion_count(&next);
                int total = count * inner;

                if (next == 'c') {
                    delete_lines(ed, ed->cursor_line, total);
                    /* open a new empty line in place */
                    PT_Edit ee = {0};
                    pt_insert_line(ed->pt, ed->cursor_line, "", 0, &ee);
                    update_tree_after_edit(ed, &ee);
                    ed->cursor_col = 0;
                    auto_indent_new_line(ed);
                } else {
                    do_delete_motion(ed, next, total);
                }
                ed->insert_mode = true;
            }
            scroll_to_cursor(ed);
            break;
        case 'D':
            /* D == d$ */
            do_delete_motion(ed, '$', 1);
            break;
        case 'C':
            /* C == c$ */
            do_delete_motion(ed, '$', 1);
            ed->insert_mode = true;
            break;
        case 's':
            /* s == cl: delete `count` chars then insert */
            do_delete_motion(ed, 'l', count);
            ed->insert_mode = true;
            break;
        case 'S':
            /* S == cc: change whole line */
            delete_lines(ed, ed->cursor_line, count);
            {
                PT_Edit ee = {0};
                pt_insert_line(ed->pt, ed->cursor_line, "", 0, &ee);
                update_tree_after_edit(ed, &ee);
                ed->cursor_col = 0;
                auto_indent_new_line(ed);
            }
            ed->insert_mode = true;
            break;
        case 'Y':
            /* Y yanks the line (same as yy in modern vim) */
            yank_line(ed);
            break;
        case 'p':
            if (ed->clip_is_line) paste_linewise(ed, true);
            else                  paste_charwise(ed, true);
            break;
        case 'P':
            if (ed->clip_is_line) paste_linewise(ed, false);
            else                  paste_charwise(ed, false);
            break;
        case 'u':
            pt_undo(ed->pt, &edit);
            reparse_document(ed);
            if (ed->cursor_line >= (int)pt_line_count(ed->pt)) {
                ed->cursor_line = (int)pt_line_count(ed->pt) - 1;
                if (ed->cursor_line < 0) ed->cursor_line = 0;
            }
            ed->cursor_col = 0;
            scroll_to_cursor(ed);
            break;
        case 'g':
            if (getch() == 'g') goto_line(ed, 0);
            break;
        case 'G':
            goto_line(ed, (int)pt_line_count(ed->pt) - 1);
            break;
        case 'w':
            for (int k = 0; k < count; k++) move_word_forward(ed);
            break;
        case 'b':
            for (int k = 0; k < count; k++) move_word_backward(ed);
            break;
        case 'e':
            move_to_byte_pos(ed, motion_e_byte(ed, get_current_byte_pos(ed), count));
            break;
        case '^': case '0': ed->cursor_col = 0; break;
        case '$':
            {
                size_t ll = pt_line_length(ed->pt, ed->cursor_line);
                if (ll > 0) {
                    char t[1]={0};
                    pt_read(ed->pt, pt_line_to_byte(ed->pt, ed->cursor_line)+ll-1, t, 1);
                    if (t[0]=='\n') ll--;
                }
                ed->cursor_col = (ll > 0) ? (int)ll - 1 : 0;
            } break;
        case 'h': case KEY_LEFT:  move_cursor(ed, 0, -count); break;
        case 'l': case KEY_RIGHT: move_cursor(ed, 0,  count); break;
        case 'j': case KEY_DOWN:  move_cursor(ed,  count, 0); break;
        case 'k': case KEY_UP:    move_cursor(ed, -count, 0); break;
        case KEY_PPAGE: case 2:   move_cursor(ed, -(LINES - 2), 0); break;
        case KEY_NPAGE: case 6:   move_cursor(ed, LINES - 2, 0); break;
        case 'x':
            {
                size_t cur = get_current_byte_pos(ed);
                size_t end = motion_l_byte(ed, cur, count);
                if (end > cur) {
                    clip_save_charwise(ed, cur, end);
                    pt_delete_byte_range(ed->pt, cur, end, &edit);
                    update_tree_after_edit(ed, &edit);
                    /* clamp cursor if line shorter now */
                    size_t lend = line_end_no_nl(ed, ed->cursor_line);
                    size_t lb = pt_line_to_byte(ed->pt, ed->cursor_line);
                    if (cur >= lend && lend > lb) move_to_byte_pos(ed, lend - 1);
                    else                          move_to_byte_pos(ed, cur);
                }
            } break;
        case '>':
            if (getch() == '>') shift_line(ed, +1);
            break;
        case '<':
            if (getch() == '<') shift_line(ed, -1);
            break;
        case '\t':
            /* normal-mode Tab: indent current line one level */
            shift_line(ed, +1);
            break;
        case KEY_BTAB:
            shift_line(ed, -1);
            break;
        case ']':
            {
                /* jump to next diagnostic */
                size_t cur = get_current_byte_pos(ed);
                size_t best = (size_t)-1;
                for (size_t i = 0; i < ed->n_diags; ++i) {
                    if (ed->diags[i].start_byte > cur && ed->diags[i].start_byte < best)
                        best = ed->diags[i].start_byte;
                }
                if (best != (size_t)-1) move_to_byte_pos(ed, best);
                else set_status_msg(ed, "No next diagnostic");
            } break;
        case '[':
            {
                /* jump to prev diagnostic */
                size_t cur = get_current_byte_pos(ed);
                size_t best = (size_t)-1;
                bool found = false;
                for (size_t i = 0; i < ed->n_diags; ++i) {
                    if (ed->diags[i].start_byte < cur &&
                        (!found || ed->diags[i].start_byte > best)) {
                        best = ed->diags[i].start_byte;
                        found = true;
                    }
                }
                if (found) move_to_byte_pos(ed, best);
                else set_status_msg(ed, "No previous diagnostic");
            } break;
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
                        set_status_msg(ed, "File saved: %s", ed->filename);
                    } else {
                        set_status_msg(ed, "Save failed!");
                    }
                    if (strcmp(cmd, "wq") == 0) ed->running = false;
                } else if (strcmp(cmd, "q!") == 0) {
                    ed->running = false;
                } else if (strcmp(cmd, "q") == 0) {
                    if (!pt_is_modified(ed->pt)) ed->running = false;
                    else set_status_msg(ed, "Unsaved changes. Use :q! to force quit.");
                } else if (strncmp(cmd, "set ", 4) == 0) {
                    if      (strcmp(cmd+4, "et")  == 0) { ed->expand_tab = true;  set_status_msg(ed, "expandtab"); }
                    else if (strcmp(cmd+4, "noet")== 0) { ed->expand_tab = false; set_status_msg(ed, "noexpandtab"); }
                    else if (strncmp(cmd+4, "ts=", 3) == 0) {
                        int n = atoi(cmd+7);
                        if (n > 0 && n <= 16) { ed->tab_width = n; set_status_msg(ed, "tab_width=%d", n); }
                    }
                }
            }
            break;
        case 'q':
            if (!pt_is_modified(ed->pt)) ed->running = false;
            else set_status_msg(ed, "Unsaved changes. Use :q! to force quit.");
            break;
    }
}

static void handle_insert_mode(Editor* ed, int ch) {
    PT_Edit edit = {0};

    if (ch == 27) {  /* ESC */
        ed->insert_mode = false;
        if (ed->cursor_col > 0) ed->cursor_col--;
        return;
    }

    /* Enter */
    if (ch == '\n' || ch == KEY_ENTER || ch == '\r') {
        size_t bp = get_current_byte_pos(ed);
        char next_c = peek_char_at(ed, bp);
        char prev_c = (bp > 0) ? peek_char_at(ed, bp - 1) : 0;
        bool split_block = (prev_c == '{' && next_c == '}')
                        || (prev_c == '(' && next_c == ')')
                        || (prev_c == '[' && next_c == ']');

        /* Compute current line's leading indent before splitting */
        char curline[4096] = {0};
        pt_get_line(ed->pt, ed->cursor_line, curline, sizeof(curline)-1);
        int base_indent = 0;
        while (curline[base_indent] == ' ' || curline[base_indent] == '\t')
            base_indent++;

        if (split_block) {
            /* Insert "\n<base+tab spaces>\n<base spaces>" at the cursor,
             * then place the cursor on the middle (inner) line. */
            int inner = base_indent + ed->tab_width;
            if (inner < 0) inner = 0;
            size_t total = (size_t)(1 + inner + 1 + base_indent);
            char* s = (char*)malloc(total + 1);
            if (s) {
                size_t k = 0;
                s[k++] = '\n';
                for (int i = 0; i < inner; i++)        s[k++] = ' ';
                s[k++] = '\n';
                for (int i = 0; i < base_indent; i++)  s[k++] = ' ';
                s[k] = '\0';
                pt_insert_at_byte(ed->pt, bp, s, total, &edit);
                update_tree_after_edit(ed, &edit);
                free(s);
                ed->cursor_line += 1;
                ed->cursor_col   = inner;
                scroll_to_cursor(ed);
            }
            return;
        }

        /* Plain split at cursor: insert newline + matching indent. */
        size_t ins_len = 1 + (size_t)base_indent;
        char* s = (char*)malloc(ins_len + 1);
        if (!s) return;
        s[0] = '\n';
        for (int i = 0; i < base_indent; i++) s[1 + i] = ' ';
        s[ins_len] = '\0';
        pt_insert_at_byte(ed->pt, bp, s, ins_len, &edit);
        update_tree_after_edit(ed, &edit);
        free(s);
        ed->cursor_line += 1;
        ed->cursor_col   = base_indent;
        scroll_to_cursor(ed);
        return;
    }

    /* Tab */
    if (ch == '\t' || ch == KEY_STAB) {
        insert_indent(ed);
        return;
    }

    /* Shift-Tab in insert mode: dedent current line */
    if (ch == KEY_BTAB) {
        shift_line(ed, -1);
        return;
    }

    /* Backspace */
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (ed->cursor_col == 0 && ed->cursor_line > 0) {
            /* join with previous line */
            size_t bp = get_current_byte_pos(ed);
            if (bp == 0) return;
            /* delete the newline between prev and current */
            pt_delete_byte_range(ed->pt, bp - 1, bp, &edit);
            update_tree_after_edit(ed, &edit);
            move_to_byte_pos(ed, bp - 1);
            return;
        }
        if (ed->cursor_col > 0) {
            size_t bp = get_current_byte_pos(ed);
            char prev = peek_char_at(ed, bp - 1);
            char next = peek_char_at(ed, bp);
            char close = autopair_close_for(ed->lang, prev);
            /* Remove auto-paired closing char too */
            if (close && next == close) {
                pt_delete_byte_range(ed->pt, bp - 1, bp + 1, &edit);
                update_tree_after_edit(ed, &edit);
                ed->cursor_col--;
                return;
            }
            /* Soft-tab backspace: remove up to tab_width spaces if all spaces before cursor */
            if (ed->expand_tab && prev == ' ') {
                int n = 0;
                size_t pos = bp;
                while (n < ed->tab_width && pos > 0) {
                    char c = peek_char_at(ed, pos - 1);
                    if (c != ' ') break;
                    pos--;
                    n++;
                    if (((ed->cursor_col - n) % ed->tab_width) == 0) break;
                }
                if (n == 0) n = 1;
                pt_delete_byte_range(ed->pt, bp - n, bp, &edit);
                update_tree_after_edit(ed, &edit);
                ed->cursor_col -= n;
                return;
            }
            pt_delete_byte_range(ed->pt, bp - 1, bp, &edit);
            update_tree_after_edit(ed, &edit);
            ed->cursor_col--;
        }
        return;
    }

    if (ch == KEY_LEFT)  { move_cursor(ed, 0, -1); return; }
    if (ch == KEY_RIGHT) { move_cursor(ed, 0,  1); return; }
    if (ch == KEY_UP)    { move_cursor(ed,-1,  0); return; }
    if (ch == KEY_DOWN)  { move_cursor(ed, 1,  0); return; }

    /* Printable */
    if (ch >= 32 && ch < 127) {
        char c = (char)ch;
        size_t bp = get_current_byte_pos(ed);
        char next = peek_char_at(ed, bp);

        /* C/Cpp/Go/JS: typing '{' after an identifier or ')' should
         * automatically insert a separating space, e.g. `if(x)` + '{' -> `if(x) {`. */
        if (c == '{' &&
            (ed->lang == LANG_C  || ed->lang == LANG_CPP ||
             ed->lang == LANG_GO || ed->lang == LANG_JS) &&
            ed->cursor_col > 0)
        {
            char prevc = peek_char_at(ed, bp - 1);
            if (prevc != ' ' && prevc != '\t' && prevc != '\n' &&
                prevc != '(' && prevc != '{' && prevc != '['  &&
                prevc != '=' && prevc != ',')
            {
                PT_Edit sp_edit = {0};
                char sp = ' ';
                pt_insert_at_byte(ed->pt, bp, &sp, 1, &sp_edit);
                update_tree_after_edit(ed, &sp_edit);
                ed->cursor_col++;
                bp++;
                next = peek_char_at(ed, bp);
            }
        }

        /* smart skip: typing a closing char that already exists -> just move */
        if (is_pair_close(c) && next == c) {
            char want_open = open_for_close(c);
            char close_for_open = autopair_close_for(ed->lang, want_open);
            if (close_for_open == c) {
                ed->cursor_col++;
                return;
            }
        }

        /* string quote double-typed: skip past */
        if ((c == '"' || c == '\'') && next == c &&
            autopair_close_for(ed->lang, c) == c) {
            ed->cursor_col++;
            return;
        }

        /* Auto-pair: insert open+close, leave cursor between */
        char close = autopair_close_for(ed->lang, c);
        if (close) {
            /* Don't auto-pair quote if previous is alnum (likely an apostrophe) */
            bool suppress = false;
            if ((c == '"' || c == '\'') && ed->cursor_col > 0) {
                char prev = peek_char_at(ed, bp - 1);
                if (is_word_char(prev)) suppress = true;
            }
            if (!suppress) {
                char buf[2] = { c, close };
                pt_insert_at_byte(ed->pt, bp, buf, 2, &edit);
                update_tree_after_edit(ed, &edit);
                ed->cursor_col++;
                return;
            }
        }

        /* Semicolon completion: in C/Cpp/Go/JS, typing ';' when next char is
         * already ';' just skips past it (avoid double semicolons). */
        if (c == ';' && next == ';' &&
            (ed->lang == LANG_C || ed->lang == LANG_CPP ||
             ed->lang == LANG_GO || ed->lang == LANG_JS)) {
            ed->cursor_col++;
            return;
        }

        pt_insert_at_byte(ed->pt, bp, &c, 1, &edit);
        update_tree_after_edit(ed, &edit);
        ed->cursor_col++;
    }
}

/* ====================== main ====================== */

int main(int argc, char** argv) {
    setlocale(LC_ALL, "");

    Editor ed;
    editor_init(&ed, argc > 1 ? argv[1] : NULL);

    initscr();
    if (has_colors()) {
        start_color();
        use_default_colors();
        assume_default_colors(-1, -1);

        init_pair(CP_DEFAULT,    -1,            -1);  /* default text */
        init_pair(CP_IDENTIFIER, COLOR_BLUE,    -1);
        init_pair(CP_STRING,     COLOR_GREEN,   -1);
        init_pair(CP_COMMENT,    COLOR_CYAN,    -1);
        init_pair(CP_KEYWORD,    COLOR_YELLOW,  -1);
        init_pair(CP_NUMBER,     COLOR_MAGENTA, -1);
        init_pair(CP_ERROR,      COLOR_RED,     -1);
        init_pair(CP_TYPE,       COLOR_YELLOW,  -1);
        init_pair(CP_PREPROC,    COLOR_MAGENTA, -1);
    }

    raw();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(1);

    while (ed.running) {
        redraw(&ed);
        int ch = getch();
        if (ed.insert_mode) handle_insert_mode(&ed, ch);
        else                handle_normal_mode(&ed, ch);
    }

    endwin();
    editor_free(&ed);
    return 0;
}
