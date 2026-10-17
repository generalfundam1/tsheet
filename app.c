/* Application: documents, editing and clipboard commands, menus, mouse and
   keyboard handling, file loading/saving/searching, and the main loop. */
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "tsheet.h"
#include "config.h"

#define FIND_MAX 500
#define UNDO_MAX 200

/* undo and redo history: one journal per user action (see sheet.c) */
static Txn *undo_s[UNDO_MAX], *redo_s[UNDO_MAX];
static int nundo, nredo;

static void hist_push(Txn **st, int *n, Txn *t) {
    if (*n == UNDO_MAX) {                 /* drop the oldest step */
        txn_free(st[0]);
        memmove(st, st + 1, (size_t)(UNDO_MAX - 1) * sizeof *st);
        (*n)--;
    }
    st[(*n)++] = t;
}

static Txn *hist_pop(Txn **st, int *n) { return *n ? st[--*n] : NULL; }

static void hist_clear(Txn **st, int *n) {
    while (*n) txn_free(st[--*n]);
}

/* lines shown by Help > Keys */
static const char *help_lines[] = {
    "Arrows move, Shift+arrows extend, Ctrl+arrows jump to data edge",
    "Enter/Tab move; F2 or typing edits; Esc cancels an edit",
    "Ctrl+C/X/V copy, cut, paste (right-click for variants)",
    "Ctrl+D / Ctrl+R fill down / right; Del clears",
    "Ctrl+click adds an area (Excel-style); Ctrl+click an area again removes it",
    "Clear and format work on all areas; copy, paste, fill, sort and merge need one",
    "Ctrl+N new sheet, Ctrl+O open, Ctrl+F find file, Ctrl+W close sheet",
    "Ctrl+S save, Ctrl+Q quit; Ctrl+PgUp / Ctrl+PgDn switch sheets",
    "F10 or click the menu bar; mouse: click cells, drag to select",
    NULL
};

void run_action(App *a, int act);
static void graph_run(App *a, int view);
static void normal_key(App *a, const Event *e);
static void dialog_open(App *a);
static void dialog_acts(App *a, const MenuItem *items, const char *title);
static void graph_menu_action(App *a, int act);

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static Doc *D(App *a) { return &a->docs[a->cur]; }

static const char *base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* "budget.csv" -> "budget" */
static void strip_ext(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

/* Makes path end with the extension of fmt, replacing a known one. */
static void fix_ext(char *path, size_t n, int fmt) {
    const char *dot = strrchr(base(path), '.');
    if (dot && io_fmt_from_path(path) >= 0) path[dot - path] = '\0';
    size_t l = strlen(path);
    snprintf(path + l, n - l, ".%s", io_fmt_name(fmt));
}

/* ---------- documents ---------- */

static int doc_add(App *a, const char *name, char *path, int fmt, Sheet *g) {
    if (a->ndocs >= MAX_SHEETS || !g) {
        snprintf(a->msg, sizeof a->msg, "too many sheets");
        sheet_free(g);
        free(path);
        return -1;
    }
    Doc *d = &a->docs[a->ndocs];
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->cw = calloc(MAX_COLS, 1);
    d->rh = calloc(MAX_ROWS, 1);
    d->path = path;
    d->fmt = fmt;
    d->g = g;
    a->cur = a->ndocs++;
    return a->cur;
}

static void doc_free(Doc *d) {
    sheet_free(d->g);
    free(d->path);
    free(d->cw);
    free(d->rh);
    memset(d, 0, sizeof *d);
}

static void doc_new(App *a) {
    char nm[32];
    snprintf(nm, sizeof nm, "Sheet%d", ++a->sheetno);
    doc_add(a, nm, NULL, FMT_CSV, sheet_new());
}

static void load_path_do(App *a, const char *path);

static void load_path(App *a, const char *path) {
    Txn *paused = txn_pause();
    load_path_do(a, path);
    txn_resume(paused);
}

static void load_path_do(App *a, const char *path) {
    char *buf;
    size_t len;
    if (io_read_file(path, &buf, &len) < 0) {
        snprintf(a->msg, sizeof a->msg, "cannot open %s", path);
        return;
    }
    int fmt = io_fmt_from_path(path);
    if (fmt < 0) fmt = a->imp_delim ? FMT_CSV : io_fmt_from_text(buf, len);
    Sheet *g = sheet_new();
    char err[256];
    if (!g || io_parse_delim(g, buf, len, fmt, fmt == FMT_CSV ? a->imp_delim : 0, err, sizeof err) < 0) {
        snprintf(a->msg, sizeof a->msg, "%.100s: %.140s", path, err);
        sheet_free(g);
        free(buf);
        return;
    }
    free(buf);
    if (doc_add(a, base(path), strdup(path), fmt, g) >= 0)
        snprintf(a->msg, sizeof a->msg, "opened %s (%s)", path, io_fmt_name(fmt));
}

static const char *delim_name(int c) {
    return c == ';' ? "semicolon" : c == '\t' ? "tab" : c == '|' ? "pipe"
         : c == ' ' ? "space" : "comma";
}

static int act_delim(int act);

/* Sets one numeric field on every cell of the selection: 0 integer style (style),
   1 prefix (s), 2 suffix (s), 3 decimal places (dec; -1 removes them). Other fields,
   bold, highlight and alignment are kept; date formats are left alone. */
static void fmt_num_set(App *a, int field, const char *s, int dec, int style) {
    Doc *d = D(a);
    Rect q[MAX_RECTS];
    int n = doc_rects(d, q);
    for (int i = 0; i < n; i++)
        for (int r = q[i].r1; r <= q[i].r2; r++)
            for (int c = q[i].c1; c <= q[i].c2; c++) {
                int al, bo, hl;
                const char *body;
                fmt_parse(sheet_fmt(d->g, r, c), &al, &bo, &hl, &body);
                if (body[0] == '%') continue;
                NumSpec ns;
                num_parse(body, &ns);
                if (field == 0) ns.style = style;
                else if (field == 1) snprintf(ns.pre, sizeof ns.pre, "%s", s);
                else if (field == 2) snprintf(ns.suf, sizeof ns.suf, "%s", s);
                else ns.dec = dec;
                char nb[200], buf[320];
                num_build(nb, sizeof nb, &ns);
                fmt_build(buf, sizeof buf, al, bo, hl, nb);
                sheet_fmt_set(d->g, r, c, buf);
            }
    d->dirty = 1;
}

/* Does every cell of the selection have the format? what: 0 bold, 1 highlight,
   2 alignment (arg is the character), 3 date/time, 4 decimal places, 5 integer
   style (arg is the style character). An empty
   result counts as no. Stops at the first cell without it. */
static int sel_all_fmt(const App *a, int what, int arg) {
    const Doc *d = &a->docs[a->cur];
    Rect q[MAX_RECTS];
    int n = doc_rects(d, q), any = 0;
    for (int i = 0; i < n; i++)
        for (int r = q[i].r1; r <= q[i].r2; r++)
            for (int c = q[i].c1; c <= q[i].c2; c++) {
                int al, bo, hl;
                const char *body;
                fmt_parse(sheet_fmt(d->g, r, c), &al, &bo, &hl, &body);
                NumSpec ns;
                num_parse(body, &ns);
                int ok = what == 0 ? bo : what == 1 ? hl : what == 2 ? al == arg
                       : what == 3 ? body[0] == '%' : what == 4 ? ns.dec >= 0 : ns.style == arg;
                if (!ok) return 0;
                any = 1;
            }
    return any;
}

/* A setting shows as on in the Import/Export dialog when it is active. */
static int act_on(const App *a, int act) {
    switch (act) {
    case A_XOPT_COMMA: case A_XOPT_SEMI: case A_XOPT_TAB: case A_XOPT_PIPE: case A_XOPT_SPACE:
        return (a->xo.delim ? a->xo.delim : ',') == act_delim(act);     /* 0 means comma */
    case A_IMP_COMMA: case A_IMP_SEMI: case A_IMP_TAB: case A_IMP_PIPE: case A_IMP_SPACE:
        return (a->imp_delim ? a->imp_delim : ',') == act_delim(act);
    case A_XOPT_CRLF:    return a->xo.crlf;
    case A_XOPT_SELONLY: return a->xo.sel_only;
    case A_FMT_BOLD:     return sel_all_fmt(a, 0, 0);
    case A_FMT_HL:       return sel_all_fmt(a, 1, 0);
    case A_ALIGN_L:      return sel_all_fmt(a, 2, '<');
    case A_ALIGN_C:      return sel_all_fmt(a, 2, '^');
    case A_ALIGN_R:      return sel_all_fmt(a, 2, '>');
    case A_FMT_DATE:     return sel_all_fmt(a, 3, 0);
    case A_FMT_DEC:      return sel_all_fmt(a, 4, 0);
    case A_INT_PLAIN:    return sel_all_fmt(a, 5, '-');
    case A_INT_COMMA:    return sel_all_fmt(a, 5, ',');
    case A_INT_SPACE:    return sel_all_fmt(a, 5, ' ');
    case A_INT_SCI:      return sel_all_fmt(a, 5, 'e');
    case A_INT_ENG:      return sel_all_fmt(a, 5, 'g');
    default:             return 0;
    }
}

/* Whether an action is applied right now (what the menus show with a *). */
int act_state(const App *a, int act) { return act_on(a, act); }

/* Settings change the list in place; other actions close the dialog. */
static int act_is_setting(int act) {
    switch (act) {
    case A_XOPT_COMMA: case A_XOPT_SEMI: case A_XOPT_TAB: case A_XOPT_PIPE: case A_XOPT_SPACE:
    case A_XOPT_CRLF: case A_XOPT_SELONLY:
    case A_IMP_COMMA: case A_IMP_SEMI: case A_IMP_TAB: case A_IMP_PIPE: case A_IMP_SPACE:
        return 1;
    default:
        return 0;
    }
}

/* The character for a delimiter action (export or import). */
static int act_delim(int act) {
    switch (act) {
    case A_XOPT_SEMI: case A_IMP_SEMI:   return ';';
    case A_XOPT_TAB:  case A_IMP_TAB:    return '\t';
    case A_XOPT_PIPE: case A_IMP_PIPE:   return '|';
    case A_XOPT_SPACE: case A_IMP_SPACE: return ' ';
    default:                             return ',';
    }
}

/* A new sheet holding a copy of the selected block, for "selection only" export.
   Formulas are copied as typed. The journal is paused so the copy is not undone. */
static Sheet *sel_copy(const Doc *d) {
    int r1, c1, r2, c2;
    doc_sel(d, &r1, &c1, &r2, &c2);
    if (r2 > d->g->max_r) r2 = d->g->max_r;
    if (c2 > d->g->max_c) c2 = d->g->max_c;
    Sheet *s = sheet_new();
    if (!s) return NULL;
    Txn *j = txn_pause();
    for (int r = r1; r <= r2; r++)
        for (int c = c1; c <= c2; c++) {
            const char *raw = sheet_get_raw(d->g, r, c);
            if (raw) sheet_put_raw(s, r - r1, c - c1, raw);
        }
    txn_resume(j);
    return s;
}

static void save_path(App *a, Doc *d, const char *path, int fmt) {
    char err[256], sheetname[64];
    strip_ext(d->name, sheetname, sizeof sheetname);
    Sheet *src = d->g, *tmp = NULL;
    if (a->xo.sel_only && d->next) {          /* one block only: never silently drop areas */
        snprintf(a->msg, sizeof a->msg, "selection only needs one area; clear the others first");
        return;
    }
    if (a->xo.sel_only) {
        tmp = sel_copy(d);
        if (!tmp) { snprintf(a->msg, sizeof a->msg, "out of memory"); return; }
        src = tmp;
    }
    int rc = io_save(src, sheetname, path, fmt, &a->xo, err, sizeof err);
    sheet_free(tmp);
    if (rc < 0) {
        snprintf(a->msg, sizeof a->msg, "%.250s", err);
        return;
    }
    if (a->xo.sel_only) {           /* the document keeps its own file name */
        d->dirty = 0;
        snprintf(a->msg, sizeof a->msg, "exported selection to %s (%s)", path, io_fmt_name(fmt));
        return;
    }
    free(d->path);
    d->path = strdup(path);
    d->fmt = fmt;
    snprintf(d->name, sizeof d->name, "%s", base(path));
    d->dirty = 0;
    snprintf(a->msg, sizeof a->msg, "saved %s (%s)", path, io_fmt_name(fmt));
}

static void close_doc(App *a) {
    Doc *d = D(a);
    if (d->dirty && a->armed != A_CLOSE) {
        a->armed = A_CLOSE;
        a->warn = 1;
        snprintf(a->msg, sizeof a->msg, "not saved or exported: Ctrl+S saves, Ctrl+W again closes it");
        return;
    }
    int i = a->cur;
    hist_clear(undo_s, &nundo);
    hist_clear(redo_s, &nredo);
    doc_free(&a->docs[i]);
    if (a->clip.doc == i) a->clip.cut = 0;
    else if (a->clip.doc > i) a->clip.doc--;
    memmove(&a->docs[i], &a->docs[i + 1], (size_t)(a->ndocs - i - 1) * sizeof(Doc));
    a->ndocs--;
    if (a->ndocs == 0) doc_new(a);
    if (a->cur >= a->ndocs) a->cur = a->ndocs - 1;
    a->armed = 0;
    snprintf(a->msg, sizeof a->msg, "sheet closed");
}

static void quit_app(App *a) {
    for (int i = 0; i < a->ndocs; i++)
        if (a->docs[i].dirty && a->armed != A_QUIT) {
            a->armed = A_QUIT;
            a->warn = 1;
            snprintf(a->msg, sizeof a->msg, "not saved or exported: Ctrl+S saves, Ctrl+Q again quits");
            return;
        }
    a->quit = 1;
}

/* ---------- prompts ---------- */

static void start_prompt(App *a, const char *label, const char *init, int act, int fmt) {
    a->mode = MODE_PROMPT;
    snprintf(a->msg, sizeof a->msg, "%s", label);
    snprintf(a->ebuf, sizeof a->ebuf, "%s", init);
    a->elen = a->epos = (int)strlen(a->ebuf);
    a->prompt_act = act;
    a->prompt_fmt = fmt;
}

static void ebuf_insert(App *a, int ch) {
    if (a->elen >= EBUF_MAX - 1) return;
    memmove(a->ebuf + a->epos + 1, a->ebuf + a->epos, (size_t)(a->elen - a->epos + 1));
    a->ebuf[a->epos++] = (char)ch;
    a->elen++;
}

static void ebuf_backspace(App *a) {
    if (a->epos <= 0) return;
    memmove(a->ebuf + a->epos - 1, a->ebuf + a->epos, (size_t)(a->elen - a->epos + 1));
    a->epos--;
    a->elen--;
}

static void ebuf_delete(App *a) {
    if (a->epos >= a->elen) return;
    memmove(a->ebuf + a->epos, a->ebuf + a->epos + 1, (size_t)(a->elen - a->epos));
    a->elen--;
}

/* ---------- find files ---------- */

typedef struct { char **v; int n, cap; long budget; } Found;

static void found_add(Found *f, const char *p) {
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 32;
        char **v = realloc(f->v, (size_t)cap * sizeof *v);
        if (!v) return;
        f->v = v;
        f->cap = cap;
    }
    f->v[f->n++] = strdup(p);
}

static int lower_eq_sub(const char *name, const char *pat) {
    size_t pl = strlen(pat);
    if (!pl) return 1;
    for (const char *n = name; *n; n++) {
        size_t k = 0;
        while (k < pl && n[k] && tolower((unsigned char)n[k]) == tolower((unsigned char)pat[k])) k++;
        if (k == pl) return 1;
    }
    return 0;
}

static int name_matches(const char *name, const char *pat) {
    if (pat[0] == '*' && pat[1] == '.') {          /* *.ext: suffix match */
        size_t nl = strlen(name), pl = strlen(pat) - 1;
        return nl >= pl && !strcasecmp(name + nl - pl, pat + 1);
    }
    return lower_eq_sub(name, pat);
}

