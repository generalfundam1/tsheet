/* Rendering: a cell buffer is composed each frame and only the cells that
   changed since the last frame are sent to the terminal. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsheet.h"
#include "config.h"

typedef struct { uint32_t cp; unsigned char at, fg; } Cell;   /* fg: kind + 1, 0 = none */

enum { AT_NORM, AT_REV, AT_BOLD, AT_SEL, AT_MENU, AT_MENUSEL, AT_BORDER,
       AT_ACTIVE, AT_BAR, AT_TAB, AT_TABSEL, AT_HDR, AT_ERR, AT_HOVER, AT_INV, AT_HL, AT_HLB,
       AT_WARN, AT_SELB, AT_SELHL, AT_SELHLB, AT_ACTHL, AT_COUNT };

static const char *sgr[AT_COUNT] = {
    "0", "0;7", "0;1", "0;30;46", "0;30;47", "0;1;30;46", "0;90",
    "0;1;7", "0;7", "0;7", "0;1;4", "0;1;7", "0;1;31", HOVER_SGR, "0;97;40",
    "0;30;" HIGHLIGHT_SGR, "0;1;30;" HIGHLIGHT_SGR,      /* highlighted text is dark on the yellow */
    "0;1;97;41",                                         /* warnings: bold white on red */
    "0;1;30;46",                                         /* selected and bold */
    "0;4;30;" HIGHLIGHT_SGR,                             /* selected and highlighted: underlined */
    "0;1;4;30;" HIGHLIGHT_SGR,                           /* selected, bold and highlighted */
    "0;1;4;30;" HIGHLIGHT_SGR                            /* the active cell, highlighted */
};

static Cell *cur_b, *prev_b;
static unsigned char pen_fg;       /* colour for the text put next */
static int bw, bh;

#define CELL_W 10                       /* content width of one grid cell */

static void frame_size(int rows, int cols) {
    if (rows == bh && cols == bw && cur_b) return;
    free(cur_b);
    free(prev_b);
    bw = cols;
    bh = rows;
    cur_b = calloc((size_t)bw * bh, sizeof *cur_b);
    prev_b = calloc((size_t)bw * bh, sizeof *prev_b);
    if (!cur_b || !prev_b) { free(cur_b); free(prev_b); cur_b = prev_b = NULL; bw = bh = 0; return; }
    for (size_t i = 0; i < (size_t)bw * bh; i++) {
        cur_b[i].cp = ' ';
        cur_b[i].at = AT_NORM;
        cur_b[i].fg = 0;
        prev_b[i].cp = 0xFFFFFFFFu;         /* force a full repaint */
    }
    term_write("\x1b[2J", 4);
}

static void put(int y, int x, uint32_t cp, int at) {
    if (y < 0 || x < 0 || y >= bh || x >= bw) return;
    cur_b[(size_t)y * bw + x].cp = cp;
    cur_b[(size_t)y * bw + x].at = (unsigned char)at;
    cur_b[(size_t)y * bw + x].fg = pen_fg;
}

static void fill(int y, int x0, int x1, uint32_t cp, int at) {
    for (int x = x0; x < x1; x++) put(y, x, cp, at);
}

/* Decodes one UTF-8 character; malformed bytes become '?'. */
static uint32_t u8dec(const char **sp) {
    const unsigned char *p = (const unsigned char *)*sp;
    uint32_t c = p[0];
    int n;
    if (c < 0x80) { *sp += 1; return c; }
    if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; }
    else { *sp += 1; return '?'; }
    for (int k = 1; k <= n; k++) {
        if ((p[k] & 0xC0) != 0x80) { *sp += 1; return '?'; }
        c = (c << 6) | (p[k] & 0x3F);
    }
    *sp += n + 1;
    return c;
}

static int u8len(const char *s) {
    int n = 0;
    while (*s) { u8dec(&s); n++; }
    return n;
}

/* Writes at most w characters; returns how many were written. */
static int puts_at(int y, int x, const char *s, int at, int w) {
    int n = 0;
    while (*s && n < w) {
        put(y, x + n, u8dec(&s), at);
        n++;
    }
    return n;
}

/* ---------- grid ---------- */

/* ---------- geometry ---------- */

