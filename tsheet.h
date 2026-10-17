/* tsheet: a small terminal spreadsheet. Only libc is required; the optional
   native X11 clipboard (HAVE_XLIB) is the only extra dependency. */
#ifndef TSHEET_H
#define TSHEET_H

#include <stddef.h>
#include <stdint.h>

#define TSHEET_NAME    "tsheet"
#define TSHEET_VERSION "alpha1"

#define MAX_ROWS   10000
#define MAX_COLS   702          /* A .. ZZ */
#define MAX_SHEETS 32
#define MAX_MENUS  8
#define EBUF_MAX   1024

/* menu actions for graph options: A_GOPT_BASE + option toggles it,
   A_GOPT_VAL + option * 32 + choice sets a choice */
#define A_GOPT_BASE 2000
#define A_GOPT_VAL  3000
#define A_CONV_BASE 4000   /* Convert menu items: A_CONV_BASE + index in conv_pairs (config.h) */

/* ---- merged cells: a rectangle that behaves as one cell (top-left holds the value) ---- */
#define MAX_MERGE 512
typedef struct { int r1, c1, r2, c2; } Merge;

/* ---- selection areas: the active one, then Ctrl+click areas (like Excel) ---- */
#define MAX_SEL   64
#define MAX_RECTS (MAX_SEL + 1)
typedef struct { int r1, c1, r2, c2; } Rect;

/* ---- grid (sheet.c) ---- */
typedef struct {
    char **rows[MAX_ROWS];      /* rows[r][c]: raw cell text or NULL */
    char **fmts[MAX_ROWS];      /* rows[r][c]: display format or NULL */
    int max_r, max_c;           /* extent of anything ever written */
    Merge mg[MAX_MERGE];        /* merged blocks, disjoint */
    int nmg;
} Sheet;

/* ---- byte buffer (io.c) ---- */
typedef struct { char *s; size_t n, cap; } Buf;

/* ---- html table (html.c) ---- */
typedef struct { int r, c, rs, cs; char *text; unsigned char bold, hl; char align; } HCell;   /* rs, cs: row and column span; bold, hl, align: the cell's formatting */
typedef struct { HCell *cells; size_t n, cap; int rows, cols; } HTable;

/* ---- file formats (io.c) ---- */
enum { FMT_CSV, FMT_TSV, FMT_XML, FMT_XLSX, FMT_ODS, FMT_JSON, FMT_COUNT };

/* export settings (File > Export options): delimiter for CSV, CRLF line ends,
   and whether to write only the selected block */
typedef struct { int delim; int crlf; int sel_only; } ExpOpt;

/* ---- one open sheet and its view state ---- */
typedef struct {
    char name[64];
    char *path;                 /* NULL until saved */
    int fmt;
    Sheet *g;
    int cr, cc, ar, ac, sel_kind, top, left, dirty;
    int next;                   /* Ctrl+click areas: ext[0..next-1] */
    Rect ext[MAX_SEL];
    int ext_kind[MAX_SEL];      /* their sel_kind: 0 cells, 1 columns, 2 rows */
    unsigned char *cw;          /* column widths in characters, 0 = default */
    unsigned char *rh;          /* row heights in text lines, 0 = default */
} Doc;

/* ---- what a cell holds, for colouring (config.h: kind_fg) ---- */
enum { KIND_TEXT, KIND_NUMBER, KIND_TIME, KIND_FORMULA, KIND_ERROR, KIND_COUNT };

/* ---- graph options dialog item (app.c fills, ui.c draws) ---- */
typedef struct { char label[160]; int act, inv; const char *desc; } DlgItem;
enum { DLGK_MAIN, DLGK_CHOICE };      /* the option list, or the choices of one option */

/* a graph option: a flag, an argument, or one of several choices. cmd is the
   gnuplot command; %s is replaced by the argument or the choice. */