static void walk(const char *dir, int depth, const char *pat, Found *f) {
    if (depth > 6 || f->n >= FIND_MAX || f->budget <= 0) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while (f->n < FIND_MAX && f->budget-- > 0 && (de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(path, &st) < 0) continue;      /* lstat: do not follow symlinks */
        if (S_ISDIR(st.st_mode)) walk(path, depth + 1, pat, f);
        else if (S_ISREG(st.st_mode) && name_matches(de->d_name, pat)) found_add(f, path);
    }
    closedir(dp);
}

static void list_free(App *a) {
    for (int i = 0; i < a->nlist; i++) free(a->list[i]);
    free(a->list);
    a->list = NULL;
    a->nlist = a->lsel = a->ltop = 0;
    a->lkind = LIST_NONE;
}

static void do_find(App *a, const char *pat) {
    Found f = {NULL, 0, 0, 200000};
    list_free(a);
    walk(".", 0, pat, &f);
    a->list = f.v;
    a->nlist = f.n;
    a->lkind = LIST_FILES;
    a->lsel = a->ltop = 0;
    if (f.n) {
        a->mode = MODE_LIST;
        snprintf(a->msg, sizeof a->msg, "%d file(s) found%s", f.n, f.n == FIND_MAX ? " (first 500)" : "");
    } else {
        a->mode = MODE_NORMAL;
        snprintf(a->msg, sizeof a->msg, "no files match \"%.200s\"", pat);
    }
}

static void open_help(App *a) {
    list_free(a);
    int n = 0;
    while (help_lines[n]) n++;
    a->list = calloc((size_t)n, sizeof *a->list);
    if (!a->list) return;
    for (int i = 0; i < n; i++) a->list[i] = strdup(help_lines[i]);
    a->nlist = n;
    a->lkind = LIST_HELP;
    a->mode = MODE_LIST;
}

/* ---------- selection and movement ---------- */

/* Moves the active selection. Plain moves (no Shift) drop the Ctrl+click areas. */
static void move_to(App *a, int r, int c, int extend);

static void move_keep(App *a, int r, int c, int extend) {
    Doc *d = D(a);
    if (r < 0) r = 0;
    if (r >= MAX_ROWS) r = MAX_ROWS - 1;
    if (c < 0) c = 0;
    if (c >= MAX_COLS) c = MAX_COLS - 1;
    d->cr = r;
    d->cc = c;
    d->sel_kind = 0;
    if (!extend) { d->ar = r; d->ac = c; }
    if (!d->g->nmg) return;
    /* a merged block touched by the selection is selected whole */
    int r1, c1, r2, c2;
    doc_sel(d, &r1, &c1, &r2, &c2);
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int i = 0; i < d->g->nmg; i++) {
            const Merge *m = &d->g->mg[i];
            if (m->r1 > r2 || m->r2 < r1 || m->c1 > c2 || m->c2 < c1) continue;
            if (m->r1 < r1) { r1 = m->r1; changed = 1; }
            if (m->c1 < c1) { c1 = m->c1; changed = 1; }
            if (m->r2 > r2) { r2 = m->r2; changed = 1; }
            if (m->c2 > c2) { c2 = m->c2; changed = 1; }
        }
    }
    /* the active cell stays where the user put it: top-left, or the moving corner */
    int down = extend ? (d->cr >= d->ar) : 0, right = extend ? (d->cc >= d->ac) : 0;
    d->cr = down ? r2 : r1;
    d->ar = down ? r1 : r2;
    d->cc = right ? c2 : c1;
    d->ac = right ? c1 : c2;
}

static void move_to(App *a, int r, int c, int extend) {
    if (!extend) D(a)->next = 0;
    move_keep(a, r, c, extend);
}

static int view_rows_n(App *a) {
    View v;
    view_build(a, &v);
    return v.nr ? v.nr : 1;
}

/* Scrolls so the cursor cell is fully on screen. */
static void ensure_visible(App *a) {
    Doc *d = D(a);
    View v;
    if (d->cr < d->top) d->top = d->cr;
    if (d->cc < d->left) d->left = d->cc;
    if (d->cr - d->top > a->rows) d->top = d->cr - a->rows / 3;
    if (d->cc - d->left > a->cols) d->left = d->cc - a->cols / 12;
    if (d->top < 0) d->top = 0;
    if (d->left < 0) d->left = 0;
    for (;;) {
        view_build(a, &v);
        int ok = 0;
        for (int i = 0; i < v.nr; i++) if (v.r[i] == d->cr) ok = 1;
        if (ok || d->top >= d->cr) break;
        d->top++;
    }
    for (;;) {
        view_build(a, &v);
        int ok = 0;
        for (int j = 0; j < v.nc; j++) if (v.c[j] == d->cc) ok = 1;
        if (ok || d->left >= d->cc) break;
        d->left++;
    }
}

/* Selection clipped to the used area, so whole-column selections stay cheap. */
static void used_sel(const Doc *d, int *r1, int *c1, int *r2, int *c2) {
    doc_sel(d, r1, c1, r2, c2);
    if (*r2 > d->g->max_r) *r2 = *r1 > d->g->max_r ? *r1 : d->g->max_r;
    if (*c2 > d->g->max_c) *c2 = *c1 > d->g->max_c ? *c1 : d->g->max_c;
}

static int in_selection(const Doc *d, int r, int c) { return doc_in_sel(d, r, c); }

/* ---------- selection areas (Ctrl+click, as in Excel) ---------- */

/* Keeps the active selection as an area; the new active one starts after it.
   An area already stored is not stored twice. */
static void area_push(App *a) {
    Doc *d = D(a);
    if (d->next >= MAX_SEL) {
        snprintf(a->msg, sizeof a->msg, "too many selection areas (max %d)", MAX_SEL);
        return;
    }
    Rect q;
    doc_sel(d, &q.r1, &q.c1, &q.r2, &q.c2);
    for (int i = 0; i < d->next; i++)
        if (d->ext_kind[i] == d->sel_kind && !memcmp(&d->ext[i], &q, sizeof q)) return;
    d->ext[d->next] = q;
    d->ext_kind[d->next] = d->sel_kind;
    d->next++;
}

/* Rectangle q minus rectangle s, as up to four pieces (top, bottom, left, right).
   Only called when they overlap. */
static int rect_minus(Rect q, Rect s, Rect out[4]) {
    Rect i = {q.r1 > s.r1 ? q.r1 : s.r1, q.c1 > s.c1 ? q.c1 : s.c1,
              q.r2 < s.r2 ? q.r2 : s.r2, q.c2 < s.c2 ? q.c2 : s.c2};
    int n = 0;
    if (i.r1 > q.r1) out[n++] = (Rect){q.r1, q.c1, i.r1 - 1, q.c2};
    if (i.r2 < q.r2) out[n++] = (Rect){i.r2 + 1, q.c1, q.r2, q.c2};
    if (i.c1 > q.c1) out[n++] = (Rect){i.r1, q.c1, i.r2, i.c1 - 1};
    if (i.c2 < q.c2) out[n++] = (Rect){i.r1, i.c2 + 1, i.r2, q.c2};
    return n;
}

static int rect_overlap(Rect q, Rect s) {
    return q.r1 <= s.r2 && s.r1 <= q.r2 && q.c1 <= s.c2 && s.c1 <= q.c2;
}

typedef struct { Rect q; int kind; } Area;

/* Removes the rectangle s from every area, active one included. A split area
   leaves cell pieces; if the active area is split, its last piece becomes the
   active selection. Changes nothing and says why if that would leave nothing
   selected, or need more than MAX_SEL stored areas. */
static int area_remove(App *a, Rect s) {
    Doc *d = D(a);
    static Area t[MAX_SEL * 5 + 5];
    int n = 0;
    for (int i = 0; i < d->next; i++) {
        if (!rect_overlap(d->ext[i], s)) {
            t[n].q = d->ext[i];
            t[n].kind = d->ext_kind[i];
            n++;
            continue;
        }
        Rect pc[4];
        int k = rect_minus(d->ext[i], s, pc);
        for (int j = 0; j < k; j++) { t[n].q = pc[j]; t[n].kind = 0; n++; }
    }
    Rect act;
    doc_sel(d, &act.r1, &act.c1, &act.r2, &act.c2);
    int act_hit = rect_overlap(act, s);
    Rect nact = act;                            /* the new active area, applied only on success */
    if (act_hit) {
        Rect pc[4];
        int k = rect_minus(act, s, pc);
        for (int j = 0; j < k; j++) { t[n].q = pc[j]; t[n].kind = 0; n++; }
        if (n == 0) {
            snprintf(a->msg, sizeof a->msg, "nothing else is selected");
            return 0;
        }
        n--;                                    /* the last piece becomes the active area */
        nact = t[n].q;
    }
    if (n > MAX_SEL) {
        snprintf(a->msg, sizeof a->msg, "too many selection areas (max %d)", MAX_SEL);
        return 0;
    }
    if (act_hit) {                              /* nothing has changed until here */
        d->ar = nact.r1;
        d->ac = nact.c1;
        d->cr = nact.r2;
        d->cc = nact.c2;
        d->sel_kind = 0;
    }
    for (int i = 0; i < n; i++) { d->ext[i] = t[i].q; d->ext_kind[i] = t[i].kind; }
    d->next = n;
    return 1;
}

/* Commands Excel refuses on several areas: copy, cut, paste, fill, sort, merge, graph. */
static int single_only(App *a) {
    if (!D(a)->next) return 0;
    snprintf(a->msg, sizeof a->msg, "this command cannot be used on multiple selections");
    return 1;
}

static int nonempty(const Doc *d, int r, int c) {
    const char *s = sheet_get(d->g, r, c);
    return s && *s;
}

/* Ctrl+arrow: jump to the edge of the current run of filled cells (Excel rules). */
static void jump(App *a, int dr, int dc, int ext) {
    Doc *d = D(a);
    int r = d->cr, c = d->cc;
    int nr = r + dr, nc = c + dc;
    if (nr < 0 || nr >= MAX_ROWS || nc < 0 || nc >= MAX_COLS) return;
    if (nonempty(d, nr, nc)) {
        while (nr + dr >= 0 && nr + dr < MAX_ROWS && nc + dc >= 0 && nc + dc < MAX_COLS &&
               nonempty(d, nr + dr, nc + dc)) { nr += dr; nc += dc; }
    } else {
        while (nr + dr >= 0 && nr + dr < MAX_ROWS && nc + dc >= 0 && nc + dc < MAX_COLS &&
               !nonempty(d, nr, nc)) { nr += dr; nc += dc; }
    }
    move_to(a, nr, nc, ext);
}

static void select_cols(App *a, int c, int extend) {
    Doc *d = D(a);
    if (!extend) { d->ac = c; d->cr = 0; }
    d->cc = c;
    d->sel_kind = 1;
}

static void select_rows(App *a, int r, int extend) {
    Doc *d = D(a);
    if (!extend) { d->ar = r; d->cc = 0; }
    d->cr = r;
    d->sel_kind = 2;
}

/* ---------- formats ----------
   A cell's format holds bold, highlight, alignment and the body (see sheet.c).
   Arguments: align 0 keeps it, body NULL keeps it, bold and hl -1 keep them. */

static const char *fmt_body_of(const char *f) {
    int a, b, h;
    const char *body;
    fmt_parse(f, &a, &b, &h, &body);
    return body;
}

static void fmt_apply(Sheet *g, int r, int c, char align, const char *body, int bold, int hl) {
    int oa, ob, oh;
    const char *obody;
    fmt_parse(sheet_fmt(g, r, c), &oa, &ob, &oh, &obody);
    int na = align == 'x' ? 0 : align ? align : oa;      /* 'x': back to the default alignment */
    const char *nb = body ? body : obody;
    int nbold = bold < 0 ? ob : bold;
    int nhl = hl < 0 ? oh : hl;
    char buf[320];
    fmt_build(buf, sizeof buf, na, nbold, nhl, nb);
    const char *cur = sheet_fmt(g, r, c);
    if (strcmp(cur ? cur : "", buf) == 0) return;      /* no change: no write, no undo entry */
    sheet_fmt_set(g, r, c, buf);
}

/* ---------- cell editing ---------- */

static void begin_edit(App *a, int first) {
    Doc *d = D(a);
    a->mode = MODE_EDIT;
    if (first) {
        a->ebuf[0] = (char)first;
        a->ebuf[1] = '\0';
        a->elen = a->epos = 1;
    } else {
        const char *raw = sheet_get(d->g, d->cr, d->cc);
        snprintf(a->ebuf, sizeof a->ebuf, "%s", raw ? raw : "");
        a->elen = a->epos = (int)strlen(a->ebuf);
    }
}

static void end_edit(App *a, int commit) {
    Doc *d = D(a);
    if (commit) {
        long long t;
        int kind;
        if (a->ebuf[0] && parse_datetime(a->ebuf, &t, &kind) == 0) {
            /* typed dates and times are stored as unix seconds */
            char num[32];
            snprintf(num, sizeof num, "%lld", t);
            sheet_set(d->g, d->cr, d->cc, num);
            const char *body = fmt_body_of(sheet_fmt(d->g, d->cr, d->cc));
            if (body[0] != '%')                     /* no time format yet: pick a default */
                fmt_apply(d->g, d->cr, d->cc, 0,
                          kind == 3 ? "%H:%M:%S" : kind == 2 ? "%Y-%m-%d %H:%M" : "%Y-%m-%d", -1, -1);
        } else {
            sheet_set(d->g, d->cr, d->cc, a->ebuf);
        }
        d->dirty = 1;
    }
    a->mode = MODE_NORMAL;
}

/* ---------- blocks: rectangular copies of cell text ---------- */

static Block blk_new(int rows, int cols) {
    Block b = {rows, cols, NULL, NULL, 0, NULL};
    b.v = calloc((size_t)(rows * cols > 0 ? rows * cols : 1), sizeof *b.v);
    return b;
}

static void blk_free(Block *b) {
    if (b->v) {
        for (int i = 0; i < b->rows * b->cols; i++) free(b->v[i]);
        free(b->v);
    }
    if (b->f) {
        for (int i = 0; i < b->rows * b->cols; i++) free(b->f[i]);
        free(b->f);
    }
    free(b->mg);
    memset(b, 0, sizeof *b);
}

static const char *blk_at(const Block *b, int r, int c) { return b->v[r * b->cols + c]; }

static Block blk_sel(const Doc *d, int r1, int c1, int r2, int c2, int disp) {
    Block b = blk_new(r2 - r1 + 1, c2 - c1 + 1);
    for (int r = r1; r <= r2; r++)
        for (int c = c1; c <= c2; c++) {
            const Merge *m = sheet_merge_of(d->g, r, c);
            if (m && (m->r1 != r || m->c1 != c)) continue;        /* covered: the block holds it */
            char *val = NULL;
            if (!disp) {
                const char *raw = sheet_get_raw(d->g, r, c);
                if (raw && *raw) val = strdup(raw);
            } else {
                char buf[256];
                int align;
                cell_display(d->g, r, c, buf, sizeof buf, &align);
                if (buf[0]) val = strdup(buf);
            }
            b.v[(r - r1) * b.cols + (c - c1)] = val;
        }
    for (int i = 0; i < d->g->nmg; i++) {
        const Merge *m = &d->g->mg[i];
        if (m->r1 < r1 || m->r2 > r2 || m->c1 < c1 || m->c2 > c2) continue;
        if (!b.mg) b.mg = calloc((size_t)d->g->nmg, sizeof *b.mg);
        if (!b.mg) break;
        b.mg[b.nm].r1 = m->r1 - r1;
        b.mg[b.nm].c1 = m->c1 - c1;
        b.mg[b.nm].r2 = m->r2 - r1;
        b.mg[b.nm].c2 = m->c2 - c1;
        b.nm++;
    }
    return b;
}

static Block blk_copy(const Block *s) {
    Block b = blk_new(s->rows, s->cols);
    for (int i = 0; i < s->rows * s->cols; i++)
        if (s->v[i]) b.v[i] = strdup(s->v[i]);
    if (s->nm) {
        b.mg = malloc((size_t)s->nm * sizeof *b.mg);
        if (b.mg) {
            memcpy(b.mg, s->mg, (size_t)s->nm * sizeof *b.mg);
            b.nm = s->nm;
        }
    }
    if (s->f) {
        b.f = calloc((size_t)(s->rows * s->cols > 0 ? s->rows * s->cols : 1), sizeof *b.f);
        if (b.f)
            for (int i = 0; i < s->rows * s->cols; i++)
                if (s->f[i]) b.f[i] = strdup(s->f[i]);
    }
    return b;
}