/* Lays out the visible columns and rows of the current sheet. */
void view_build(const App *a, View *v) {
    const Doc *d = &a->docs[a->cur];
    v->nc = v->nr = 0;
    int x = ROWHDR_W;
    for (int c = d->left; c < MAX_COLS && v->nc < VIEW_MAX; c++) {
        if (x >= a->cols - 1) break;
        int w = doc_colw(d, c);
        v->c[v->nc] = c;
        v->x[v->nc] = x;
        v->w[v->nc] = w;
        v->nc++;
        x += w + 1;
    }
    v->xend = v->nc ? v->x[v->nc - 1] + v->w[v->nc - 1] + 1 : ROWHDR_W;
    int y = 5;                                  /* first row starts below the header */
    for (int r = d->top; r < MAX_ROWS && v->nr < VIEW_MAX; r++) {
        int h = doc_rowh(d, r);
        if (y + h > a->rows - 4) break;         /* border must stay above the tab bar */
        v->r[v->nr] = r;
        v->y[v->nr] = y;
        v->h[v->nr] = h;
        v->nr++;
        y += h + 1;
    }
}

static void vline_row(int y, int h, const View *v) {
    for (int k = 0; k < h; k++) {
        put(y + k, 0, 0x2502, AT_BORDER);
        put(y + k, ROWHDR_W, 0x2502, AT_BORDER);
        for (int j = 0; j < v->nc; j++) put(y + k, v->x[j], 0x2502, AT_BORDER);
        put(y + k, v->xend, 0x2502, AT_BORDER);
    }
}

/* kind 0 top, 1 middle, 2 bottom */
static void hline_v(int y, int kind, const View *v) {
    static const uint32_t L[3] = {0x250C, 0x251C, 0x2514};
    static const uint32_t M[3] = {0x252C, 0x253C, 0x2534};
    static const uint32_t R[3] = {0x2510, 0x2524, 0x2518};
    for (int x = 1; x < v->xend; x++) put(y, x, 0x2500, AT_BORDER);
    put(y, 0, L[kind], AT_BORDER);
    put(y, ROWHDR_W, M[kind], AT_BORDER);
    for (int j = 0; j < v->nc; j++) put(y, v->x[j], M[kind], AT_BORDER);
    put(y, v->xend, R[kind], AT_BORDER);
}

/* Statistics over the selected block, for the status bar. */
static void stats(const Doc *d, double *sum, int *nums, int *count) {
    Rect q[MAX_RECTS];
    int n = doc_rects(d, q);
    *sum = 0;
    *nums = 0;
    *count = 0;
    for (int i = 0; i < n; i++)
        for (int r = q[i].r1; r <= q[i].r2; r++)
            for (int c = q[i].c1; c <= q[i].c2; c++) {
                int seen = 0;                                  /* a cell in two areas counts once */
                for (int j = 0; j < i && !seen; j++) seen = rect_has(&q[j], r, c);
                if (seen) continue;
                const Merge *m = sheet_merge_of(d->g, r, c);
                if (m && (m->r1 != r || m->c1 != c)) continue; /* a merged block counts once */
                double v;
                int st = cell_num(d->g, r, c, 0, &v);
                if (st == 3) continue;
                (*count)++;
                if (st == 0) { *sum += v; (*nums)++; }
            }
}

/* Attribute for a cell: the selection and cursor win, then hover, then the
   format's highlight and bold. */
static int cell_attr(int active, int sel, int hovered, int bold, int hl) {
    /* formats stay visible in a selection: bold keeps its weight, highlight keeps its colour */
    if (active) return hl ? AT_ACTHL : AT_ACTIVE;
    if (sel) {
        if (hl) return bold ? AT_SELHLB : AT_SELHL;
        return bold ? AT_SELB : AT_SEL;
    }
    if (hovered) return AT_HOVER;
    if (hl && bold) return AT_HLB;
    if (hl) return AT_HL;
    if (bold) return AT_BOLD;
    return AT_NORM;
}