enum { GO_FLAG, GO_ARG, GO_CHOICE };
typedef struct {
    const char *name;           /* shown in the editor */
    int kind;
    const char *cmd;
    const char *off;            /* choice that emits this command instead (or NULL) */
    const char *const *choices; /* GO_CHOICE values */
    const char *help;           /* from the gnuplot documentation */
} GraphOpt;

/* ---- undo history: old values of cells changed by one user action ---- */
typedef struct { Sheet *g; int r, c, fmt; char *old; } JEntry;   /* fmt: 1 for a format */
typedef struct { JEntry *e; int n, cap; } Txn;

/* ---- clipboard blocks ---- */
/* f (optional): the format string of each cell, from pasted HTML or text; NULL when none */
typedef struct { int rows, cols; char **v; Merge *mg; int nm; char **f; } Block;
typedef struct {
    Block raw, disp;            /* formulas as typed / displayed values */
    char *text, *html;          /* what we last gave the system clipboard */
    int r0, c0, cut, valid;
    int values;                 /* copied as values: internal paste uses disp */
    int doc;                    /* sheet index the copy came from */
} Clip;

/* ---- input (term.c) ---- */
enum {
    K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_BTAB,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10
};
enum { EV_NONE, EV_KEY, EV_MOUSE, EV_RESIZE };
enum { MS_PRESS, MS_RELEASE, MS_MOVE, MS_WHEEL_UP, MS_WHEEL_DOWN };
enum { MOD_SHIFT = 1, MOD_CTRL = 2 };
typedef struct { int type, key, mods, btn, mtype, mx, my; } Event;

/* ---- actions, bound to menu items (config.h) ---- */
enum {
    A_NONE, A_NEW, A_OPEN, A_FIND, A_SAVE, A_SAVEAS, A_SAVEAS_CSV, A_SAVEAS_TSV,
    A_SAVEAS_XML, A_SAVEAS_XLSX, A_SAVEAS_ODS, A_SAVEAS_JSON, A_CLOSE, A_QUIT,
    A_CUT, A_COPY_RAW, A_COPY_VALUES, A_COPY_FMT,
    A_PASTE, A_PASTE_VALUES, A_PASTE_FORMULAS, A_PASTE_FMT, A_PASTE_TRANSPOSE,
    A_CLEAR, A_FILL_DOWN, A_FILL_RIGHT, A_SELALL,
    A_NEXT_SHEET, A_PREV_SHEET, A_HELP,
    A_ALIGN_L, A_ALIGN_C, A_ALIGN_R, A_FMT_DATE, A_FMT_DEC, A_FMT_CLEAR, A_RESIZE,
    A_COPY_LATEX,
    A_VIM, A_VIMCMD, A_DLG_TITLE, A_DLG_XLAB, A_DLG_YLAB, A_DLG_ARG, A_GRAPH_OPTIONS, A_UNDO, A_REDO, A_FMT_BOLD, A_FMT_HL, A_MERGE, A_UNMERGE,
    A_GRAPH, A_GRAPH_STYLE_LINES, A_GRAPH_STYLE_POINTS, A_GRAPH_STYLE_LINESPOINTS,
    A_GRAPH_STYLE_BARS, A_GRAPH_GRID, A_GRAPH_LOGY, A_GRAPH_EXTRA,
    A_SORT_ASC, A_SORT_DESC, A_GOTO,
    A_XOPT_COMMA, A_XOPT_SEMI, A_XOPT_TAB, A_XOPT_PIPE, A_XOPT_CRLF,
    A_XOPT_SELONLY, A_XOPT_SHOW, A_IMP_COMMA, A_IMP_SEMI, A_IMP_TAB, A_IMP_PIPE,
    A_XOPT_SPACE, A_IMP_SPACE, A_GRAPH_VIEW, A_DLG_IMPORT, A_DLG_EXPORT, A_CONV_TO,
    A_PREFIX, A_SUFFIX, A_INT_PLAIN, A_INT_COMMA, A_INT_SPACE, A_INT_SCI, A_INT_ENG
};