/* Transposes in place: moves the pointers, the old block is left empty. */
static Block blk_transpose(Block *b) {
    Block t = blk_new(b->cols, b->rows);
    for (int r = 0; r < b->rows; r++)
        for (int c = 0; c < b->cols; c++) {
            t.v[c * t.cols + r] = b->v[r * b->cols + c];
            b->v[r * b->cols + c] = NULL;
        }
    if (b->nm) {
        t.mg = malloc((size_t)b->nm * sizeof *t.mg);
        if (t.mg) {
            for (int i = 0; i < b->nm; i++) {
                const Merge *m = &b->mg[i];
                t.mg[i].r1 = m->c1; t.mg[i].c1 = m->r1;
                t.mg[i].r2 = m->c2; t.mg[i].c2 = m->r2;
            }
            t.nm = b->nm;
        }
    }
    if (b->f) {
        t.f = calloc((size_t)(b->rows * b->cols > 0 ? b->rows * b->cols : 1), sizeof *t.f);
        for (int r = 0; t.f && r < b->rows; r++)
            for (int c = 0; c < b->cols; c++) {
                t.f[c * t.cols + r] = b->f[r * b->cols + c];
                b->f[r * b->cols + c] = NULL;
            }
    }
    return t;
}

/* Tab/newline separated text. Tabs and newlines inside cells become spaces. */
static char *blk_tsv(const Block *b) {
    Buf o = {NULL, 0, 0};
    for (int r = 0; r < b->rows; r++) {
        for (int c = 0; c < b->cols; c++) {
            const char *t = blk_at(b, r, c);
            for (; t && *t; t++) {
                char ch = (*t == '\t' || *t == '\n' || *t == '\r') ? ' ' : *t;
                buf_put(&o, &ch, 1);
            }
            if (c + 1 < b->cols) buf_put(&o, "\t", sizeof("\t") - 1);
        }
        if (r + 1 < b->rows) buf_put(&o, "\n", sizeof("\n") - 1);
    }
    if (!o.s) return strdup("");
    return o.s;
}

static Block blk_from_tsv(const char *t) {
    size_t len = strlen(t);
    if (len && t[len - 1] == '\n') len--;
    int rows = 1, cols = 1, cur = 1;
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '\t') cur++;
        else if (t[i] == '\n') { rows++; if (cur > cols) cols = cur; cur = 1; }
    }
    if (cur > cols) cols = cur;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols > MAX_COLS) cols = MAX_COLS;
    Block b = blk_new(rows, cols);
    int r = 0, c = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || t[i] == '\t' || t[i] == '\n') {
            /* whitespace around a field is not part of it; a blank field is an empty cell */
            size_t fs = start, fe = i;
            if (fe > fs && t[fe - 1] == '\r') fe--;
            while (fs < fe && isspace((unsigned char)t[fs])) fs++;
            while (fe > fs && isspace((unsigned char)t[fe - 1])) fe--;
            if (fe > fs && r < rows && c < cols)
                b.v[r * cols + c] = strndup(t + fs, fe - fs);
            if (i < len && t[i] == '\t') c++;
            else { r++; c = 0; }
            start = i + 1;
        }
    }
    return b;
}

static Block blk_from_html(HTable *t) {
    Block b = blk_new(t->rows, t->cols);
    int spans = 0;
    for (size_t i = 0; i < t->n; i++) if (t->cells[i].rs > 1 || t->cells[i].cs > 1) spans++;
    if (spans) b.mg = calloc((size_t)spans, sizeof *b.mg);
    for (size_t i = 0; i < t->n; i++) {
        HCell *h = &t->cells[i];
        if (h->r < t->rows && h->c < t->cols) {
            b.v[h->r * t->cols + h->c] = h->text;
            h->text = NULL;
        }
        if ((h->rs > 1 || h->cs > 1) && b.mg && b.nm < spans) {
            b.mg[b.nm].r1 = h->r;
            b.mg[b.nm].c1 = h->c;
            b.mg[b.nm].r2 = h->r + h->rs - 1;
            b.mg[b.nm].c2 = h->c + h->cs - 1;
            b.nm++;
        }
    }
    b.f = calloc((size_t)(b.rows * b.cols > 0 ? b.rows * b.cols : 1), sizeof *b.f);
    for (size_t i = 0; b.f && i < t->n; i++) {
        HCell *h = &t->cells[i];
        if (h->r >= t->rows || h->c >= t->cols) continue;
        int idx = h->r * t->cols + h->c;
        char raw[160], body[200] = "", fmt[256];
        NumSpec ns;
        char *txt = b.v[idx];
        if (txt && txt[0] != '=' && num_from_text(txt, raw, sizeof raw, &ns)) {
            free(txt);
            b.v[idx] = strdup(raw);
            if (ns.style != '-' || ns.dec >= 0 || ns.pre[0] || ns.suf[0]) num_build(body, sizeof body, &ns);
        }
        fmt_build(fmt, sizeof fmt, h->align, h->bold, h->hl, body);
        b.f[idx] = strdup(fmt);
    }
    return b;
}

static void clip_clear(App *a) {
    blk_free(&a->clip.raw);
    blk_free(&a->clip.disp);
    free(a->clip.text);
    free(a->clip.html);
    a->clip.text = a->clip.html = NULL;
    a->clip.valid = a->clip.cut = a->clip.values = 0;
}

static void fmt_sel(App *a, char align, const char *body, int bold, int hl) {
    Doc *d = D(a);
    Rect q[MAX_RECTS];
    int n = doc_rects(d, q);
    for (int i = 0; i < n; i++)
        for (int r = q[i].r1; r <= q[i].r2; r++)
            for (int c = q[i].c1; c <= q[i].c2; c++) fmt_apply(d->g, r, c, align, body, bold, hl);
    d->dirty = 1;
}

/* Bold or highlight: turns it off if the selection's first cell has it, else on. */
static void fmt_toggle(App *a, int highlight) {
    /* every cell has it: it is removed from all; otherwise it is added to all */
    int off = sel_all_fmt(a, highlight ? 1 : 0, 0);
    if (highlight) fmt_sel(a, 0, NULL, -1, !off);
    else fmt_sel(a, 0, NULL, !off, -1);
}

/* ---------- copy, cut, paste ---------- */

/* Appends s to a buffer, escaping the characters LaTeX treats specially. */
static void latex_escape(Buf *o, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '\\': buf_put(o, "\\textbackslash{}", sizeof("\\textbackslash{}") - 1); break;
        case '&': case '%': case '$': case '#': case '_': case '{': case '}':
            buf_put(o, "\\", sizeof("\\") - 1);
            buf_put(o, s, 1);
            break;
        case '~': buf_put(o, "\\textasciitilde{}", sizeof("\\textasciitilde{}") - 1); break;
        case '^': buf_put(o, "\\textasciicircum{}", sizeof("\\textasciicircum{}") - 1); break;
        default: buf_put(o, s, 1);
        }
    }
}

/* A tabular environment for the selection. Columns are right-aligned when every
   non-empty cell in them is a number, otherwise left-aligned. */
/* LaTeX tabular for a range, following its merged blocks: \multicolumn for
   merged columns, \multirow for merged rows. Bold is \textbf, highlight is
   \colorbox. Needs \usepackage{multirow,xcolor} in the document. */
static char *latex_range(const Doc *d, int r1, int c1, int r2, int c2) {
    int ncols = c2 - c1 + 1;
    char *spec = malloc((size_t)ncols + 1);
    if (!spec) return strdup("");
    for (int c = c1; c <= c2; c++) {
        int numeric = 1, any = 0;
        for (int r = r1; r <= r2; r++) {
            const Merge *m = sheet_merge_of(d->g, r, c);
            if (m && (m->r1 != r || m->c1 != c)) continue;
            if (!sheet_get(d->g, r, c)) continue;
            char buf[256];
            int align;
            any = 1;
            cell_display(d->g, r, c, buf, sizeof buf, &align);
            if (align != 1) numeric = 0;
        }
        spec[c - c1] = (any && numeric) ? 'r' : 'l';
    }
    spec[ncols] = '\0';
    Buf o = {NULL, 0, 0};
    buf_put(&o, "% needs \\usepackage{multirow,xcolor}\n", sizeof("% needs \\usepackage{multirow,xcolor}\n") - 1);
    buf_put(&o, "\\begin{tabular}{", sizeof("\\begin{tabular}{") - 1);
    buf_put(&o, spec, (size_t)ncols);
    buf_put(&o, "}\n\\hline\n", sizeof("}\n\\hline\n") - 1);
    free(spec);
    for (int r = r1; r <= r2; r++) {
        int first = 1;
        for (int c = c1; c <= c2; c++) {
            const Merge *m = sheet_merge_of(d->g, r, c);
            int covered = m && (m->r1 != r || m->c1 != c);
            if (covered && m->r1 == r) continue;          /* inside a \multicolumn */
            if (!first) buf_put(&o, " & ", 3);
            first = 0;
            if (covered) continue;                        /* empty: covered from above */
            int cs = m ? (m->c2 < c2 ? m->c2 : c2) - c + 1 : 1;
            int rs = m ? (m->r2 < r2 ? m->r2 : r2) - r + 1 : 1;
            char txt[256];
            int align = 0, bold = 0, hl = 0;
            cell_display(d->g, r, c, txt, sizeof txt, &align);
            sheet_style(d->g, r, c, &bold, &hl);
            Buf t = {NULL, 0, 0};
            if (hl) buf_put(&t, "\\colorbox{yellow}{", sizeof("\\colorbox{yellow}{") - 1);
            if (bold) buf_put(&t, "\\textbf{", sizeof("\\textbf{") - 1);
            latex_escape(&t, txt);
            if (bold) buf_put(&t, "}", 1);
            if (hl) buf_put(&t, "}", 1);
            if (rs > 1) {
                char pre[48];
                snprintf(pre, sizeof pre, "\\multirow{%d}{*}{", rs);
                Buf w = {NULL, 0, 0};
                buf_put(&w, pre, strlen(pre));
                buf_put(&w, t.s ? t.s : "", t.n);
                buf_put(&w, "}", 1);
                buf_free(&t);
                t = w;
            }
            if (cs > 1) {
                char pre[48];
                char a = align == 1 ? 'r' : align == 2 ? 'c' : 'l';
                snprintf(pre, sizeof pre, "\\multicolumn{%d}{%c}{", cs, a);
                buf_put(&o, pre, strlen(pre));
                buf_put(&o, t.s ? t.s : "", t.n);
                buf_put(&o, "}", 1);
            } else {
                buf_put(&o, t.s ? t.s : "", t.n);
            }
            buf_free(&t);
        }
        buf_put(&o, " \\\\\n\\hline\n", sizeof(" \\\\\n\\hline\n") - 1);
    }
    buf_put(&o, "\\end{tabular}\n", sizeof("\\end{tabular}\n") - 1);
    if (!o.s) return strdup("");
    return o.s;
}

/* mode: 0 formulas and values, 1 values only, 2 with formatting (HTML), 3 LaTeX table */
static void do_copy(App *a, int cut, int mode) {
    if (single_only(a)) return;
    Doc *d = D(a);
    int r1, c1, r2, c2;
    used_sel(d, &r1, &c1, &r2, &c2);
    Block raw = blk_sel(d, r1, c1, r2, c2, 0);
    Block disp = blk_sel(d, r1, c1, r2, c2, 1);
    clip_clear(a);
    a->clip.raw = raw;
    a->clip.disp = disp;
    a->clip.r0 = r1;
    a->clip.c0 = c1;
    a->clip.cut = cut;
    a->clip.values = mode == 1;
    a->clip.valid = 1;
    a->clip.doc = a->cur;
    char *text = mode == 3 ? latex_range(d, r1, c1, r2, c2)
                 : blk_tsv(mode == 1 || mode == 2 ? &disp : &raw);     /* values and formatting: as shown */
    char *html = mode == 2 ? html_build(d->g, r1, c1, r2, c2) : NULL;
    clip_set_rich(text, html);
    if (cut) {                            /* the cells move to the clipboard */
        sheet_unmerge(d->g, r1, c1, r2, c2);
        for (int r = r1; r <= r2; r++)
            for (int c = c1; c <= c2; c++) sheet_set(d->g, r, c, NULL);
        d->dirty = 1;
    }
    a->clip.text = text;
    a->clip.html = html;
    snprintf(a->msg, sizeof a->msg, "%s %dx%d%s (%s)", cut ? "cut" : "copied",
             r2 - r1 + 1, c2 - c1 + 1, html ? " with formatting" : "", clip_backend());
}

static void store(Sheet *g, int r, int c, const char *raw, int dr, int dc) {
    if (raw[0] == '=' && (dr || dc)) {
        char *f = formula_shift(raw, dr, dc);
        sheet_set(g, r, c, f);
        free(f);
    } else sheet_set(g, r, c, raw);
}

/* Writes a block at the cursor. Used when the clipboard holds only HTML. */
static void blk_merges_apply(App *a, const Block *b, int r0, int c0) {
    Doc *d = D(a);
    for (int i = 0; i < b->nm; i++) {
        const Merge *m = &b->mg[i];
        if (r0 + m->r2 >= MAX_ROWS || c0 + m->c2 >= MAX_COLS) continue;
        sheet_merge_add(d->g, r0 + m->r1, c0 + m->c1, r0 + m->r2, c0 + m->c2);
    }
}

/* Sets the format of a pasted cell: the source's own format when it has one (HTML),
   else a number with a prefix or suffix (text "$1,234.50", "12 kg") becomes a number
   with that format. Plain text clears the format, as a paste from another book does. */
static void paste_fmt(Doc *d, int rr, int cc, const Block *src, int idx, const char *t) {
    if (src->f) {
        sheet_fmt_set(d->g, rr, cc, src->f[idx] ? src->f[idx] : "");
        return;
    }
    char body[200] = "", raw[160];
    NumSpec ns;
    if (t && t[0] != '=' && num_from_text(t, raw, sizeof raw, &ns)) {
        sheet_set(d->g, rr, cc, raw);
        if (ns.style != '-' || ns.dec >= 0 || ns.pre[0] || ns.suf[0]) num_build(body, sizeof body, &ns);
    }
    sheet_fmt_set(d->g, rr, cc, body);
}

static void paste_block_at_cursor(App *a, const Block *src) {
    Doc *d = D(a);
    for (int r = 0; r < src->rows; r++)
        for (int c = 0; c < src->cols; c++) {
            const char *t = blk_at(src, r, c);
            if (d->cr + r < MAX_ROWS && d->cc + c < MAX_COLS) {
                sheet_set(d->g, d->cr + r, d->cc + c, t ? t : "");
                paste_fmt(d, d->cr + r, d->cc + c, src, r * src->cols + c, t);
            }
        }
    blk_merges_apply(a, src, d->cr, d->cc);
    d->ar = d->cr;
    d->ac = d->cc;
    move_to(a, d->cr + src->rows - 1, d->cc + src->cols - 1, 1);
    d->dirty = 1;
    snprintf(a->msg, sizeof a->msg, "pasted %dx%d", src->rows, src->cols);
}