/* A merged block is one cell: no borders inside it, its value centred across it. */
static void block_draw(const App *a, const View *v, const Merge *m) {
    const Doc *d = &a->docs[a->cur];
    int ja = -1, jb = -1, ia = -1, ib = -1;
    for (int j = 0; j < v->nc; j++)
        if (v->c[j] >= m->c1 && v->c[j] <= m->c2) { if (ja < 0) ja = j; jb = j; }
    for (int i = 0; i < v->nr; i++)
        if (v->r[i] >= m->r1 && v->r[i] <= m->r2) { if (ia < 0) ia = i; ib = i; }
    if (ja < 0 || ia < 0) return;
    int x0 = v->x[ja] + 1, xr = v->x[jb] + v->w[jb];
    int y0 = v->y[ia], y1 = v->y[ib] + v->h[ib] - 1;
    int active = m->r1 == d->cr && m->c1 == d->cc;
    int inside = doc_in_sel(d, m->r1, m->c1);
    int hovered = a->hov_r >= m->r1 && a->hov_r <= m->r2 && a->hov_c >= m->c1 && a->hov_c <= m->c2;
    int bold, hl;
    sheet_style(d->g, m->r1, m->c1, &bold, &hl);
    int at = cell_attr(active, inside, hovered, bold, hl);
    for (int y = y0; y <= y1; y++) fill(y, x0, xr + 1, ' ', at);
    for (int k = ja + 1; k <= jb; k++)                        /* no inner vertical lines */
        for (int y = y0; y <= y1; y++) put(y, v->x[k], ' ', at);
    for (int i = ia; i < ib; i++) {                           /* no inner horizontal lines */
        int ly = v->y[i] + v->h[i];
        for (int x = x0; x <= xr; x++) put(ly, x, ' ', at);
    }
    if (v->r[ia] == m->r1 && v->c[ja] == m->c1) {
        char txt[256];
        int align = 0;
        cell_display(d->g, m->r1, m->c1, txt, sizeof txt, &align);
        int W = xr - x0 + 1;
        int len = u8len(txt);
        if (len > W) len = W;
        int x = align == 1 ? x0 + W - len : align == 2 ? x0 + (W - len) / 2 : x0;
        int ty = y0 + (y1 - y0) / 2;
        int kind = cell_kind(d->g, m->r1, m->c1);
        pen_fg = (!active && !inside && !hl && kind >= 0) ? (unsigned char)(kind + 1) : 0;   /* highlighted text keeps its dark colour */
        puts_at(ty, x, txt, at, W);
        pen_fg = 0;
    }
}

/* ---------- borders ----------
   Every border character is chosen from its four neighbours: a line leaves a
   junction up, down, left or right only where the grid really has a line there.
   Lines inside a merged block are not drawn, so they break cleanly at its edges.
   Vertical positions t: 0 the left edge, 1 between the row header and column 0,
   t >= 2 between columns t-2 and t-1, nc+1 the right edge. Horizontal spaces t:
   0 the row header, 1..nc the columns (t-1). Boundaries b: 0 the top of the data,
   nr the bottom, else between data rows b-1 and b. */

#define BU 1
#define BD 2
#define BL 4
#define BR 8

static int bcell_same(const Sheet *g, int r1, int c1, int r2, int c2) {
    const Merge *a = sheet_merge_of(g, r1, c1);
    return a && a == sheet_merge_of(g, r2, c2);
}

static int bpos_x(const View *v, int t) {
    if (t == 0) return 0;
    if (t == v->nc + 1) return v->xend;
    return v->x[t - 1];
}

/* vertical line at position t beside data row index i */
static int bvseg(const Sheet *g, const View *v, int i, int t) {
    if (t <= 1 || t >= v->nc + 1) return 1;
    return !bcell_same(g, v->r[i], v->c[t - 2], v->r[i], v->c[t - 1]);
}

/* horizontal line at boundary b across space t */
static int bhseg(const Sheet *g, const View *v, int b, int t) {
    if (t == 0 || b == 0 || b == v->nr) return 1;
    return !bcell_same(g, v->r[b - 1], v->c[t - 1], v->r[b], v->c[t - 1]);
}

static uint32_t junction_char(int m) {
    switch (m) {
    case 0: return ' ';
    case BU | BD: return 0x2502;
    case BL | BR: return 0x2500;
    case BD | BR: return 0x250C;
    case BD | BL: return 0x2510;
    case BU | BR: return 0x2514;
    case BU | BL: return 0x2518;
    case BU | BD | BR: return 0x251C;
    case BU | BD | BL: return 0x2524;
    case BD | BL | BR: return 0x252C;
    case BU | BL | BR: return 0x2534;
    case BU | BD | BL | BR: return 0x253C;
    default:
        if (m & (BU | BD)) return 0x2502;
        return 0x2500;
    }
}