typedef struct MenuItem {
    const char *label;          /* NULL ends a list, "-" is a separator */
    const char *key;            /* shortcut hint shown on the right */
    int act;
    const struct MenuItem *sub; /* non-NULL: submenu */
    const char *desc;           /* shown in the status bar while the item is highlighted */
} MenuItem;
/* items: a pop-up panel; with NULL items, click opens the graph dialog */
typedef struct { const char *title; const MenuItem *items; } MenuBar;
typedef struct { const MenuItem *items; int x, y, sel; } Panel;

enum { MODE_NORMAL, MODE_EDIT, MODE_PROMPT, MODE_LIST, MODE_RESIZE, MODE_DLG };
enum { LIST_NONE, LIST_FILES, LIST_HELP };

typedef struct {
    Doc docs[MAX_SHEETS];
    int ndocs, cur, sheetno;
    int rows, cols;             /* terminal size */
    int mode;
    char ebuf[EBUF_MAX];        /* cell edit or prompt text */
    int elen, epos;
    int prompt_act, prompt_fmt;
    char msg[256];              /* status message, or prompt label */
    int dragging, drag_kind;     /* 1-3 selection, 4 column width, 5 row height */
    int rs_idx, rs_orig, rs_anchor;
    int fr1, fc1, fr2, fc2;      /* fill handle: cells being filled (empty if fr2 < fr1) */
    int fill_axis;               /* 1 down, 2 right */
    int vim, vim_vis, vim_pend;  /* vim keys: option, visual mode, pending g/d/y */
    int hov_r, hov_c;            /* cell under the pointer, -1 if none */
    int mhov;                    /* menu bar title under the pointer, -1 if none */
    int warn;                    /* the status message is a warning: shown in red */
    int fref_pos, fref_len;      /* formula mode: where the last inserted reference is */
    char gextra[1024];          /* graph settings: gnuplot commands, ';' separated */
    DlgItem dlg[24];
    int ndlg, dlg_kind, dlg_sel, dlg_top, dlg_opt;
    const MenuItem *acts;       /* non-NULL: the dialog lists these actions, not graph options */
    const char *acts_title;
    double last_t;
    int last_r, last_c;
    int armed;                  /* a second press confirms e.g. close */
    int mb;                     /* open menubar index, -1 if none */
    Panel pan[2];
    int npanels;
    int tabx[MAX_SHEETS], tabw[MAX_SHEETS];
    char **list;
    int nlist, lsel, ltop, lkind;
    Clip clip;
    ExpOpt xo;                  /* export options */
    int imp_delim;              /* delimiter for opening text files (0: comma) */
    int quit;
} App;

/* sizes: a cell is its width in characters (content) and its height in text lines */
#define COL_DEF_W   10
#define ROW_DEF_H   1
#define ROWHDR_W    6            /* row-number gutter, its right border is at x = 6 */
#define COL_MIN_W   3
#define COL_MAX_W   60
#define ROW_MAX_H   10
#define VIEW_MAX    512

static inline int doc_colw(const Doc *d, int c) { return d->cw && d->cw[c] ? d->cw[c] : COL_DEF_W; }
static inline int doc_rowh(const Doc *d, int r) { return d->rh && d->rh[r] ? d->rh[r] : ROW_DEF_H; }

/* What is on screen: visible columns and rows with their positions and sizes.
   Row i has h[i] text lines starting at y[i], then a border line at y[i]+h[i]. */
typedef struct {
    int nc, c[VIEW_MAX], x[VIEW_MAX], w[VIEW_MAX], xend;
    int nr, r[VIEW_MAX], y[VIEW_MAX], h[VIEW_MAX];
} View;