/* act: A_PASTE, A_PASTE_VALUES, A_PASTE_FORMULAS, A_PASTE_FMT, A_PASTE_TRANSPOSE */
static void do_paste(App *a, int act) {
    if (single_only(a)) return;
    Doc *d = D(a);
    char *txt = clip_get();
    if (!txt || !*txt) {
        /* no plain text: an HTML-only clipboard (some apps offer just a table) */
        free(txt);
        char *html = clip_get_html();
        HTable t;
        if (html && html_parse_table(html, &t)) {
            Block b = blk_from_html(&t);
            html_table_free(&t);
            free(html);
            paste_block_at_cursor(a, &b);
            blk_free(&b);
            return;
        }
        free(html);
        snprintf(a->msg, sizeof a->msg, "clipboard is empty");
        return;
    }
    int internal = a->clip.valid && a->clip.text && !strcmp(txt, a->clip.text);
    Block own = {0, 0, NULL, NULL, 0, NULL};
    int owned = 0, shift = 0, dr = 0, dc = 0;
    const Block *src;
    if (internal) {
        int values = act == A_PASTE_VALUES || a->clip.values;
        src = values ? &a->clip.disp : &a->clip.raw;
        if (!values) { shift = 1; dr = d->cr - a->clip.r0; dc = d->cc - a->clip.c0; }
        if (a->clip.cut) {                    /* a cut already cleared its cells: move, don't shift */
            a->clip.cut = 0;
            shift = 0;
            dr = dc = 0;
        }
    } else {
        /* Ctrl+V and menu paste prefer an HTML table (web pages, LibreOffice) */
        if (act == A_PASTE_FMT || act == A_PASTE) {
            char *html = clip_get_html();
            if (html) {
                HTable t;
                if (html_parse_table(html, &t)) {
                    own = blk_from_html(&t);
                    owned = 1;
                }
                html_table_free(&t);
                free(html);
            }
        }
        if (!owned) { own = blk_from_tsv(txt); owned = 1; }
        src = &own;
    }
    if (act == A_PASTE_TRANSPOSE) {
        if (!owned) { own = blk_copy(src); owned = 1; }
        Block t = blk_transpose(&own);
        blk_free(&own);
        own = t;
        src = &own;
    }
    int rows = src->rows, cols = src->cols;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const char *t = blk_at(src, r, c);
            int rr = d->cr + r, cc = d->cc + c;
            if (rr < MAX_ROWS && cc < MAX_COLS) {
                store(d->g, rr, cc, t ? t : "", shift ? dr : 0, shift ? dc : 0);
                if (!internal) paste_fmt(d, rr, cc, src, r * src->cols + c, t);
            }
        }
    blk_merges_apply(a, src, d->cr, d->cc);
    d->ar = d->cr;
    d->ac = d->cc;
    move_to(a, d->cr + rows - 1, d->cc + cols - 1, 1);
    d->dirty = 1;
    snprintf(a->msg, sizeof a->msg, "pasted %dx%d", rows, cols);
    if (owned) blk_free(&own);
    free(txt);
}

static void do_clear(App *a) {
    Doc *d = D(a);
    Rect q[MAX_RECTS];
    int n = doc_rects(d, q);
    for (int i = 0; i < n; i++)
        for (int r = q[i].r1; r <= q[i].r2; r++)
            for (int c = q[i].c1; c <= q[i].c2; c++)
                if (sheet_get(d->g, r, c)) sheet_set(d->g, r, c, NULL);   /* empty cells need no write */
    d->dirty = 1;
}

/* Fill down copies the top row; fill right copies the left column. */
static void do_fill(App *a, int down) {
    if (single_only(a)) return;
    Doc *d = D(a);
    int r1, c1, r2, c2;
    used_sel(d, &r1, &c1, &r2, &c2);
    if (down) {
        for (int c = c1; c <= c2; c++) {
            const char *src = sheet_get(d->g, r1, c);
            if (!src) src = "";
            for (int r = r1 + 1; r <= r2; r++) store(d->g, r, c, src, r - r1, 0);
        }
    } else {
        for (int r = r1; r <= r2; r++) {
            const char *src = sheet_get(d->g, r, c1);
            if (!src) src = "";
            for (int c = c1 + 1; c <= c2; c++) store(d->g, r, c, src, 0, c - c1);
        }
    }
    d->dirty = 1;
}

/* ---------- fill handle: drag the square at a selection's corner ----------
   Like Excel: a run of two or more numbers continues as a linear series, and
   anything else repeats the selection's pattern (formulas shift their references). */

/* Fills positions t1..t2 from the source positions s1..s2 of one line.
   vertical: the line is column `fixed` and positions are rows; else row `fixed`. */
static void fill_line(Sheet *g, int vertical, int fixed, int s1, int s2, int t1, int t2) {
    int n = s2 - s1 + 1;
    int numeric = n >= 2;
    double last = 0, prev = 0;
    for (int p = s1; p <= s2 && numeric; p++) {
        int r = vertical ? p : fixed, c = vertical ? fixed : p;
        const char *raw = sheet_get(g, r, c);
        double x;
        if (!raw || raw[0] == '=' || cell_num(g, r, c, 0, &x) != 0) { numeric = 0; break; }
        prev = last;
        last = x;
    }
    double step = last - prev;
    for (int t = t1; t <= t2; t++) {
        int r = vertical ? t : fixed, c = vertical ? fixed : t;
        if (numeric) {
            char buf[64];
            snprintf(buf, sizeof buf, "%.15g", last + step * (t - s2));
            sheet_set(g, r, c, buf);
        } else {
            int src = s1 + (t - s1) % n;
            const char *raw = sheet_get(g, vertical ? src : fixed, vertical ? fixed : src);
            int off = t - src;
            store(g, r, c, raw ? raw : "", vertical ? off : 0, vertical ? 0 : off);
        }
    }
}

/* The cells a drag from the handle covers: downwards or to the right, whichever
   the pointer has moved further. */
static void fill_track(App *a, int row, int col) {
    Doc *d = D(a);
    int r1, c1, r2, c2;
    doc_sel(d, &r1, &c1, &r2, &c2);
    int dv = row - r2, dh = col - c2;
    a->fr1 = a->fc1 = 0;
    a->fr2 = a->fc2 = -1;
    if (dv > 0 && dv >= dh) {
        a->fill_axis = 1;
        a->fr1 = r2 + 1; a->fr2 = row; a->fc1 = c1; a->fc2 = c2;
    } else if (dh > 0) {
        a->fill_axis = 2;
        a->fr1 = r1; a->fr2 = r2; a->fc1 = c2 + 1; a->fc2 = col;
    }
}

static void fill_apply(App *a) {
    Doc *d = D(a);
    if (a->fr2 < a->fr1 || a->fc2 < a->fc1) return;      /* dragged back inside: nothing */
    int r1, c1, r2, c2;
    doc_sel(d, &r1, &c1, &r2, &c2);
    if (a->fill_axis == 1) {
        for (int c = c1; c <= c2; c++) fill_line(d->g, 1, c, r1, r2, r2 + 1, a->fr2);
        d->cr = a->fr2;
        d->cc = c2;
    } else {
        for (int r = r1; r <= r2; r++) fill_line(d->g, 0, r, c1, c2, c2 + 1, a->fc2);
        d->cr = r2;
        d->cc = a->fc2;
    }
    d->ar = r1;
    d->ac = c1;
    d->sel_kind = 0;
    d->dirty = 1;
    char cn[8];
    col_name(d->cc, cn);
    snprintf(a->msg, sizeof a->msg, "filled to %s%d", cn, d->cr + 1);
    a->fr2 = a->fc2 = -1;
}

/* Is (x,y) on the handle of the selection? The handle is the bottom-right
   corner of the selection's last cell. */
static int fill_handle_hit(App *a, const View *v, int x, int y) {
    Doc *d = D(a);
    if (d->sel_kind) return 0;
    int r1, c1, r2, c2;
    doc_sel(d, &r1, &c1, &r2, &c2);
    for (int i = 0; i < v->nr; i++)
        for (int j = 0; j < v->nc; j++)
            if (v->r[i] == r2 && v->c[j] == c2) {
                int hx = v->x[j] + v->w[j] + 1, hy = v->y[i] + v->h[i];
                return x >= hx - 1 && x <= hx + 1 && y == hy;
            }
    return 0;
}

/* ---------- menus ---------- */

static int item_ok(const MenuItem *it) { return it->label && strcmp(it->label, "-") != 0; }

static int first_ok(const MenuItem *items) {
    for (int i = 0; items[i].label; i++) if (item_ok(&items[i])) return i;
    return 0;
}

static int bar_count(void) {
    int n = 0;
    while (menubar[n].title) n++;
    return n;
}

static void menu_close(App *a) {
    a->npanels = 0;
    a->mb = -1;
}

static void menu_open_bar(App *a, int i) {
    if (!menubar[i].items) { dialog_open(a); return; }   /* Graph: centred dialog */
    a->mb = i;
    a->pan[0].items = menubar[i].items;
    a->pan[0].x = ui_menu_x(i);
    a->pan[0].y = 1;
    a->pan[0].sel = first_ok(menubar[i].items);
    a->npanels = 1;
}

static void menu_open_ctx(App *a, int x, int y) {
    int w = ui_panel_w(context_menu), h = ui_panel_h(context_menu);
    if (x > a->cols - w) x = a->cols - w;
    if (x < 0) x = 0;
    if (y > a->rows - 2 - h) y = a->rows - 2 - h;
    if (y < 1) y = 1;
    a->mb = -1;
    a->pan[0].items = context_menu;
    a->pan[0].x = x;
    a->pan[0].y = y;
    a->pan[0].sel = first_ok(context_menu);
    a->npanels = 1;
}

/* Opens the submenu of the selected item in the top-level panel. */
static void menu_open_sub(App *a) {
    if (a->npanels != 1) return;
    Panel *p = &a->pan[0];
    const MenuItem *it = &p->items[p->sel];
    if (!it->sub) return;
    Panel s;
    s.items = it->sub;
    s.sel = first_ok(it->sub);
    int pw = ui_panel_w(p->items), sw = ui_panel_w(it->sub), sh = ui_panel_h(it->sub);
    s.x = p->x + pw - 1;
    if (s.x + sw > a->cols) s.x = p->x - sw + 1;
    if (s.x < 0) s.x = 0;
    s.y = p->y + 1 + p->sel;
    if (s.y + sh > a->rows - 2) s.y = a->rows - 2 - sh;
    if (s.y < 0) s.y = 0;
    a->pan[1] = s;
    a->npanels = 2;
}

static void menu_run_item(App *a, const MenuItem *it) {
    int act = it->act;
    menu_close(a);
    run_action(a, act);
}

static void menu_activate(App *a) {
    Panel *p = &a->pan[a->npanels - 1];
    const MenuItem *it = &p->items[p->sel];
    if (!item_ok(it)) return;
    if (it->sub) { menu_open_sub(a); return; }
    menu_run_item(a, it);
}

static void menu_move(App *a, int dir) {
    Panel *p = &a->pan[a->npanels - 1];
    int n = 0;
    while (p->items[n].label) n++;
    if (!n) return;
    int i = p->sel;
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (item_ok(&p->items[i])) break;
    }
    p->sel = i;
}

static void menu_key(App *a, const Event *e) {
    switch (e->key) {
    case K_UP:    menu_move(a, -1); break;
    case K_DOWN:  menu_move(a, 1); break;
    case K_RIGHT:
        if (a->npanels == 1 && a->pan[0].items[a->pan[0].sel].sub) menu_open_sub(a);
        else if (a->mb >= 0 && a->npanels == 1) menu_open_bar(a, (a->mb + 1) % bar_count());
        break;
    case K_LEFT:
        if (a->npanels == 2) a->npanels = 1;
        else if (a->mb >= 0) menu_open_bar(a, (a->mb + bar_count() - 1) % bar_count());
        break;
    case '\r': case '\n': menu_activate(a); break;
    default: menu_close(a);                              /* Esc or anything else */
    }
}

/* Index of the item under (y,x) in panel p, or -1. */
static int panel_hit(const Panel *p, int y, int x) {
    int w = ui_panel_w(p->items), h = ui_panel_h(p->items);
    if (x < p->x || x >= p->x + w || y <= p->y || y >= p->y + h - 1) return -1;
    int i = y - p->y - 1;
    return item_ok(&p->items[i]) ? i : -2;               /* -2: separator */
}

/* ---------- list (find results, help) ---------- */

static void list_activate(App *a) {
    if (a->lkind == LIST_FILES && a->lsel < a->nlist) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s", a->list[a->lsel]);
        list_free(a);
        a->mode = MODE_NORMAL;
        load_path(a, path);
        return;
    }
    list_free(a);
    a->mode = MODE_NORMAL;
}

static void list_move(App *a, int delta) {
    int x, y, w, h;
    ui_list_geom(a, &x, &y, &w, &h);
    int vis = h - 2;
    a->lsel += delta;
    if (a->lsel < 0) a->lsel = 0;
    if (a->lsel >= a->nlist) a->lsel = a->nlist - 1;
    if (a->lsel < a->ltop) a->ltop = a->lsel;
    if (a->lsel >= a->ltop + vis) a->ltop = a->lsel - vis + 1;
}

static void list_key(App *a, const Event *e) {
    switch (e->key) {
    case K_UP:    list_move(a, -1); break;
    case K_DOWN:  list_move(a, 1); break;
    case K_PGUP:  list_move(a, -10); break;
    case K_PGDN:  list_move(a, 10); break;
    case '\r': case '\n': list_activate(a); break;
    case 27:      list_free(a); a->mode = MODE_NORMAL; break;
    default: break;
    }
}

/* ---------- commands ---------- */

/* ---------- sort ---------- */

/* One row's sort key. kind: 0 number, 1 text, 2 empty (empty always sorts last). */
typedef struct { int idx, kind; double num; char *text; } SortKey;

static int sort_desc;

/* ASCII case-insensitive comparison, so "apple" and "Banana" order naturally. */
static int text_cmp_ci(const char *x, const char *y) {
    for (;; x++, y++) {
        int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);
        if (cx != cy) return cx < cy ? -1 : 1;
        if (!cx) return 0;
    }
}

static int sort_cmp(const void *pa, const void *pb) {
    const SortKey *x = pa, *y = pb;
    if (x->kind == 2 || y->kind == 2) return (x->kind == 2) - (y->kind == 2);
    int r;
    if (x->kind != y->kind) r = x->kind < y->kind ? -1 : 1;   /* numbers before text */
    else if (x->kind == 0) r = (x->num > y->num) - (x->num < y->num);
    else r = text_cmp_ci(x->text, y->text);
    if (sort_desc) r = -r;
    if (r == 0) r = (x->idx > y->idx) - (x->idx < y->idx);     /* stable for ties */
    return r;
}

/* Sorts the rows of the selection by the active column, moving the selected
   columns together. Cell text is moved as typed, so formulas keep their
   references; check them after sorting. Refuses to cross merged cells. */
static void do_sort(App *a, int desc) {
    if (single_only(a)) return;
    Doc *d = D(a);
    Sheet *g = d->g;
    int r1, c1, r2, c2;
    doc_sel(d, &r1, &c1, &r2, &c2);
    if (r1 == r2 && c1 == c2) {                  /* one cell: sort the whole data area */
        r1 = 0; c1 = 0; r2 = MAX_ROWS - 1; c2 = MAX_COLS - 1;
    }
    if (r2 > g->max_r) r2 = g->max_r;
    if (c2 > g->max_c) c2 = g->max_c;
    if (r2 <= r1) { snprintf(a->msg, sizeof a->msg, "nothing to sort"); return; }

    for (int i = 0; i < g->nmg; i++) {
        const Merge *m = &g->mg[i];
        if (m->r1 <= r2 && m->r2 >= r1 && m->c1 <= c2 && m->c2 >= c1) {
            snprintf(a->msg, sizeof a->msg, "cannot sort across merged cells");
            return;
        }
    }

    int kc = d->cc < c1 ? c1 : d->cc > c2 ? c2 : d->cc;   /* the key column */
    int n = r2 - r1 + 1, w = c2 - c1 + 1;
    SortKey *keys = calloc((size_t)n, sizeof *keys);
    char **raws = calloc((size_t)n * w, sizeof *raws);
    char **fmts = calloc((size_t)n * w, sizeof *fmts);
    if (!keys || !raws || !fmts) {
        free(keys); free(raws); free(fmts);
        snprintf(a->msg, sizeof a->msg, "out of memory");
        return;
    }

    for (int i = 0; i < n; i++) {
        int r = r1 + i;
        double v = 0;
        int k = cell_num(g, r, kc, 0, &v);
        keys[i].idx = i;
        if (k == 0) { keys[i].kind = 0; keys[i].num = v; }
        else if (k == 3) keys[i].kind = 2;
        else {
            char buf[1024];
            int al = 0;
            cell_display(g, r, kc, buf, sizeof buf, &al);
            keys[i].kind = 1;
            keys[i].text = strdup(buf);
        }
        for (int j = 0; j < w; j++) {
            const char *s = sheet_get_raw(g, r, c1 + j);
            const char *f = sheet_fmt(g, r, c1 + j);
            raws[i * w + j] = s ? strdup(s) : NULL;
            fmts[i * w + j] = f ? strdup(f) : NULL;
        }
    }

    sort_desc = desc;
    qsort(keys, (size_t)n, sizeof *keys, sort_cmp);

    for (int i = 0; i < n; i++) {
        int src = keys[i].idx;
        for (int j = 0; j < w; j++) {
            sheet_put_raw(g, r1 + i, c1 + j, raws[src * w + j]);
            sheet_put_fmt(g, r1 + i, c1 + j, fmts[src * w + j]);
        }
    }

    for (int i = 0; i < n; i++) {
        free(keys[i].text);
        for (int j = 0; j < w; j++) {
            free(raws[i * w + j]);
            free(fmts[i * w + j]);
        }
    }
    free(keys); free(raws); free(fmts);

    d->dirty = 1;
    char cn[8];
    col_name(kc, cn);
    snprintf(a->msg, sizeof a->msg, "sorted %d rows by column %s %s",
             n, cn, desc ? "descending" : "ascending");
}