static void draw_borders(const App *a, const View *v) {
    const Sheet *g = a->docs[a->cur].g;
    int nc = v->nc, nr = v->nr;
    for (int b = 0; b <= nr; b++) {
        int y = b == 0 ? 4 : v->y[b - 1] + v->h[b - 1];
        for (int t = 0; t <= nc + 1; t++) {
            int x = bpos_x(v, t);
            int m = 0;
            if (b > 0 ? bvseg(g, v, b - 1, t) : 1) m |= BU;
            if (b < nr && bvseg(g, v, b, t)) m |= BD;
            if (t >= 1 && bhseg(g, v, b, t - 1)) m |= BL;
            if (t <= nc && bhseg(g, v, b, t)) m |= BR;
            put(y, x, junction_char(m), AT_BORDER);
            if (t <= nc) {                              /* the run to the next junction */
                uint32_t run = bhseg(g, v, b, t) ? 0x2500 : ' ';
                for (int xx = x + 1; xx < bpos_x(v, t + 1); xx++) put(y, xx, run, AT_BORDER);
            }
        }
    }
    for (int i = 0; i < nr; i++)
        for (int y = v->y[i]; y < v->y[i] + v->h[i]; y++)
            for (int t = 0; t <= nc + 1; t++)
                put(y, bpos_x(v, t), bvseg(g, v, i, t) ? 0x2502 : ' ', AT_BORDER);
}

static void draw_grid(const App *a) {
    const Doc *d = &a->docs[a->cur];
    View v;
    view_build(a, &v);
    char buf[256], num[16];

    hline_v(2, 0, &v);
    vline_row(3, 1, &v);
    for (int j = 0; j < v.nc; j++) {
        int c = v.c[j], w = v.w[j], x0 = v.x[j] + 1;
        char cn[8];
        col_name(c, cn);
        int l = (int)strlen(cn), pad = (w - l) / 2;
        int sel = doc_hdr_sel(d, c, 1);
        int at = sel ? AT_SEL : AT_HDR;
        fill(3, x0, x0 + w, ' ', at);
        puts_at(3, x0 + pad, cn, at, w - pad);
    }
    for (int i = 0; i < v.nr; i++) {
        int r = v.r[i], y = v.y[i], h = v.h[i];
        int mid = y + (h - 1) / 2;          /* text is centred vertically in the cell */

        int rsel = doc_hdr_sel(d, r, 0);
        int rat = rsel ? AT_SEL : AT_HDR;
        for (int k = 0; k < h; k++) fill(y + k, 1, ROWHDR_W, ' ', rat);
        snprintf(num, sizeof num, "%d", r + 1);
        int rl = (int)strlen(num), rpad = (ROWHDR_W - 1 - rl) / 2;
        puts_at(mid, 1 + rpad, num, rat, ROWHDR_W - 1 - rpad);

        for (int j = 0; j < v.nc; j++) {
            int c = v.c[j], w = v.w[j], x0 = v.x[j] + 1;
            const Merge *m = sheet_merge_of(d->g, r, c);
            if (m && (m->r1 != r || m->c1 != c)) continue;      /* drawn with its block */
            int active = r == d->cr && c == d->cc;
            int inside = doc_in_sel(d, r, c);
            int infill = a->drag_kind == 7 && a->dragging && r >= a->fr1 && r <= a->fr2
                      && c >= a->fc1 && c <= a->fc2;
            int hovered = r == a->hov_r && c == a->hov_c;
            int bold, hl;
            sheet_style(d->g, r, c, &bold, &hl);
            int at = cell_attr(active, inside || infill, hovered, bold, hl);
            for (int k = 0; k < h; k++) fill(y + k, x0, x0 + w, ' ', at);
            /* colour by kind, except on selected cells where the selection colours win */
            int kind = cell_kind(d->g, r, c);
            pen_fg = (!active && !inside && !hl && kind >= 0) ? (unsigned char)(kind + 1) : 0;   /* highlighted text keeps its dark colour */

            const char *text;
            int align = 0, skip = 0;
            if (active && a->mode == MODE_EDIT) {
                text = a->ebuf;
                skip = a->epos > w - 1 ? a->epos - (w - 1) : 0;
            } else {
                cell_display(d->g, r, c, buf, sizeof buf, &align);
                text = buf;
            }
            int len = u8len(text + skip);
            if (len > w) len = w;
            int x = align == 1 ? x0 + w - len : align == 2 ? x0 + (w - len) / 2 : x0;
            puts_at(mid, x, text + skip, at, w);
            pen_fg = 0;
        }
    }

    draw_borders(a, &v);
    for (int mi = 0; mi < d->g->nmg; mi++) block_draw(a, &v, &d->g->mg[mi]);

    /* fill handle: a square at the bottom-right corner of a cell selection */
    if (d->sel_kind == 0) {
        int hr2 = d->ar > d->cr ? d->ar : d->cr, hc2 = d->ac > d->cc ? d->ac : d->cc;
        for (int i = 0; i < v.nr; i++)
            for (int j = 0; j < v.nc; j++)
                if (v.r[i] == hr2 && v.c[j] == hc2)
                    put(v.y[i] + v.h[i], v.x[j] + v.w[j] + 1, 0x25A0, AT_BOLD);
    }
}