static inline void doc_sel(const Doc *d, int *r1, int *c1, int *r2, int *c2) {
    *r1 = d->ar < d->cr ? d->ar : d->cr;
    *r2 = d->ar < d->cr ? d->cr : d->ar;
    *c1 = d->ac < d->cc ? d->ac : d->cc;
    *c2 = d->ac < d->cc ? d->cc : d->ac;
    if (d->sel_kind == 1) { *r1 = 0; *r2 = MAX_ROWS - 1; }   /* whole columns */
    if (d->sel_kind == 2) { *c1 = 0; *c2 = MAX_COLS - 1; }   /* whole rows */
}

/* Areas: index 0..next-1 are the stored ones, index next is the active one. */
static inline int doc_nrects(const Doc *d) { return d->next + 1; }
static inline int doc_kind(const Doc *d, int i) { return i < d->next ? d->ext_kind[i] : d->sel_kind; }
static inline void doc_rect(const Doc *d, int i, Rect *q) {
    if (i < d->next) *q = d->ext[i];
    else doc_sel(d, &q->r1, &q->c1, &q->r2, &q->c2);
}
static inline int rect_has(const Rect *q, int r, int c) {
    return r >= q->r1 && r <= q->r2 && c >= q->c1 && c <= q->c2;
}
/* A column (by_col) or row header is lit when an area covers it. Row areas do
   not light column headers and column areas do not light row headers. */
static inline int doc_hdr_sel(const Doc *d, int idx, int by_col) {
    Rect q;
    for (int i = 0; i < doc_nrects(d); i++) {
        int kind = doc_kind(d, i);
        if (by_col ? kind == 2 : kind == 1) continue;
        doc_rect(d, i, &q);
        if (by_col ? idx >= q.c1 && idx <= q.c2 : idx >= q.r1 && idx <= q.r2) return 1;
    }
    return 0;
}
static inline int doc_in_sel(const Doc *d, int r, int c) {
    Rect q;
    for (int i = 0; i < doc_nrects(d); i++) {
        doc_rect(d, i, &q);
        if (rect_has(&q, r, c)) return 1;
    }
    return 0;
}
/* ---- sheet.c ---- */
int         doc_rects(const Doc *d, Rect *out);   /* areas clipped to the used cells */
Sheet      *sheet_new(void);
void        sheet_free(Sheet *s);
const char *sheet_get(const Sheet *s, int r, int c);
void        sheet_set(Sheet *s, int r, int c, const char *raw);
void        col_name(int c, char *buf);
int         cell_num(const Sheet *s, int r, int c, int depth, double *out); /* 0 num,1 text,2 err,3 empty */
void        cell_display(const Sheet *s, int r, int c, char *out, size_t n, int *align); /* 0 left 1 right 2 centre */
const char *sheet_fmt(const Sheet *s, int r, int c);
const char *sheet_get_raw(const Sheet *s, int r, int c);       /* no merge redirect */
const Merge *sheet_merge_of(const Sheet *s, int r, int c);     /* NULL if not merged */
int         sheet_merge_add(Sheet *s, int r1, int c1, int r2, int c2);
void        sheet_unmerge(Sheet *s, int r1, int c1, int r2, int c2);
void        sheet_put_raw(Sheet *s, int r, int c, const char *raw);   /* exact cell, journaled */
void        sheet_put_fmt(Sheet *s, int r, int c, const char *fmt);
void        fmt_parse(const char *f, int *align, int *bold, int *hl, const char **body);
void        fmt_build(char *out, size_t n, int align, int bold, int hl, const char *body);

/* ---- number formats (sheet.c). A numeric body is "n" + style + decimals + SEP +
   prefix + SEP + suffix + SEP. style: '-' plain, ',' 1,000, ' ' 1 000, 'e' 1*10^3,
   'g' 1 k (engineering). decimals: '-' or a digit. ".N" is the older decimals-only
   body and still reads. */