/* "B12" -> row 11, column 1. Letters name the column (A..ZZ), digits the row. */
static int parse_cell_ref(const char *s, int *r, int *c) {
    while (isspace((unsigned char)*s)) s++;
    int col = 0, nl = 0;
    while (isalpha((unsigned char)*s)) {
        col = col * 26 + (toupper((unsigned char)*s) - 'A' + 1);
        if (col > MAX_COLS) return 0;
        s++; nl++;
    }
    if (!nl || *s < '0' || *s > '9') return 0;
    long row = 0;
    while (*s >= '0' && *s <= '9') {
        row = row * 10 + (*s - '0');
        if (row > MAX_ROWS) return 0;
        s++;
    }
    while (isspace((unsigned char)*s)) s++;
    if (*s || !row) return 0;
    *r = (int)row - 1;
    *c = col - 1;
    return 1;
}

/* Every cell has this alignment: it goes back to the default; otherwise it is set on all. */
static void align_toggle(App *a, char al) {
    fmt_sel(a, sel_all_fmt(a, 2, al) ? 'x' : al, NULL, -1, -1);
}

/* Every cell has this integer style: it goes back to plain; otherwise it is set on all. */
static void int_toggle(App *a, int st) {
    fmt_num_set(a, 0, NULL, 0, sel_all_fmt(a, 5, st) ? '-' : st);
}

/* ---------- convert (Convert menu) ---------- */

/* Does a formula read a cell of the selection? Such a formula follows that cell
   when the cell is converted, so it must not be converted again. */
static int formula_hits_sel(const char *f, const Rect *q, int n) {
    for (const char *p = f; *p;) {
        if (!isalpha((unsigned char)*p)) { p++; continue; }
        int col = 0, nl = 0;
        const char *s = p;
        while (isalpha((unsigned char)*s) && nl < 3) { col = col * 26 + (toupper((unsigned char)*s) - 'A' + 1); s++; nl++; }
        long row = 0;
        int nd = 0;
        while (isdigit((unsigned char)*s) && nd < 6) { row = row * 10 + (*s - '0'); s++; nd++; }
        if (nd && row >= 1 && !isalnum((unsigned char)*s) && *s != '(' && (p == f || !isalnum((unsigned char)p[-1]))) {
            for (int i = 0; i < n; i++)
                if (row - 1 >= q[i].r1 && row - 1 <= q[i].r2 && col - 1 >= q[i].c1 && col - 1 <= q[i].c2) return 1;
        }
        p = s > p ? s : p + 1;
    }
    return 0;
}

/* Converts the numbers of the selection to unit `to`. A cell's own unit (its
   suffix) is its source when that is a known unit; a plain number takes `from`.
   A formula stays a formula: =(x)*a+b. Numbers of another kind are left alone. */
static void convert_sel(App *a, const char *from, const char *to) {
    int tid = units_find(to), fid = from ? units_find(from) : -1;
    if (tid < 0 || (from && fid < 0)) {
        snprintf(a->msg, sizeof a->msg, "unknown unit: %s", tid < 0 ? to : from);
        return;
    }
    Doc *d = D(a);
    Rect q[MAX_RECTS];
    int n = doc_rects(d, q), conv = 0, skip = 0;
    for (int i = 0; i < n; i++)
        for (int r = q[i].r1; r <= q[i].r2; r++)
            for (int c = q[i].c1; c <= q[i].c2; c++) {
                const Merge *m = sheet_merge_of(d->g, r, c);
                if (m && (m->r1 != r || m->c1 != c)) continue;
                const char *raw = sheet_get(d->g, r, c);
                double v;
                if (!raw || cell_num(d->g, r, c, 0, &v) != 0) continue;     /* numbers only */
                int al, bo, hl;
                const char *body;
                fmt_parse(sheet_fmt(d->g, r, c), &al, &bo, &hl, &body);
                NumSpec ns;
                num_parse(body, &ns);
                char sym[64];
                snprintf(sym, sizeof sym, "%s", ns.suf);
                size_t sl = strlen(sym);
                while (sl && sym[sl - 1] == ' ') sym[--sl] = '\0';
                char *su = sym;
                while (*su == ' ') su++;
                int sid = su[0] ? units_find(su) : fid;
                double A, B;
                if (sid < 0 || units_linear(sid, tid, &A, &B) != 0) { skip++; continue; }
                if (raw[0] == '=' && formula_hits_sel(raw, q, n)) { skip++; continue; }   /* it follows its inputs */
                if (raw[0] == '=') {
                    char nf[600];
                    snprintf(nf, sizeof nf, "=(%s)*%.15g%+.15g", raw + 1, A, B);
                    sheet_set(d->g, r, c, nf);
                } else {
                    char nr[64];
                    snprintf(nr, sizeof nr, "%.12g", v * A + B);
                    sheet_set(d->g, r, c, nr);
                }
                snprintf(ns.suf, sizeof ns.suf, " %s", units_sym(tid));
                char nb[200], fb[320];
                num_build(nb, sizeof nb, &ns);
                fmt_build(fb, sizeof fb, al, bo, hl, nb);
                sheet_fmt_set(d->g, r, c, fb);
                conv++;
            }
    if (conv) d->dirty = 1;
    if (conv)
        snprintf(a->msg, sizeof a->msg, "converted %d to %s%s", conv, units_sym(tid),
                 skip ? " (other cells left as they are)" : "");
    else
        snprintf(a->msg, sizeof a->msg, "no number in the selection converts to %s", units_sym(tid));
}

void run_action(App *a, int act) {
    if (act >= A_CONV_BASE && act - A_CONV_BASE < (int)(sizeof conv_pairs / sizeof conv_pairs[0])) {
        convert_sel(a, conv_pairs[act - A_CONV_BASE][0], conv_pairs[act - A_CONV_BASE][1]);
        return;
    }
    if (act >= A_GOPT_BASE) { graph_menu_action(a, act); return; }
    Doc *d = D(a);
    (void)kind_fg;             /* palette lives in config.h, used by ui.c */
    switch (act) {
    case A_NEW:     doc_new(a); snprintf(a->msg, sizeof a->msg, "new sheet"); break;
    case A_OPEN:    start_prompt(a, "Open: ", "", A_OPEN, -1); break;
    case A_FIND:    start_prompt(a, "Find file (text or *.ext): ", "", A_FIND, -1); break;
    case A_SAVE: {
        if (d->path) { save_path(a, d, d->path, d->fmt); break; }
        char nm[256];
        strip_ext(d->name, nm, sizeof nm);
        snprintf(nm + strlen(nm), sizeof nm - strlen(nm), ".csv");
        start_prompt(a, "Save as: ", nm, A_SAVEAS, FMT_CSV);
        break;
    }
    case A_SAVEAS_CSV: case A_SAVEAS_TSV: case A_SAVEAS_XML: case A_SAVEAS_XLSX: case A_SAVEAS_ODS:
    case A_SAVEAS_JSON: {
        int fmt = act == A_SAVEAS_CSV ? FMT_CSV : act == A_SAVEAS_TSV ? FMT_TSV
                : act == A_SAVEAS_XML ? FMT_XML : act == A_SAVEAS_XLSX ? FMT_XLSX
                : act == A_SAVEAS_JSON ? FMT_JSON : FMT_ODS;
        char nm[256];
        strip_ext(d->name, nm, sizeof nm);
        fix_ext(nm, sizeof nm, fmt);
        start_prompt(a, "Save as: ", nm, A_SAVEAS, fmt);
        break;
    }
    case A_CLOSE:   close_doc(a); break;
    case A_QUIT:    quit_app(a); break;
    case A_CUT:     do_copy(a, 1, 0); break;
    case A_COPY_RAW:    do_copy(a, 0, 0); break;
    case A_COPY_VALUES: do_copy(a, 0, 1); break;
    case A_COPY_FMT:    do_copy(a, 0, 2); break;
    case A_COPY_LATEX:  do_copy(a, 0, 3); break;
    case A_PASTE: case A_PASTE_VALUES: case A_PASTE_FORMULAS:
    case A_PASTE_FMT: case A_PASTE_TRANSPOSE:
        do_paste(a, act);
        break;
    case A_CLEAR:   do_clear(a); break;
    case A_FILL_DOWN:  do_fill(a, 1); break;
    case A_FILL_RIGHT: do_fill(a, 0); break;
    case A_SELALL:
        d->next = 0;
        d->ar = 0; d->ac = 0;
        d->cr = d->g->max_r; d->cc = d->g->max_c;
        d->sel_kind = 0;
        break;
    case A_NEXT_SHEET: a->cur = (a->cur + 1) % a->ndocs; break;
    case A_PREV_SHEET: a->cur = (a->cur + a->ndocs - 1) % a->ndocs; break;
    case A_HELP:    open_help(a); break;
    case A_ALIGN_L: align_toggle(a, '<'); break;
    case A_ALIGN_C: align_toggle(a, '^'); break;
    case A_ALIGN_R: align_toggle(a, '>'); break;
    case A_FMT_BOLD: fmt_toggle(a, 0); break;
    case A_FMT_HL:   fmt_toggle(a, 1); break;
    case A_MERGE: {
        if (single_only(a)) break;
        int r1, c1, r2, c2;
        doc_sel(d, &r1, &c1, &r2, &c2);
        if (r1 == r2 && c1 == c2) { snprintf(a->msg, sizeof a->msg, "select more than one cell to merge"); break; }
        if (sheet_merge_add(d->g, r1, c1, r2, c2) == 0) {
            d->dirty = 1;
            snprintf(a->msg, sizeof a->msg, "merged %dx%d", r2 - r1 + 1, c2 - c1 + 1);
        } else snprintf(a->msg, sizeof a->msg, "cannot merge");
        move_to(a, d->cr, d->cc, 0);
        break;
    }
    case A_UNMERGE: {
        int r1, c1, r2, c2;
        doc_sel(d, &r1, &c1, &r2, &c2);
        sheet_unmerge(d->g, r1, c1, r2, c2);
        d->dirty = 1;
        snprintf(a->msg, sizeof a->msg, "unmerged");
        break;
    }
    case A_FMT_DATE:                 /* applied everywhere: removed; otherwise asks for a format */
        if (sel_all_fmt(a, 3, 0)) {
            fmt_sel(a, 0, "", -1, -1);
            snprintf(a->msg, sizeof a->msg, "date format removed");
        } else start_prompt(a, "Date/time format (strftime, UTC): ", "%Y-%m-%d %H:%M", A_FMT_DATE, -1);
        break;
    case A_FMT_DEC:
        if (sel_all_fmt(a, 4, 0)) {
            fmt_num_set(a, 3, NULL, -1, 0);
            snprintf(a->msg, sizeof a->msg, "decimal places removed");
        } else start_prompt(a, "Decimal places (0-9): ", "2", A_FMT_DEC, -1);
        break;
    case A_FMT_CLEAR: fmt_sel(a, 0, "", -1, -1); break;
    case A_VIM:
        a->vim = !a->vim;
        a->vim_vis = a->vim_pend = 0;
        snprintf(a->msg, sizeof a->msg, "vim keys %s", a->vim ? "on" : "off");
        break;
    case A_GRAPH:           graph_run(a, 0); break;
    case A_GRAPH_VIEW:      graph_run(a, 1); break;
    case A_GRAPH_OPTIONS:   dialog_open(a); break;
    case A_UNDO: {
        Txn *t = hist_pop(undo_s, &nundo);
        if (!t) { snprintf(a->msg, sizeof a->msg, "nothing to undo"); break; }
        Txn *inv = calloc(1, sizeof *inv);
        if (inv) {
            txn_undo(t, inv);
            hist_push(redo_s, &nredo, inv);
        }
        txn_free(t);
        D(a)->dirty = 1;
        snprintf(a->msg, sizeof a->msg, "undone");
        break;
    }
    case A_REDO: {
        Txn *t = hist_pop(redo_s, &nredo);
        if (!t) { snprintf(a->msg, sizeof a->msg, "nothing to redo"); break; }
        Txn *inv = calloc(1, sizeof *inv);
        if (inv) {
            txn_undo(t, inv);
            hist_push(undo_s, &nundo, inv);
        }
        txn_free(t);
        D(a)->dirty = 1;
        snprintf(a->msg, sizeof a->msg, "redone");
        break;
    }
    case A_GRAPH_EXTRA:
        start_prompt(a, "Extra gnuplot commands (separate with ;): ", a->gextra, A_GRAPH_EXTRA, -1);
        break;
    case A_XOPT_COMMA: case A_XOPT_SEMI: case A_XOPT_TAB: case A_XOPT_PIPE: case A_XOPT_SPACE:
        a->xo.delim = act_delim(act);
        snprintf(a->msg, sizeof a->msg, "export delimiter: %s", delim_name(a->xo.delim));
        break;
    case A_XOPT_CRLF:
        a->xo.crlf = !a->xo.crlf;
        snprintf(a->msg, sizeof a->msg, "export line endings: %s", a->xo.crlf ? "CRLF" : "LF");
        break;
    case A_XOPT_SELONLY:
        a->xo.sel_only = !a->xo.sel_only;
        snprintf(a->msg, sizeof a->msg, "export selection only: %s", a->xo.sel_only ? "on" : "off");
        break;
    case A_PREFIX: case A_SUFFIX: {
        int al, bo, hl;
        const char *body;
        NumSpec ns;
        fmt_parse(sheet_fmt(d->g, d->cr, d->cc), &al, &bo, &hl, &body);
        num_parse(body, &ns);
        int pre = act == A_PREFIX;
        start_prompt(a, pre ? "Prefix, UTF-8 or LaTeX (empty clears): "
                            : "Suffix, UTF-8 or LaTeX (empty clears): ",
                     pre ? ns.pre : ns.suf, act, -1);
        break;
    }
    case A_INT_PLAIN: case A_INT_COMMA: case A_INT_SPACE: case A_INT_SCI: case A_INT_ENG: {
        int st = act == A_INT_PLAIN ? '-' : act == A_INT_COMMA ? ',' : act == A_INT_SPACE ? ' '
               : act == A_INT_SCI ? 'e' : 'g';
        int_toggle(a, st);
        snprintf(a->msg, sizeof a->msg, "numbers: %s",
                 st == '-' ? "plain" : st == ',' ? "1,000" : st == ' ' ? "1 000"
                 : st == 'e' ? "1*10^3" : "1 k");
        break;
    }
    case A_CONV_TO:
        start_prompt(a, "Convert to (ft, or 'm ft' for from and to): ", "", A_CONV_TO, -1);
        break;
    case A_DLG_IMPORT: dialog_acts(a, import_menu, "Import"); break;
    case A_DLG_EXPORT: dialog_acts(a, export_menu, "Export"); break;
    case A_XOPT_SHOW:
        snprintf(a->msg, sizeof a->msg, "export: %s delimiter, %s, selection only %s; open: %s delimiter",
                 delim_name(a->xo.delim), a->xo.crlf ? "CRLF" : "LF",
                 a->xo.sel_only ? "on" : "off", delim_name(a->imp_delim));
        break;
    case A_IMP_COMMA: case A_IMP_SEMI: case A_IMP_TAB: case A_IMP_PIPE: case A_IMP_SPACE:
        a->imp_delim = act_delim(act);
        snprintf(a->msg, sizeof a->msg, "opening text files: %s delimiter", delim_name(a->imp_delim));
        break;
    case A_SORT_ASC:  do_sort(a, 0); break;
    case A_SORT_DESC: do_sort(a, 1); break;
    case A_GOTO:
        start_prompt(a, "Go to cell: ", "", A_GOTO, -1);
        break;
    case A_RESIZE:
        a->mode = MODE_RESIZE;
        snprintf(a->msg, sizeof a->msg, "resize: arrows set width/height, Enter ends");
        break;
    default: break;
    }
}