/* ---------- menus and lists ---------- */

static int panel_count(const MenuItem *items) {
    int n = 0;
    while (items[n].label) n++;
    return n;
}

int ui_panel_w(const MenuItem *items) {
    int w = 20;
    for (int i = 0; items[i].label; i++) {
        if (!strcmp(items[i].label, "-")) continue;
        int l = (int)strlen(items[i].label) + (int)strlen(items[i].key) + 6;
        if (l > w) w = l;
    }
    return w;
}

int ui_panel_h(const MenuItem *items) { return panel_count(items) + 2; }

int ui_menu_x(int i) {
    int x = 1;
    for (int j = 0; j < i && menubar[j].title; j++) x += (int)strlen(menubar[j].title) + 3;
    return x;
}

int ui_menu_w(int i) { return (int)strlen(menubar[i].title) + 2; }

static void draw_panel(const App *a, const Panel *p) {
    int w = ui_panel_w(p->items), h = ui_panel_h(p->items), x = p->x;
    for (int y = p->y; y < p->y + h; y++) fill(y, x, x + w, ' ', AT_MENU);
    put(p->y, x, 0x250C, AT_MENU);
    put(p->y, x + w - 1, 0x2510, AT_MENU);
    put(p->y + h - 1, x, 0x2514, AT_MENU);
    put(p->y + h - 1, x + w - 1, 0x2518, AT_MENU);
    for (int k = x + 1; k < x + w - 1; k++) {
        put(p->y, k, 0x2500, AT_MENU);
        put(p->y + h - 1, k, 0x2500, AT_MENU);
    }
    for (int y = p->y + 1; y < p->y + h - 1; y++) {
        put(y, x, 0x2502, AT_MENU);
        put(y, x + w - 1, 0x2502, AT_MENU);
    }
    for (int i = 0; p->items[i].label; i++) {
        int row = p->y + 1 + i;
        const MenuItem *it = &p->items[i];
        if (!strcmp(it->label, "-")) {
            put(row, x, 0x251C, AT_MENU);
            for (int k = x + 1; k < x + w - 1; k++) put(row, k, 0x2500, AT_MENU);
            put(row, x + w - 1, 0x2524, AT_MENU);
            continue;
        }
        int at = i == p->sel ? AT_MENUSEL : AT_MENU;
        fill(row, x + 1, x + w - 1, ' ', at);
        puts_at(row, x + 2, it->label, at, w - 4);
        if (it->act && act_state(a, it->act)) put(row, x + 1, '*', at);   /* applied */
        if (it->sub) put(row, x + w - 3, '>', at);
        else if (it->key[0]) {
            int kl = (int)strlen(it->key);
            puts_at(row, x + w - 3 - kl, it->key, at, kl);
        }
    }
}

void ui_list_geom(const App *a, int *x, int *y, int *w, int *h) {
    int longest = 40;
    for (int i = 0; i < a->nlist; i++) {
        int l = u8len(a->list[i]) + 4;
        if (l > longest) longest = l;
    }
    *w = longest > a->cols - 4 ? a->cols - 4 : longest;
    if (*w < 20) *w = 20;
    *h = a->nlist + 2;
    if (*h > a->rows - 6) *h = a->rows - 6;
    if (*h < 3) *h = 3;
    *x = (a->cols - *w) / 2;
    *y = 3;
}

static void draw_list(const App *a) {
    int x, y, w, h;
    ui_list_geom(a, &x, &y, &w, &h);
    for (int r = y; r < y + h; r++) fill(r, x, x + w, ' ', AT_MENU);
    put(y, x, 0x250C, AT_MENU);
    put(y, x + w - 1, 0x2510, AT_MENU);
    put(y + h - 1, x, 0x2514, AT_MENU);
    put(y + h - 1, x + w - 1, 0x2518, AT_MENU);
    for (int k = x + 1; k < x + w - 1; k++) {
        put(y, k, 0x2500, AT_MENU);
        put(y + h - 1, k, 0x2500, AT_MENU);
    }
    for (int r = y + 1; r < y + h - 1; r++) {
        put(r, x, 0x2502, AT_MENU);
        put(r, x + w - 1, 0x2502, AT_MENU);
    }
    const char *title = a->lkind == LIST_FILES
        ? " Find results: Enter opens, Esc closes " : " Keys: Esc closes ";
    puts_at(y, x + 2, title, AT_MENU, w - 4);
    int rows = h - 2;
    for (int i = 0; i < rows && a->ltop + i < a->nlist; i++) {
        int idx = a->ltop + i, row = y + 1 + i;
        int at = idx == a->lsel ? AT_MENUSEL : AT_MENU;
        fill(row, x + 1, x + w - 1, ' ', at);
        puts_at(row, x + 2, a->list[idx], at, w - 4);
    }
}