typedef struct { int style; int dec; char pre[64]; char suf[64]; } NumSpec;
void        num_parse(const char *body, NumSpec *ns);
void        num_build(char *out, size_t n, const NumSpec *ns);
/* units.c: unit conversions (the Convert menu) */
int         units_find(const char *sym);            /* index of a unit symbol, -1 if unknown */
const char *units_sym(int id);
int         units_linear(int from, int to, double *a, double *b);   /* v*a+b converts; -1: different kinds */
int         num_from_text(const char *in, char *raw, size_t rawn, NumSpec *ns);
void        latex_to_utf8(const char *in, char *out, size_t n);
void        sheet_style(const Sheet *s, int r, int c, int *bold, int *hl);
void        sheet_fmt_set(Sheet *s, int r, int c, const char *fmt);
int         parse_datetime(const char *in, long long *out, int *kind); /* 0 ok: 1 date, 2 date+time, 3 time */
int         cell_kind(const Sheet *s, int r, int c);   /* -1 empty, else KIND_* */
char       *formula_shift(const char *f, int dr, int dc);
void        txn_begin(Txn *t);          /* record changes into t */
void        txn_end(void);
Txn        *txn_pause(void);            /* stop recording; returns the journal in use */
void        txn_resume(Txn *j);
void        txn_free(Txn *t);
void        txn_undo(Txn *t, Txn *inv); /* restores t's old values; inv records the current ones */

/* ---- io.c ---- */
void        buf_put(Buf *b, const char *s, size_t n);
void        buf_free(Buf *b);
int         io_read_fd(int fd, char **out, size_t *len);
int         io_read_file(const char *path, char **out, size_t *len);
int         io_fmt_from_path(const char *path);     /* -1 if unknown */
int         io_fmt_from_text(const char *buf, size_t len);
int         io_parse(Sheet *g, const char *buf, size_t len, int fmt, char *err, size_t errn);
int         io_parse_delim(Sheet *g, const char *buf, size_t len, int fmt, int delim, char *err, size_t errn);
/* read.c: xlsx and ods (zip files), first sheet only */
int         read_xlsx(Sheet *g, const char *buf, size_t len, char *err, size_t errn);
int         read_ods(Sheet *g, const char *buf, size_t len, char *err, size_t errn);
int         io_save(const Sheet *g, const char *name, const char *path, int fmt, const ExpOpt *o, char *err, size_t errn);
const char *io_fmt_name(int fmt);

/* ---- html.c ---- */
char       *html_build(const Sheet *sh, int r1, int c1, int r2, int c2);
int         html_parse_table(const char *html, HTable *t);
void        html_table_free(HTable *t);

/* ---- clip.c ---- */
int         clip_set_rich(const char *text, const char *html);
char       *clip_get(void);
char       *clip_get_html(void);
const char *clip_backend(void);

/* ---- term.c ---- */
int         term_init(void);
void        term_fini(void);
void        term_size(int *rows, int *cols);
int         term_poll(Event *e, int timeout_ms);   /* 1 event, 0 none, -1 closed */
void        term_write(const char *s, size_t n);
void        term_flush(void);

/* ---- ui.c ---- */
void        ui_draw(App *a);
int         act_state(const App *a, int act);   /* 1 when the action is applied (shown with *) */
void        view_build(const App *a, View *v);
int         ui_panel_w(const MenuItem *items);
int         ui_panel_h(const MenuItem *items);
int         ui_menu_x(int i);
int         ui_menu_w(int i);
void        ui_list_geom(const App *a, int *x, int *y, int *w, int *h);
void        ui_dlg_geom(const App *a, int *x, int *y, int *w, int *h);
int         ui_dlg_list_rows(const App *a);
/* ---- graph.c ---- */
int         graph_draw(const Doc *d, int r1, int c1, int r2, int c2, const char *cmds,
                       const char *base, char *pdf, size_t pdfn, char *msg, size_t msgn, int view);
void        graph_open(const char *path);

#endif