/* ---------- resize mode ---------- */

static void resize_col(App *a, int c, int delta) {
    D(a)->dirty = 1;
    Doc *d = D(a);
    int w = doc_colw(d, c) + delta;
    if (w < COL_MIN_W) w = COL_MIN_W;
    if (w > COL_MAX_W) w = COL_MAX_W;
    d->cw[c] = (unsigned char)w;
    char cn[8];
    col_name(c, cn);
    snprintf(a->msg, sizeof a->msg, "column %s width %d", cn, w);
}

static void resize_row(App *a, int r, int delta) {
    D(a)->dirty = 1;
    Doc *d = D(a);
    int h = doc_rowh(d, r) + delta;
    if (h < ROW_DEF_H) h = ROW_DEF_H;
    if (h > ROW_MAX_H) h = ROW_MAX_H;
    d->rh[r] = (unsigned char)h;
    snprintf(a->msg, sizeof a->msg, "row %d height %d", r + 1, h);
}

/* Arrows change the current column width (left/right) or row height (up/down).
   Shift makes steps of 5. Enter or Esc leaves the mode. */
static void resize_key(App *a, const Event *e) {
    Doc *d = D(a);
    int step = e->mods & MOD_SHIFT ? 5 : 1;
    switch (e->key) {
    case K_LEFT:  resize_col(a, d->cc, -step); break;
    case K_RIGHT: resize_col(a, d->cc, step); break;
    case K_UP:    resize_row(a, d->cr, -1); break;
    case K_DOWN:  resize_row(a, d->cr, 1); break;
    case '\r': case '\n': case 27:
        a->mode = MODE_NORMAL;
        snprintf(a->msg, sizeof a->msg, "resize done");
        break;
    default: break;
    }
}

/* ---------- graph ---------- */

static void graph_run(App *a, int view) {
    if (single_only(a)) return;
    Doc *d = D(a);
    int r1, c1, r2, c2;
    used_sel(d, &r1, &c1, &r2, &c2);
    if (r1 == r2 && c1 == c2) {                 /* one cell selected: graph the whole data */
        r1 = c1 = 0;
        r2 = d->g->max_r;
        c2 = d->g->max_c;
    }
    char pdf[PATH_MAX], msg[256];
    if (graph_draw(d, r1, c1, r2, c2, a->gextra, d->name, pdf, sizeof pdf, msg, sizeof msg, view) == 0) {
        if (!view) graph_open(pdf);
        if (!view) snprintf(a->msg, sizeof a->msg, "graph opened: %.200s", pdf);
        else snprintf(a->msg, sizeof a->msg, "%s", msg);
    } else {
        snprintf(a->msg, sizeof a->msg, "%.250s", msg);
    }
}

/* ---------- vim keys (Options > Vim keys) ---------- */

/* Normal and visual mode. Ctrl keys, arrows, F keys and Enter/Tab/Delete keep
   their usual meaning. Insert mode is the cell editor (Esc commits). */
static void vim_normal(App *a, const Event *e) {
    Doc *d = D(a);
    int k = e->key;
    if (k == 27) {
        a->vim_vis = a->vim_pend = 0;
        d->sel_kind = 0;
        return;
    }
    if (k == 18) { run_action(a, A_REDO); return; }      /* vim: Ctrl+R redoes */
    if (k > 255 || k < 32 || k == 127 || (e->mods & MOD_CTRL)) {
        normal_key(a, e);
        return;
    }
    int ext = a->vim_vis;
    if (ext && (k == 'y' || k == 'd' || k == 'x')) {     /* visual: act on the selection */
        if (k == 'y') do_copy(a, 0, 0);
        else do_clear(a);
        a->vim_vis = 0;
        move_to(a, d->cr, d->cc, 0);
        return;
    }
    if (a->vim_pend) {
        int p = a->vim_pend;
        a->vim_pend = 0;
        if (p == 'g' && k == 'g') move_to(a, 0, d->cc, ext);
        else if (p == 'd' && k == 'd') do_clear(a);
        else if (p == 'y' && k == 'y') do_copy(a, 0, 0);
        return;
    }
    switch (k) {
    case 'h': move_to(a, d->cr, d->cc - 1, ext); break;
    case 'j': move_to(a, d->cr + 1, d->cc, ext); break;
    case 'k': move_to(a, d->cr - 1, d->cc, ext); break;
    case 'l': move_to(a, d->cr, d->cc + 1, ext); break;
    case '0': move_to(a, d->cr, 0, ext); break;
    case '$': move_to(a, d->cr, d->g->max_c, ext); break;
    case 'G': move_to(a, d->g->max_r, d->cc, ext); break;
    case 'w': jump(a, 0, 1, ext); break;
    case 'b': jump(a, 0, -1, ext); break;
    case 'g': case 'd': case 'y': a->vim_pend = k; break;
    case 'v':
        a->vim_vis = !a->vim_vis;
        if (a->vim_vis) { d->ar = d->cr; d->ac = d->cc; }
        else move_to(a, d->cr, d->cc, 0);
        break;
    case 'i': begin_edit(a, 0); a->epos = 0; break;
    case 'a': begin_edit(a, 0); break;
    case 'A': begin_edit(a, 0); a->epos = a->elen; break;
    case 'I': begin_edit(a, 0); a->epos = 0; break;
    case 'o':
        move_to(a, d->cr + 1, d->cc, 0);
        begin_edit(a, 0);
        a->ebuf[0] = '\0';
        a->elen = a->epos = 0;
        break;
    case 'x': do_clear(a); break;
    case 'p': do_paste(a, A_PASTE); break;
    case 'u': run_action(a, A_UNDO); break;
    case ':': start_prompt(a, ":", "", A_VIMCMD, -1); break;
    case '=': begin_edit(a, '='); break;
    default: break;
    }
}

/* ---------- graph options ----------
   Every option is a gnuplot command in a->gextra (';' separated). The editor,
   the right-click menu and the preview all read and write that text. */

typedef struct { char t[64][320]; int n; } Toks;

static void toks_split(Toks *k, const char *s) {
    k->n = 0;
    const char *p = s;
    while (*p && k->n < 64) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *q = p;
        while (len && isspace((unsigned char)*q)) { q++; len--; }
        while (len && isspace((unsigned char)q[len - 1])) len--;
        if (len && len < 319) {
            memcpy(k->t[k->n], q, len);
            k->t[k->n][len] = '\0';
            k->n++;
        }
        if (!e) break;
        p = e + 1;
    }
}

static void toks_join(const Toks *k, char *out, size_t n) {
    size_t l = 0;
    out[0] = '\0';
    for (int i = 0; i < k->n; i++) {
        int w = snprintf(out + l, n - l, "%s%s", i ? "; " : "", k->t[i]);
        if (w < 0 || (size_t)w >= n - l) break;
        l += (size_t)w;
    }
}

static void tok_add(Toks *k, const char *t) {
    if (k->n < 64) {
        snprintf(k->t[k->n], sizeof k->t[0], "%s", t);
        k->n++;
    }
}

/* Does command tok belong to option o? val receives its value ("on" for flags). */
static int opt_match(const GraphOpt *o, const char *tok, char *val, size_t n) {
    if (o->off && !strcmp(tok, o->off)) { snprintf(val, n, "off"); return 1; }
    if (o->kind == GO_FLAG) {
        if (strcmp(tok, o->cmd)) return 0;
        snprintf(val, n, "on");
        return 1;
    }
    const char *pct = strchr(o->cmd, '%');       /* the %s */
    size_t pl = (size_t)(pct - o->cmd), sl = strlen(pct + 2), tl = strlen(tok);
    if (tl < pl + sl || strncmp(tok, o->cmd, pl) || strcmp(tok + tl - sl, pct + 2)) return 0;
    size_t vl = tl - pl - sl;
    if (vl >= n) vl = n - 1;
    memcpy(val, tok + pl, vl);
    val[vl] = '\0';
    return 1;
}

static int opt_find(const Toks *k, const GraphOpt *o, char *val, size_t n) {
    for (int i = 0; i < k->n; i++)
        if (opt_match(o, k->t[i], val, n)) return 1;
    val[0] = '\0';
    return 0;
}

static void opt_del(Toks *k, const GraphOpt *o) {
    int w = 0;
    char val[320];
    for (int i = 0; i < k->n; i++) {
        if (!opt_match(o, k->t[i], val, sizeof val)) {
            if (w != i) memcpy(k->t[w], k->t[i], sizeof k->t[0]);
            w++;
        }
    }
    k->n = w;
}

/* Sets option o to value (NULL or "" removes it). Quotes in values are dropped,
   since they would end the gnuplot string. */
static void opt_put(Toks *k, const GraphOpt *o, const char *value) {
    opt_del(k, o);
    if (!value || !*value) return;
    char clean[256], cmd[320];
    int w = 0;
    for (const char *p = value; *p && w < 255; p++)
        if (*p != '\'') clean[w++] = *p;
    clean[w] = '\0';
    if (o->off && !strcmp(clean, "off")) tok_add(k, o->off);
    else if (o->kind == GO_FLAG) tok_add(k, o->cmd);
    else {
        snprintf(cmd, sizeof cmd, o->cmd, clean);
        tok_add(k, cmd);
    }
}

static int dlg_prompt(int act) {
    return act == A_DLG_ARG || act == A_GRAPH_EXTRA;
}

static void dlg_add(App *a, const char *label, int act, int inv, const char *desc) {
    if (a->ndlg >= 24) return;
    DlgItem *it = &a->dlg[a->ndlg++];
    snprintf(it->label, sizeof it->label, "%.150s", label);
    it->act = act;
    it->inv = inv;
    it->desc = desc;
}

/* The items of a menu, as dialog rows: separators are skipped, a row shows [*] when on. */
static void acts_refresh(App *a) {
    a->ndlg = 0;
    for (const MenuItem *it = a->acts; it->label; it++) {
        if (!strcmp(it->label, "-")) continue;
        int on = act_on(a, it->act);
        char label[200];
        if (it->key[0]) snprintf(label, sizeof label, "[%c] %s   %s", on ? '*' : ' ', it->label, it->key);
        else snprintf(label, sizeof label, "[%c] %s", on ? '*' : ' ', it->label);
        dlg_add(a, label, it->act, on, it->desc);
    }
}

enum { GI_DRAW = -1, GI_RAW = -2, GI_CLOSE = -3 };

static void dlg_follow(App *a) {
    int L = ui_dlg_list_rows(a);
    if (a->dlg_sel < a->dlg_top) a->dlg_top = a->dlg_sel;
    if (a->dlg_sel >= a->dlg_top + L) a->dlg_top = a->dlg_sel - L + 1;
    if (a->dlg_top < 0) a->dlg_top = 0;
}

static void dlg_refresh(App *a) {
    if (a->acts) { acts_refresh(a); return; }
    Toks k;
    toks_split(&k, a->gextra);
    a->ndlg = 0;
    char val[320], label[200];
    if (a->dlg_kind == DLGK_MAIN) {
        for (int i = 0; i < GO_COUNT; i++) {
            const GraphOpt *o = &graph_opts[i];
            int on = opt_find(&k, o, val, sizeof val);
            if (o->kind == GO_FLAG)
                snprintf(label, sizeof label, "[%c] %s", on ? '*' : ' ', o->name);
            else if (o->kind == GO_CHOICE)
                snprintf(label, sizeof label, "[%c] %-15s %s", on ? '*' : ' ', o->name,
                         on ? val : "(default)");
            else
                snprintf(label, sizeof label, "[%c] %-15s %s", on ? '*' : ' ', o->name,
                         on ? val : "(not set)");
            dlg_add(a, label, i, on, o->help);
        }
        dlg_add(a, "Draw graph (F9)", GI_DRAW, 0, "Render the graph to a PDF and open it.");
        dlg_add(a, "Raw command text...", GI_RAW, 0,
                "Edit the whole command text by hand (';' separated).");
        dlg_add(a, "Close", GI_CLOSE, 0, "Close the editor.");
        return;
    }
    /* choices of one option: the current value is inverted */
    const GraphOpt *o = &graph_opts[a->dlg_opt];
    int on = opt_find(&k, o, val, sizeof val);
    for (int i = 0; o->choices[i]; i++)
        dlg_add(a, o->choices[i], i, on && !strcmp(val, o->choices[i]), o->help);
}

static void dialog_open(App *a) {
    a->acts = NULL;
    a->mode = MODE_DLG;
    a->dlg_kind = DLGK_MAIN;
    a->dlg_sel = a->dlg_top = 0;
    a->npanels = 0;
    a->mb = -1;
    dlg_refresh(a);
}

/* A dialog listing the items of a menu (File > Import, File > Export). */
static void dialog_acts(App *a, const MenuItem *items, const char *title) {
    a->acts = items;
    a->acts_title = title;
    a->mode = MODE_DLG;
    a->dlg_kind = DLGK_MAIN;
    a->dlg_sel = a->dlg_top = 0;
    a->npanels = 0;
    a->mb = -1;
    dlg_refresh(a);
}

static void graph_text_store(App *a, Toks *k) {
    toks_join(k, a->gextra, sizeof a->gextra);
    dlg_refresh(a);
}

/* text prompts (an argument's value, or the raw command text) */
static void dlg_text_accept(App *a, const char *text) {
    Toks k;
    toks_split(&k, a->gextra);
    const GraphOpt *o = &graph_opts[a->dlg_opt];
    opt_put(&k, o, text);
    snprintf(a->msg, sizeof a->msg, "%.60s %s", o->name, text[0] ? "set" : "cleared");
    graph_text_store(a, &k);
}

static void dlg_activate(App *a) {
    dlg_refresh(a);
    if (a->dlg_sel < 0 || a->dlg_sel >= a->ndlg) return;
    int act = a->dlg[a->dlg_sel].act;
    if (a->acts) {
        if (!act_is_setting(act)) { a->acts = NULL; a->mode = MODE_NORMAL; }
        run_action(a, act);              /* a setting stays open: the list shows its new state */
        return;
    }
    Toks k;
    toks_split(&k, a->gextra);
    char val[320];
    if (a->dlg_kind == DLGK_MAIN) {
        if (act == GI_DRAW) { a->mode = MODE_NORMAL; graph_run(a, 0); return; }
        if (act == GI_CLOSE) { a->mode = MODE_NORMAL; return; }
        if (act == GI_RAW) {
            start_prompt(a, "Command text (;-separated): ", a->gextra, A_GRAPH_EXTRA, -1);
            return;
        }
        const GraphOpt *o = &graph_opts[act];
        if (o->kind == GO_FLAG) {
            int on = opt_find(&k, o, val, sizeof val);
            opt_put(&k, o, on ? NULL : "on");
            graph_text_store(a, &k);
        } else if (o->kind == GO_CHOICE) {
            a->dlg_kind = DLGK_CHOICE;
            a->dlg_opt = act;
            a->dlg_sel = a->dlg_top = 0;
            dlg_refresh(a);
        } else {
            a->dlg_opt = act;
            opt_find(&k, o, val, sizeof val);
            start_prompt(a, o->name, val, A_DLG_ARG, -1);
            snprintf(a->msg, sizeof a->msg, "%s: %.40s", o->name, o->cmd);
        }
        return;
    }
    /* a choice was picked: apply it and return to the list */
    const GraphOpt *o = &graph_opts[a->dlg_opt];
    opt_put(&k, o, o->choices[act]);
    snprintf(a->msg, sizeof a->msg, "%s: %s", o->name, o->choices[act]);
    a->dlg_kind = DLGK_MAIN;
    a->dlg_sel = a->dlg_opt;
    a->dlg_top = 0;
    graph_text_store(a, &k);
    dlg_follow(a);
}