/* ---------- graph options editor ----------
   Layout: title border, key help, option list (scrolls), description of the
   highlighted option, command preview, bottom border. */

#define DLG_CHROME 12                     /* rows that are not the list */
#define DLG_DESC_ROWS 2
#define DLG_PREVIEW_ROWS 4

int ui_dlg_list_rows(const App *a) {
    int room = a->rows - DLG_CHROME - 2;  /* keep a little of the grid visible */
    if (room < 3) room = 3;
    return a->ndlg < room ? a->ndlg : room;
}

void ui_dlg_geom(const App *a, int *x, int *y, int *w, int *h) {
    *w = 78;
    if (*w > a->cols - 4) *w = a->cols - 4;
    if (*w < 30) *w = 30;
    *h = ui_dlg_list_rows(a) + DLG_CHROME;
    *x = (a->cols - *w) / 2;
    *y = (a->rows - *h) / 2;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

/* writes text wrapped to width w on consecutive rows, at most rows lines */
static void put_wrapped(int y, int x, const char *s, int w, int rows, int at) {
    const char *p = s;
    for (int r = 0; r < rows && *p; r++) {
        int n = 0;
        const char *q = p;
        while (*q && n < w) { q++; n++; }
        if (*q && q != p) {                  /* break at the last blank if possible */
            const char *b = q;
            while (b > p && *b != ' ') b--;
            if (b > p) q = b + 1;
        }
        int len = (int)(q - p);
        puts_at(y + r, x, p, at, len > w ? w : len);
        p = q;
        while (*p == ' ') p++;
    }
}

static void draw_dlg(const App *a) {
    int x, y, w, h;
    ui_dlg_geom(a, &x, &y, &w, &h);
    int L = ui_dlg_list_rows(a);
    for (int r = y; r < y + h; r++) fill(r, x, x + w, ' ', AT_MENU);
    put(y, x, 0x250C, AT_MENU);
    put(y, x + w - 1, 0x2510, AT_MENU);
    put(y + h - 1, x, 0x2514, AT_MENU);
    put(y + h - 1, x + w - 1, 0x2518, AT_MENU);
    for (int k = x + 1; k < x + w - 1; k++) {
        put(y, k, 0x2500, AT_MENU);
        put(y + h - 1, k, 0x2500, AT_MENU);
    }
    for (int r = y + 1; r < y + h - 1; r++) {
        put(r, x, 0x2502, AT_MENU);
        put(r, x + w - 1, 0x2502, AT_MENU);
    }
    /* separators inside the box */
    int seps[3] = {y + 2 + L, y + 5 + L, 0};
    for (int s = 0; s < 2; s++) {
        put(seps[s], x, 0x251C, AT_MENU);
        put(seps[s], x + w - 1, 0x2524, AT_MENU);
        for (int k = x + 1; k < x + w - 1; k++) put(seps[s], k, 0x2500, AT_MENU);
    }

    int applied = 0;
    for (int i = 0; i < a->ndlg; i++) applied += a->dlg[i].inv;
    char title[96];
    if (a->acts) snprintf(title, sizeof title, " %s   Enter applies, Esc closes ", a->acts_title);
    else if (a->dlg_kind == DLGK_MAIN) snprintf(title, sizeof title, " Graph options   %d applied ", applied);
    else snprintf(title, sizeof title, " %s   choose a value ", graph_opts[a->dlg_opt].name);
    puts_at(y, x + 2, title, AT_MENU, w - 4);
    puts_at(y + 1, x + 2, a->acts ? "Enter/Space runs it   Esc closes   [*] = on"
                                  : "Enter/Space sets   Esc goes back   inverted = applied",
            AT_MENU, w - 4);

    /* the option list */
    for (int i = 0; i < L; i++) {
        int idx = a->dlg_top + i;
        if (idx >= a->ndlg) break;
        int row = y + 2 + i;
        int at = idx == a->dlg_sel ? AT_MENUSEL : a->dlg[idx].inv ? AT_INV : AT_MENU;
        fill(row, x + 1, x + w - 1, ' ', at);
        puts_at(row, x + 2, a->dlg[idx].label, at, w - 4);
    }
    if (a->dlg_top > 0) put(y + 2, x + w - 2, '^', AT_MENU);
    if (a->dlg_top + L < a->ndlg) put(y + 1 + L, x + w - 2, 'v', AT_MENU);

    /* description of the highlighted item */
    if (a->dlg_sel >= 0 && a->dlg_sel < a->ndlg && a->dlg[a->dlg_sel].desc)
        put_wrapped(y + 3 + L, x + 2, a->dlg[a->dlg_sel].desc, w - 4, DLG_DESC_ROWS, AT_MENU);

    if (a->acts) return;                       /* an action list has no command preview */
    /* command preview: the gnuplot commands the field produces */
    puts_at(y + 6 + L, x + 2, "gnuplot commands:", AT_BOLD, w - 4);
    put_wrapped(y + 7 + L, x + 2, a->gextra[0] ? a->gextra : "(none)", w - 4, DLG_PREVIEW_ROWS, AT_NORM);
}

/* ---------- frame ---------- */

static void flush_frame(void) {
    int lastat = -1, lastfg = -1, lasty = -1, lastx = -1;
    char seq[32];
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            size_t i = (size_t)y * bw + x;
            Cell *n = &cur_b[i], *o = &prev_b[i];
            if (n->cp == o->cp && n->at == o->at && n->fg == o->fg) continue;
            if (y != lasty || x != lastx) {
                int l = snprintf(seq, sizeof seq, "\x1b[%d;%dH", y + 1, x + 1);
                term_write(seq, (size_t)l);
            }
            if (n->at != lastat || n->fg != lastfg) {
                int l = n->fg
                    ? snprintf(seq, sizeof seq, "\x1b[%s;%sm", sgr[n->at], kind_fg[n->fg - 1])
                    : snprintf(seq, sizeof seq, "\x1b[%sm", sgr[n->at]);
                term_write(seq, (size_t)l);
                lastat = n->at;
                lastfg = n->fg;
            }
            char u[4];
            uint32_t cp = n->cp;
            int l;
            if (cp < 0x80) { u[0] = (char)cp; l = 1; }
            else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); l = 2; }
            else { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); l = 3; }
            term_write(u, (size_t)l);
            *o = *n;
            lasty = y;
            lastx = x + 1;
        }
    term_write("\x1b[0m", 4);
}