static void dlg_key(App *a, const Event *e) {
    dlg_refresh(a);
    if (a->acts && e->key == 27) { a->acts = NULL; a->mode = MODE_NORMAL; return; }
    switch (e->key) {
    case K_UP:   a->dlg_sel = (a->dlg_sel + a->ndlg - 1) % a->ndlg; dlg_follow(a); break;
    case K_DOWN: a->dlg_sel = (a->dlg_sel + 1) % a->ndlg; dlg_follow(a); break;
    case K_PGUP: a->dlg_sel = a->dlg_sel > 5 ? a->dlg_sel - 5 : 0; dlg_follow(a); break;
    case K_PGDN: a->dlg_sel = a->dlg_sel + 5 < a->ndlg ? a->dlg_sel + 5 : a->ndlg - 1; dlg_follow(a); break;
    case ' ': case '\r': case '\n': dlg_activate(a); break;
    case 27:
        if (a->dlg_kind != DLGK_MAIN) {
            a->dlg_kind = DLGK_MAIN;
            a->dlg_sel = a->dlg_opt;
            a->dlg_top = 0;
            dlg_refresh(a);
            dlg_follow(a);
        } else a->mode = MODE_NORMAL;
        break;
    default: break;
    }
}

static void dlg_mouse(App *a, const Event *e) {
    dlg_refresh(a);
    if (e->mtype == MS_WHEEL_UP || e->mtype == MS_WHEEL_DOWN) {
        a->dlg_top += e->mtype == MS_WHEEL_UP ? -3 : 3;
        int L = ui_dlg_list_rows(a);
        if (a->dlg_top > a->ndlg - L) a->dlg_top = a->ndlg - L;
        if (a->dlg_top < 0) a->dlg_top = 0;
        return;
    }
    int x, y, w, h;
    ui_dlg_geom(a, &x, &y, &w, &h);
    int L = ui_dlg_list_rows(a);
    int inside = e->mx >= x && e->mx < x + w && e->my >= y + 2 && e->my < y + 2 + L;
    if (inside) {
        int idx = a->dlg_top + (e->my - y - 2);
        if (idx >= a->ndlg) return;
        if (e->mtype == MS_MOVE) { a->dlg_sel = idx; return; }
        if (e->mtype == MS_PRESS && e->btn == 0) { a->dlg_sel = idx; dlg_activate(a); }
        return;
    }
    if (e->mtype == MS_PRESS) {                  /* outside: back out, or close */
        if (a->acts) { a->acts = NULL; a->mode = MODE_NORMAL; return; }
        if (a->dlg_kind != DLGK_MAIN) {
            a->dlg_kind = DLGK_MAIN;
            a->dlg_sel = a->dlg_opt;
            a->dlg_top = 0;
        } else a->mode = MODE_NORMAL;
    }
}

/* menu actions: toggle a flag, set a choice, or toggle the legend */
static void graph_menu_action(App *a, int act) {
    Toks k;
    toks_split(&k, a->gextra);
    char val[320];
    if (act >= A_GOPT_VAL) {
        int v = act - A_GOPT_VAL, idx = v / 32, ch = v % 32;
        const GraphOpt *o = &graph_opts[idx];
        opt_put(&k, o, o->choices[ch]);
        snprintf(a->msg, sizeof a->msg, "%s: %s", o->name, o->choices[ch]);
    } else {
        int idx = act - A_GOPT_BASE;
        const GraphOpt *o = &graph_opts[idx];
        int on = opt_find(&k, o, val, sizeof val);
        if (o->kind == GO_FLAG) {
            opt_put(&k, o, on ? NULL : "on");
            snprintf(a->msg, sizeof a->msg, "%s %s", o->name, on ? "off" : "on");
        } else if (o->kind == GO_CHOICE) {             /* legend: on at top right, or off */
            int legend_off = on && !strcmp(val, "off");
            opt_put(&k, o, legend_off ? "top right" : (o->off ? "off" : NULL));
            snprintf(a->msg, sizeof a->msg, "%s %s", o->name, legend_off ? "on" : "off");
        } else {
            a->dlg_opt = idx;
            start_prompt(a, o->name, val, A_DLG_ARG, -1);
            return;
        }
    }
    graph_text_store(a, &k);
}

/* ---------- prompt, normal and edit keys ---------- */

static void prompt_accept(App *a) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", a->ebuf);
    a->mode = dlg_prompt(a->prompt_act) ? MODE_DLG : MODE_NORMAL;
    if (a->prompt_act == A_DLG_ARG) {
        dlg_text_accept(a, path);
        return;
    }
    if (a->prompt_act == A_VIMCMD) {
        if (!strcmp(path, "w")) run_action(a, A_SAVE);
        else if (!strcmp(path, "q")) run_action(a, A_QUIT);
        else if (!strcmp(path, "q!")) a->quit = 1;
        else if (!strcmp(path, "wq") || !strcmp(path, "x")) {
            run_action(a, A_SAVE);
            if (!D(a)->dirty) a->quit = 1;
        } else snprintf(a->msg, sizeof a->msg, "not a command: %.60s", path);
        return;
    }
    if (a->prompt_act == A_GRAPH_EXTRA) {          /* may be cleared to empty */
        snprintf(a->gextra, sizeof a->gextra, "%.1000s", a->ebuf);
        snprintf(a->msg, sizeof a->msg, "graph commands updated");
        return;
    }
    if (a->prompt_act == A_PREFIX || a->prompt_act == A_SUFFIX) {       /* empty clears it */
        int pre = a->prompt_act == A_PREFIX;
        if (strlen(path) > 60) { snprintf(a->msg, sizeof a->msg, "too long (60 bytes at most)"); return; }
        fmt_num_set(a, pre ? 1 : 2, path, 0, 0);
        snprintf(a->msg, sizeof a->msg, "%s %s", pre ? "prefix" : "suffix", path[0] ? "set" : "cleared");
        return;
    }
    if (!path[0]) { snprintf(a->msg, sizeof a->msg, "cancelled"); return; }
    if (a->prompt_act == A_OPEN) load_path(a, path);
    else if (a->prompt_act == A_FIND) do_find(a, path);
    else if (a->prompt_act == A_GOTO) {
        int r, c;
        if (parse_cell_ref(path, &r, &c)) move_to(a, r, c, 0);
        else snprintf(a->msg, sizeof a->msg, "not a cell: %.40s", path);
    }
    else if (a->prompt_act == A_CONV_TO) {
        char f1[32] = "", f2[32] = "";
        int k = sscanf(path, "%31s %31s", f1, f2);
        if (k == 2) convert_sel(a, f1, f2);
        else if (k == 1) convert_sel(a, NULL, f1);
    }
    else if (a->prompt_act == A_FMT_DATE) {
        if (path[0] != '%') snprintf(a->msg, sizeof a->msg, "format must start with %% (e.g. %%Y-%%m-%%d)");
        else fmt_sel(a, 0, path, -1, -1);
    } else if (a->prompt_act == A_FMT_DEC) {
        if (path[0] >= '0' && path[0] <= '9' && !path[1]) {
            fmt_num_set(a, 3, NULL, path[0] - '0', 0);
        } else snprintf(a->msg, sizeof a->msg, "decimal places must be 0-9");
    }
    else if (a->prompt_act == A_SAVEAS) {
        int fmt = a->prompt_fmt >= 0 ? a->prompt_fmt : io_fmt_from_path(path);
        if (fmt < 0) fmt = FMT_CSV;
        fix_ext(path, sizeof path, fmt);
        save_path(a, D(a), path, fmt);
    }
}

static void prompt_key(App *a, const Event *e) {
    switch (e->key) {
    case '\r': case '\n': prompt_accept(a); break;
    case 27:
        a->mode = dlg_prompt(a->prompt_act) ? MODE_DLG : MODE_NORMAL;
        snprintf(a->msg, sizeof a->msg, "cancelled");
        break;
    case 127: case 8: ebuf_backspace(a); break;
    case K_DEL: ebuf_delete(a); break;
    case K_LEFT: if (a->epos > 0) a->epos--; break;
    case K_RIGHT: if (a->epos < a->elen) a->epos++; break;
    case K_HOME: a->epos = 0; break;
    case K_END: a->epos = a->elen; break;
    default:
        if (e->key >= 32 && e->key < 256) ebuf_insert(a, e->key);
    }
}

static void edit_key(App *a, const Event *e) {
    switch (e->key) {
    case 27:       end_edit(a, a->vim); break;      /* vim: Esc saves the cell */
    case '\r': case '\n': end_edit(a, 1); move_to(a, D(a)->cr + 1, D(a)->cc, 0); break;
    case '\t':     end_edit(a, 1); move_to(a, D(a)->cr, D(a)->cc + 1, 0); break;
    case K_BTAB:   end_edit(a, 1); move_to(a, D(a)->cr, D(a)->cc - 1, 0); break;
    case K_UP:     end_edit(a, 1); move_to(a, D(a)->cr - 1, D(a)->cc, 0); break;
    case K_DOWN:   end_edit(a, 1); move_to(a, D(a)->cr + 1, D(a)->cc, 0); break;
    case K_LEFT:   if (a->epos > 0) a->epos--; break;
    case K_RIGHT:  if (a->epos < a->elen) a->epos++; break;
    case K_HOME:   a->epos = 0; break;
    case K_END:    a->epos = a->elen; break;
    case 127: case 8: ebuf_backspace(a); break;
    case K_DEL:    ebuf_delete(a); break;
    default:
        if (e->key >= 32 && e->key < 256) ebuf_insert(a, e->key);
    }
}

static void normal_key(App *a, const Event *e) {
    Doc *d = D(a);
    int k = e->key, ctrl = e->mods & MOD_CTRL, shift = e->mods & MOD_SHIFT;
    if (k != 23 && k != 17) a->armed = 0;      /* only Ctrl+W / Ctrl+Q keep a pending confirm */
    int vr = view_rows_n(a);
    switch (k) {
    case K_UP:    if (ctrl) jump(a, -1, 0, shift); else move_to(a, d->cr - 1, d->cc, shift); break;
    case K_DOWN:  if (ctrl) jump(a, 1, 0, shift);  else move_to(a, d->cr + 1, d->cc, shift); break;
    case K_LEFT:  if (ctrl) jump(a, 0, -1, shift); else move_to(a, d->cr, d->cc - 1, shift); break;
    case K_RIGHT: if (ctrl) jump(a, 0, 1, shift);  else move_to(a, d->cr, d->cc + 1, shift); break;
    case K_PGUP:
        if (ctrl) run_action(a, A_PREV_SHEET);
        else move_to(a, d->cr - vr, d->cc, shift);
        break;
    case K_PGDN:
        if (ctrl) run_action(a, A_NEXT_SHEET);
        else move_to(a, d->cr + vr, d->cc, shift);
        break;
    case K_HOME:  if (ctrl) move_to(a, 0, 0, shift); else move_to(a, d->cr, 0, shift); break;
    case K_END:   if (ctrl) move_to(a, d->g->max_r, d->g->max_c, shift); else move_to(a, d->cr, d->g->max_c, shift); break;
    case '\t':    move_to(a, d->cr, d->cc + 1, 0); break;
    case K_BTAB:  move_to(a, d->cr, d->cc - 1, 0); break;
    case '\r': case '\n': move_to(a, d->cr + 1, d->cc, 0); break;
    case K_F1:    open_help(a); break;
    case K_F2:    begin_edit(a, 0); a->mode = MODE_EDIT; break;
    case K_F3:    run_action(a, A_RESIZE); break;
    case K_F9:    run_action(a, A_GRAPH); break;
    case K_F10:   menu_open_bar(a, 0); break;
    case K_DEL: case 127: case 8: do_clear(a); break;
    case 27:      a->clip.cut = 0; d->sel_kind = 0; d->next = 0; break;
    case 1:       run_action(a, A_SELALL); break;          /* Ctrl+A */
    case 2:       run_action(a, A_FMT_BOLD); break;        /* Ctrl+B */
    case 26:      run_action(a, A_UNDO); break;            /* Ctrl+Z */
    case 25:      run_action(a, A_REDO); break;            /* Ctrl+Y */
    case 3:       run_action(a, A_COPY_RAW); break;        /* Ctrl+C */
    case 24:      run_action(a, A_CUT); break;             /* Ctrl+X */
    case 22:      run_action(a, A_PASTE); break;           /* Ctrl+V */
    case 4:       run_action(a, A_FILL_DOWN); break;       /* Ctrl+D */
    case 18:      run_action(a, A_FILL_RIGHT); break;      /* Ctrl+R */
    case 19:      run_action(a, A_SAVE); break;            /* Ctrl+S */
    case 14:      run_action(a, A_NEW); break;             /* Ctrl+N */
    case 15:      run_action(a, A_OPEN); break;            /* Ctrl+O */
    case 6:       run_action(a, A_FIND); break;            /* Ctrl+F */
    case 23:      run_action(a, A_CLOSE); break;           /* Ctrl+W */
    case 17:      run_action(a, A_QUIT); break;            /* Ctrl+Q */
    case 7:       run_action(a, A_GOTO); break;            /* Ctrl+G */
    default:
        if (k >= 32 && k < 256 && !ctrl) begin_edit(a, k);
    }
}

/* ---------- mouse ---------- */

static int col_index_at(const View *v, int x) {
    for (int j = 0; j < v->nc; j++)
        if (x >= v->x[j] && x <= v->x[j] + v->w[j]) return j;
    return -1;
}

static int row_index_at(const View *v, int y) {
    for (int i = 0; i < v->nr; i++)
        if (y >= v->y[i] && y <= v->y[i] + v->h[i]) return i;
    return -1;
}

/* Kind 1 cell, 2 column header, 3 row header, 0 none. */
static int grid_hit(const View *v, int y, int x, int *r, int *c) {
    int j = col_index_at(v, x), i = row_index_at(v, y);
    if (y == 3 && j >= 0) { *c = v->c[j]; return 2; }
    if (x < ROWHDR_W && i >= 0) { *r = v->r[i]; return 3; }
    if (i >= 0 && j >= 0) { *r = v->r[i]; *c = v->c[j]; return 1; }
    return 0;
}

/* Column whose divider is at x in the header line (resizes the column to its left). */
static int divider_col(const View *v, int y, int x, int *col) {
    if (y != 3 || !v->nc) return 0;
    if (x == v->xend) { *col = v->c[v->nc - 1]; return 1; }
    for (int j = 1; j < v->nc; j++)
        if (x == v->x[j]) { *col = v->c[j - 1]; return 1; }
    return 0;
}

/* Row whose bottom border is on line y in the number gutter. */
static int divider_row(const View *v, int y, int x, int *row) {
    if (x >= ROWHDR_W) return 0;
    for (int i = 0; i < v->nr; i++)
        if (y == v->y[i] + v->h[i]) { *row = v->r[i]; return 1; }
    return 0;
}

/* Replaces the reference inserted by the last click (if any) with text. */
static void formula_put(App *a, const char *text) {
    if (a->fref_len > 0) {
        memmove(a->ebuf + a->fref_pos, a->ebuf + a->fref_pos + a->fref_len,
                (size_t)(a->elen - a->fref_pos - a->fref_len + 1));
        a->elen -= a->fref_len;
        a->epos = a->fref_pos;
    }
    int n = (int)strlen(text);
    if (a->elen + n >= EBUF_MAX - 1) { a->fref_len = 0; return; }
    memmove(a->ebuf + a->epos + n, a->ebuf + a->epos, (size_t)(a->elen - a->epos + 1));
    memcpy(a->ebuf + a->epos, text, (size_t)n);
    a->fref_pos = a->epos;
    a->fref_len = n;
    a->epos += n;
    a->elen += n;
}