void ui_draw(App *a) {
    frame_size(a->rows, a->cols);
    if (!cur_b) return;
    for (size_t i = 0; i < (size_t)bw * bh; i++) {
        cur_b[i].cp = ' ';
        cur_b[i].at = AT_NORM;
    }
    if (a->rows < 10 || a->cols < 30) {
        puts_at(0, 0, "terminal too small", AT_ERR, bw);
        flush_frame();
        return;
    }
    const Doc *d = &a->docs[a->cur];

    /* menu bar */
    fill(0, 0, bw, ' ', AT_BAR);
    for (int i = 0; menubar[i].title; i++) {
        int at = a->mb == i || a->mhov == i ? AT_MENUSEL : AT_BAR;   /* hover looks like an open menu */
        fill(0, ui_menu_x(i), ui_menu_x(i) + ui_menu_w(i), ' ', at);
        puts_at(0, ui_menu_x(i) + 1, menubar[i].title, at, ui_menu_w(i) - 2);
    }
    char title[160];
    snprintf(title, sizeof title, "%s%s", d->name, d->dirty ? " *" : "");
    int tl = u8len(title);
    if (tl < bw - 40) puts_at(0, bw - tl - 1, title, AT_BAR, tl);

    /* formula bar */
    char cn[8], ref[32];
    col_name(d->cc, cn);
    snprintf(ref, sizeof ref, "%s%d", cn, d->cr + 1);
    puts_at(1, 0, ref, AT_BOLD, 8);
    puts_at(1, 9, "fx", AT_NORM, 3);
    if (a->mode == MODE_EDIT) puts_at(1, 12, a->ebuf, AT_NORM, bw - 12);
    else {
        const char *raw = sheet_get(d->g, d->cr, d->cc);
        if (raw) puts_at(1, 12, raw, AT_NORM, bw - 12);
    }

    draw_grid(a);

    /* sheet tabs */
    int ty = bh - 3, tx = 0;
    fill(ty, 0, bw, ' ', AT_BAR);
    for (int i = 0; i < a->ndocs && tx < bw; i++) {
        char lab[80];
        snprintf(lab, sizeof lab, " %s%s ", a->docs[i].name, a->docs[i].dirty ? "*" : "");
        int w = u8len(lab);
        a->tabx[i] = tx;
        a->tabw[i] = w;
        puts_at(ty, tx, lab, i == a->cur ? AT_TABSEL : AT_TAB, bw - tx);
        tx += w + 1;
    }

    /* bottom bar, three rows: tabs (bh-3), status and prompt (bh-2),
       current cell and statistics (bh-1) */
    int sy = bh - 2, iy = bh - 1;
    fill(sy, 0, bw, ' ', AT_BAR);
    fill(iy, 0, bw, ' ', AT_BAR);
    const char *mode = a->npanels ? "Menu"
                     : a->mode == MODE_EDIT ? (a->vim ? "Insert" : "Edit")
                     : a->mode == MODE_PROMPT ? "Prompt"
                     : a->mode == MODE_LIST ? "List" : a->mode == MODE_RESIZE ? "Resize"
                     : a->mode == MODE_DLG ? (a->acts ? "Options" : "Graph")
                     : a->vim ? (a->vim_vis ? "Visual" : "Normal") : "Ready";
    puts_at(sy, 1, mode, AT_BAR, 8);
    if (a->mode == MODE_PROMPT) {
        int l = puts_at(sy, 10, a->msg, AT_BAR, bw - 12);
        puts_at(sy, 10 + l, a->ebuf, AT_BAR, bw - 12 - l);
    } else {
        /* an open pop-up explains its highlighted item here */
        const char *shown = a->msg;
        if (a->npanels) {
            const Panel *p = &a->pan[a->npanels - 1];
            if (p->sel >= 0 && p->items[p->sel].desc) shown = p->items[p->sel].desc;
        }
        puts_at(sy, 10, shown, a->warn && shown == a->msg ? AT_WARN : AT_BAR, bw - 12);
    }
    int r1, c1, r2, c2;
    doc_sel(d, &r1, &c1, &r2, &c2);
    char st[120] = "";
    if (a->mode == MODE_NORMAL && (d->next || r1 != r2 || c1 != c2)) {
        double sum;
        int nums, count;
        stats(d, &sum, &nums, &count);
        int k = d->next ? snprintf(st, sizeof st, "%d areas  ", d->next + 1) : 0;
        snprintf(st + k, sizeof st - k, "Average: %.6g  Count: %d  Sum: %.6g",
                 nums ? sum / nums : 0.0, count, sum);
    }
    int stl = (int)strlen(st);
    if (stl) puts_at(iy, bw - stl - 2, st, AT_BAR, stl);
    const char *raw = sheet_get(d->g, d->cr, d->cc);
    char info[300];
    snprintf(info, sizeof info, "%s  %s", ref, raw ? raw : "");
    puts_at(iy, 1, info, AT_BAR, bw - stl - 4);

    /* menus, lists, then the cursor */
    for (int i = 0; i < a->npanels; i++) draw_panel(a, &a->pan[i]);
    if (a->mode == MODE_LIST) draw_list(a);
    if (a->mode == MODE_DLG) draw_dlg(a);

    flush_frame();

    char seq[64];
    View v;
    view_build(a, &v);
    if (a->mode == MODE_EDIT && a->npanels == 0) {
        int ci = -1, cj = -1;
        for (int i = 0; i < v.nr; i++) if (v.r[i] == d->cr) ci = i;
        for (int j = 0; j < v.nc; j++) if (v.c[j] == d->cc) cj = j;
        if (ci >= 0 && cj >= 0) {
            int w = v.w[cj], skip = a->epos > w - 1 ? a->epos - (w - 1) : 0;
            int l = snprintf(seq, sizeof seq, "\x1b[%d;%dH\x1b[?25h",
                             v.y[ci] + (v.h[ci] - 1) / 2 + 1, v.x[cj] + 2 + (a->epos - skip));
            term_write(seq, (size_t)l);
        } else term_write("\x1b[?25l", 6);
    } else if (a->mode == MODE_PROMPT) {
        int l = snprintf(seq, sizeof seq, "\x1b[%d;%dH\x1b[?25h", bh - 1,
                         10 + u8len(a->msg) + a->epos + 1);
        term_write(seq, (size_t)l);
    } else {
        term_write("\x1b[?25l", 6);
    }
}