static void cell_name(int r, int c, char *out, size_t n) {
    char cn[8];
    col_name(c, cn);
    snprintf(out, n, "%s%d", cn, r + 1);
}

static void handle_mouse(App *a, const Event *e) {
    Doc *d = D(a);
    if (e->mtype == MS_MOVE) {                       /* the title under the pointer is highlighted */
        a->mhov = -1;
        if (e->my == 0)
            for (int i = 0; menubar[i].title; i++)
                if (e->mx >= ui_menu_x(i) && e->mx < ui_menu_x(i) + ui_menu_w(i)) a->mhov = i;
    }
    if (a->mode == MODE_DLG) { dlg_mouse(a, e); return; }
    int shift = e->mods & MOD_SHIFT;
    int ctrl = e->mods & MOD_CTRL;
    int press = e->mtype == MS_PRESS;
    /* formula mode: an edit that starts with '=' keeps the edit open while
       clicks insert cell references */
    int formula = a->mode == MODE_EDIT && a->ebuf[0] == '=';

    if (a->mode == MODE_LIST) {
        int x, y, w, h;
        ui_list_geom(a, &x, &y, &w, &h);
        if (e->mx >= x && e->mx < x + w && e->my > y && e->my < y + h - 1) {
            if (press) {
                int idx = a->ltop + (e->my - y - 1);
                if (idx < a->nlist) { a->lsel = idx; list_activate(a); }
            }
            return;
        }
        if (press) { list_free(a); a->mode = MODE_NORMAL; }
        return;
    }

    if (a->npanels) {
        if (e->mtype == MS_WHEEL_UP || e->mtype == MS_WHEEL_DOWN) return;
        for (int i = a->npanels - 1; i >= 0; i--) {
            int idx = panel_hit(&a->pan[i], e->my, e->mx);
            if (idx == -2) return;                       /* separator: keep menu open */
            if (idx < 0) continue;
            Panel *p = &a->pan[i];
            if (e->mtype == MS_MOVE) {
                if (i == 0) {
                    if (a->npanels == 2) a->npanels = 1;
                    p->sel = idx;
                    if (p->items[idx].sub) menu_open_sub(a);
                } else p->sel = idx;
                return;
            }
            if (press) {
                p->sel = idx;
                if (p->items[idx].sub) { if (i == 0) menu_open_sub(a); }
                else { a->npanels = i + 1; menu_activate(a); }
            }
            return;
        }
        if (e->my == 0) {                                /* menu bar hover/click */
            for (int i = 0; menubar[i].title; i++) {
                if (e->mx >= ui_menu_x(i) && e->mx < ui_menu_x(i) + ui_menu_w(i)) {
                    if (press && a->mb == i) menu_close(a);
                    else if (press || (e->mtype == MS_MOVE && a->mb != i && menubar[i].items))
                        menu_open_bar(a, i);
                    return;
                }
            }
        }
        if (!press) return;                              /* outside: close and handle the click */
        menu_close(a);
    }

    if (!a->npanels && e->mtype == MS_MOVE && e->my == 0 && !a->dragging) {
        /* hover over a title in the menu bar: its pop-up opens, no click needed */
        for (int i = 0; menubar[i].title; i++) {
            if (e->mx >= ui_menu_x(i) && e->mx < ui_menu_x(i) + ui_menu_w(i)) {
                if (menubar[i].items) menu_open_bar(a, i);
                return;
            }
        }
    }

    if (e->mtype == MS_MOVE && !a->dragging) {          /* plain pointer motion: hover */
        View hv;
        view_build(a, &hv);
        int hr = -1, hc = -1;
        if (grid_hit(&hv, e->my, e->mx, &hr, &hc) != 1) hr = hc = -1;
        a->hov_r = hr;
        a->hov_c = hc;
        return;
    }

    if (e->mtype == MS_WHEEL_UP || e->mtype == MS_WHEEL_DOWN) {
        int delta = e->mtype == MS_WHEEL_UP ? -3 : 3;
        d->top += delta;
        if (d->top < 0) d->top = 0;
        if (d->top > MAX_ROWS - 1) d->top = MAX_ROWS - 1;
        return;
    }

    if (e->my == 0 && press) {                           /* open a menu from the bar */
        for (int i = 0; menubar[i].title; i++)
            if (e->mx >= ui_menu_x(i) && e->mx < ui_menu_x(i) + ui_menu_w(i)) {
                menu_open_bar(a, i);
                return;
            }
    }
    if (e->my == a->rows - 3 && press) {                 /* sheet tabs */
        for (int i = 0; i < a->ndocs; i++)
            if (e->mx >= a->tabx[i] && e->mx < a->tabx[i] + a->tabw[i]) {
                if (a->mode == MODE_EDIT) end_edit(a, 1);
                a->cur = i;
                return;
            }
        return;
    }

    if (a->mode == MODE_EDIT && press && !formula) end_edit(a, 1);
    if (e->mtype == MS_RELEASE) {
        if (a->drag_kind == 7) fill_apply(a);
        a->dragging = 0;
        a->drag_kind = 0;
        return;
    }

    View v;
    view_build(a, &v);
    int kind, r = 0, c = 0, idx = 0;
    kind = grid_hit(&v, e->my, e->mx, &r, &c);

    if (formula) {
        char name[32];
        if (e->mtype == MS_PRESS && e->btn == 0 && kind == 1) {
            /* a click inserts the cell; dragging turns it into a range */
            cell_name(r, c, name, sizeof name);
            a->fref_len = 0;
            formula_put(a, name);
            a->rs_idx = r;               /* anchor of a range drag */
            a->rs_orig = c;
            a->drag_kind = 6;
            a->dragging = 1;
        } else if (e->mtype == MS_MOVE && a->dragging && a->drag_kind == 6 && kind == 1) {
            char a1[32], a2[32], range[72];
            cell_name(a->rs_idx, a->rs_orig, a1, sizeof a1);
            cell_name(r, c, a2, sizeof a2);
            if (!strcmp(a1, a2)) snprintf(range, sizeof range, "%s", a1);
            else snprintf(range, sizeof range, "%s:%s", a1, a2);
            formula_put(a, range);
        }
        return;                          /* clicks never change the selection in formula mode */
    }

    if (e->mtype == MS_PRESS && e->btn == 0 && e->my == 3 && e->mx < ROWHDR_W) {
        run_action(a, A_SELALL);                      /* the corner of the headers: select all */
        return;
    }

    if (e->mtype == MS_PRESS && e->btn == 2) {           /* right button: context menu */
        if (kind == 1) {
            if (!in_selection(d, r, c)) move_to(a, r, c, 0);
            menu_open_ctx(a, e->mx, e->my);
        }
        return;
    }

    if (e->mtype == MS_PRESS && e->btn == 0) {
        if (divider_col(&v, e->my, e->mx, &idx)) {       /* drag a column divider */
            a->drag_kind = 4;
            a->rs_idx = idx;
            a->rs_orig = doc_colw(d, idx);
            a->rs_anchor = e->mx;
            a->dragging = 1;
            return;
        }
        if (divider_row(&v, e->my, e->mx, &idx)) {       /* drag a row divider */
            a->drag_kind = 5;
            a->rs_idx = idx;
            a->rs_orig = doc_rowh(d, idx);
            a->rs_anchor = e->my;
            a->dragging = 1;
            return;
        }
        if ((kind == 2 || kind == 3) && ctrl) {        /* Ctrl+click: unselect a lit header, else add it */
            int by_col = kind == 2, idx = by_col ? c : r;
            if (doc_hdr_sel(d, idx, by_col)) {
                Rect s = by_col ? (Rect){0, idx, MAX_ROWS - 1, idx} : (Rect){idx, 0, idx, MAX_COLS - 1};
                if (area_remove(a, s)) snprintf(a->msg, sizeof a->msg, "%s unselected", by_col ? "column" : "row");
                return;
            }
            area_push(a);
        } else if ((kind == 2 || kind == 3) && !shift) {
            d->next = 0;
        }
        if (!d->next && fill_handle_hit(a, &v, e->mx, e->my)) {       /* drag the fill handle */
            a->drag_kind = 7;
            a->dragging = 1;
            a->fr2 = a->fc2 = -1;
            return;
        }
        if (kind == 1 && ctrl) {                       /* Ctrl+click: a selected cell is unselected, else added */
            if (doc_in_sel(d, r, c)) {
                Rect cell = {r, c, r, c};
                if (area_remove(a, cell)) snprintf(a->msg, sizeof a->msg, "cell unselected");
                return;
            }
            area_push(a);
            move_keep(a, r, c, 0);
            a->drag_kind = 1;
        } else if (kind == 1) {
            double t = now_sec();
            if (t - a->last_t < 0.5 && r == a->last_r && c == a->last_c) {
                a->last_t = -10;
                move_to(a, r, c, 0);
                begin_edit(a, 0);
                a->mode = MODE_EDIT;
                return;
            }
            a->last_t = t;
            a->last_r = r;
            a->last_c = c;
            move_to(a, r, c, shift);
            a->drag_kind = 1;
        } else if (kind == 2) {
            select_cols(a, c, shift);
            a->drag_kind = 2;
        } else if (kind == 3) {
            select_rows(a, r, shift);
            a->drag_kind = 3;
        } else return;
        a->dragging = 1;
        ensure_visible(a);
        return;
    }

    if (e->mtype == MS_MOVE && a->dragging) {
        if (a->drag_kind == 7) {                         /* fill: follow the pointer */
            int row = row_index_at(&v, e->my);
            if (row < 0) row = e->my < 5 ? d->top : (v.nr ? v.r[v.nr - 1] : d->top);
            int j = col_index_at(&v, e->mx);
            int col = j >= 0 ? v.c[j] : e->mx > v.xend && v.nc ? v.c[v.nc - 1] : d->left;
            fill_track(a, row, col);
            return;
        }
        if (a->drag_kind == 4) {                         /* column width follows the pointer */
            int w = a->rs_orig + (e->mx - a->rs_anchor);
            if (w < COL_MIN_W) w = COL_MIN_W;
            if (w > COL_MAX_W) w = COL_MAX_W;
            d->cw[a->rs_idx] = (unsigned char)w;
            return;
        }
        if (a->drag_kind == 5) {                         /* row height follows the pointer */
            int h = a->rs_orig + (e->my - a->rs_anchor);
            if (h < ROW_DEF_H) h = ROW_DEF_H;
            if (h > ROW_MAX_H) h = ROW_MAX_H;
            d->rh[a->rs_idx] = (unsigned char)h;
            return;
        }
        /* selection drag, autoscrolling at the grid edges */
        int bottom = v.nr ? v.y[v.nr - 1] + v.h[v.nr - 1] : 5;
        int row;
        if (e->my < 5) {
            if (d->top > 0) d->top--;
            row = d->top;
        } else if (e->my > bottom) {
            d->top++;
            view_build(a, &v);
            row = v.nr ? v.r[v.nr - 1] : d->top;
        } else {
            int i = row_index_at(&v, e->my);
            row = i >= 0 ? v.r[i] : d->top;
        }
        int col = d->left;
        if (e->mx > v.xend && v.nc) col = v.c[v.nc - 1];
        else {
            int j = col_index_at(&v, e->mx);
            if (j >= 0) col = v.c[j];
        }
        if (row >= MAX_ROWS) row = MAX_ROWS - 1;
        if (a->drag_kind == 1) move_to(a, row, col, 1);
        else if (a->drag_kind == 2) select_cols(a, col, 1);
        else if (a->drag_kind == 3) select_rows(a, row, 1);
        ensure_visible(a);
    }
}

/* ---------- input dispatch ---------- */

static void handle_key(App *a, const Event *e) {
    if (a->npanels) { menu_key(a, e); return; }
    if (a->mode == MODE_DLG) { dlg_key(a, e); return; }
    if (a->mode == MODE_LIST) { list_key(a, e); return; }
    if (a->mode == MODE_RESIZE) { resize_key(a, e); return; }
    if (a->mode == MODE_NORMAL && a->vim) { vim_normal(a, e); return; }
    if (a->mode == MODE_PROMPT) { prompt_key(a, e); return; }
    if (a->mode == MODE_EDIT) { edit_key(a, e); return; }
    normal_key(a, e);
}

static void handle_event(App *a, const Event *e);

/* One journal per event: everything an event changes is one undo step. */
static void handle(App *a, const Event *e) {
    Txn *t = calloc(1, sizeof *t);
    a->warn = 0;
    if (t) txn_begin(t);
    handle_event(a, e);
    if (t) {
        txn_end();
        if (t->n) {
            D(a)->dirty = 1;
            hist_push(undo_s, &nundo, t);
            hist_clear(redo_s, &nredo);       /* a new change ends the redo chain */
        } else txn_free(t);
    }
}

static void handle_event(App *a, const Event *e) {
    if (e->type == EV_RESIZE) {
        term_size(&a->rows, &a->cols);
        ensure_visible(a);
        return;
    }
    if (e->type == EV_MOUSE) handle_mouse(a, e);
    else if (e->type == EV_KEY) handle_key(a, e);
    ensure_visible(a);
}

/* ---------- startup ---------- */

static int fmt_by_name(const char *s) {
    for (int i = 0; i < FMT_COUNT; i++)
        if (!strcasecmp(s, io_fmt_name(i))) return i;
    return -1;
}

static void usage(void) {
    fputs("usage: tsheet [-f csv|tsv|xml] [file ...]\n"
          "       producer | tsheet [-f fmt]   (stdin is read as a sheet)\n"
          "Files open as sheets; formats are picked from the extension.\n", stderr);
}

int main(int argc, char **argv) {
    App *a = calloc(1, sizeof *a);
    if (!a) return 1;
    char *files[64];
    int nf = 0, stdin_fmt = -1, use_stdin = !isatty(0);
    a->vim = vim_default;
    a->mhov = -1;
    a->hov_r = a->hov_c = -1;
    snprintf(a->gextra, sizeof a->gextra, "%s", GRAPH_DEFAULTS);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        if (!strcmp(argv[i], "--version")) { printf("%s %s\n", TSHEET_NAME, TSHEET_VERSION); return 0; }
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            stdin_fmt = fmt_by_name(argv[++i]);
            if (stdin_fmt < 0) { usage(); return 1; }
            continue;
        }
        if (!strcmp(argv[i], "-")) { use_stdin = 1; continue; }
        if (!strcmp(argv[i], "--vim")) { a->vim = 1; continue; }
        if (nf < 64) files[nf++] = argv[i];
    }

    if (use_stdin) {
        char *buf;
        size_t len;
        if (io_read_fd(0, &buf, &len) < 0) { fprintf(stderr, "tsheet: cannot read stdin\n"); return 1; }
        int fmt = stdin_fmt >= 0 ? stdin_fmt : io_fmt_from_text(buf, len);
        Sheet *g = sheet_new();
        char err[256];
        if (!g || io_parse_delim(g, buf, len, fmt, fmt == FMT_CSV ? a->imp_delim : 0, err, sizeof err) < 0) {
            fprintf(stderr, "tsheet: stdin is not a valid %s sheet: %s\n", io_fmt_name(fmt), err);
            free(buf);
            sheet_free(g);
            return 1;
        }
        free(buf);
        doc_add(a, "stdin", NULL, fmt, g);
    }
    for (int i = 0; i < nf; i++) load_path(a, files[i]);
    if (a->ndocs == 0) doc_new(a);

    if (term_init() < 0) {
        fprintf(stderr, "tsheet: no terminal available\n");
        return 1;
    }
    term_size(&a->rows, &a->cols);
    a->mb = -1;
    ensure_visible(a);

    Event e;
    ui_draw(a);
    term_flush();
    while (!a->quit) {
        int r = term_poll(&e, -1);
        if (r < 0) break;
        if (r == 0) continue;
        handle(a, &e);
        while (!a->quit && (r = term_poll(&e, 0)) > 0) handle(a, &e);   /* coalesce bursts */
        if (a->quit) break;
        if (a->mode == MODE_DLG) dlg_refresh(a);
        ui_draw(a);
        term_flush();
    }

    term_fini();
    for (int i = 0; i < a->ndocs; i++) doc_free(&a->docs[i]);
    list_free(a);
    clip_clear(a);
    free(a);
    return 0;
}
