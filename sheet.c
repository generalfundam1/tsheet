#include "tsheet.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- storage ---------- */

/* ---------- storage, merges and the undo journal ----------
   Writes to a merged cell go to its top-left cell. The *_put* functions and
   sheet_get_raw address exact cells; undo uses them so it never redirects.
   The journal records, for each change, the value it replaces (kind 0 raw,
   1 format, 2 the list of merged blocks). */

static Txn *journal = NULL;

static const char *raw_at(const Sheet *s, int r, int c) {
    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS || !s->rows[r]) return NULL;
    return s->rows[r][c];
}

static const char *fmt_at(const Sheet *s, int r, int c) {
    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS || !s->fmts[r]) return NULL;
    return s->fmts[r][c];
}

static char *merges_text(const Sheet *s) {
    char *out = malloc((size_t)s->nmg * 32 + 2);
    if (!out) return NULL;
    out[0] = '\0';
    size_t l = 0;
    for (int i = 0; i < s->nmg; i++) {
        const Merge *m = &s->mg[i];
        l += (size_t)snprintf(out + l, 32, "%d %d %d %d;", m->r1, m->c1, m->r2, m->c2);
    }
    return out;
}

static void merges_from_text(Sheet *s, const char *t) {
    s->nmg = 0;
    int a, b, c, d, n;
    const char *p = t ? t : "";
    while (sscanf(p, "%d %d %d %d;%n", &a, &b, &c, &d, &n) == 4 && s->nmg < MAX_MERGE) {
        s->mg[s->nmg].r1 = a;
        s->mg[s->nmg].c1 = b;
        s->mg[s->nmg].r2 = c;
        s->mg[s->nmg].c2 = d;
        s->nmg++;
        p += n;
    }
}

static void jrec(Sheet *s, int r, int c, int kind) {
    if (!journal) return;
    if (journal->n == journal->cap) {
        int cap = journal->cap ? journal->cap * 2 : 64;
        JEntry *p = realloc(journal->e, (size_t)cap * sizeof *p);
        if (!p) return;
        journal->e = p;
        journal->cap = cap;
    }
    char *ser = NULL;
    const char *cur;
    if (kind == 2) { ser = merges_text(s); cur = ser; }
    else if (kind == 1) cur = fmt_at(s, r, c);
    else cur = raw_at(s, r, c);
    JEntry *en = &journal->e[journal->n++];
    en->g = s;
    en->r = r;
    en->c = c;
    en->fmt = kind;
    en->old = cur ? strdup(cur) : NULL;
    free(ser);
}

void txn_begin(Txn *t) { journal = t; }
void txn_end(void) { journal = NULL; }

Txn *txn_pause(void) {
    Txn *j = journal;
    journal = NULL;
    return j;
}

void txn_resume(Txn *j) { journal = j; }

void txn_free(Txn *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) free(t->e[i].old);
    free(t->e);
    free(t);
}

void txn_undo(Txn *t, Txn *inv) {
    Txn *saved = txn_pause();
    for (int i = t->n - 1; i >= 0; i--) {
        JEntry *en = &t->e[i];
        journal = inv;                       /* remember what is there now */
        jrec(en->g, en->r, en->c, en->fmt);
        journal = NULL;
        if (en->fmt == 2) merges_from_text(en->g, en->old);
        else if (en->fmt == 1) sheet_put_fmt(en->g, en->r, en->c, en->old);
        else sheet_put_raw(en->g, en->r, en->c, en->old);
    }
    journal = saved;
}

Sheet *sheet_new(void) { return calloc(1, sizeof(Sheet)); }

void sheet_free(Sheet *s) {
    if (!s) return;
    for (int r = 0; r < MAX_ROWS; r++) {
        if (!s->rows[r]) continue;
        for (int c = 0; c < MAX_COLS; c++) free(s->rows[r][c]);
        free(s->rows[r]);
    }
    for (int r = 0; r < MAX_ROWS; r++) {
        if (!s->fmts[r]) continue;
        for (int c = 0; c < MAX_COLS; c++) free(s->fmts[r][c]);
        free(s->fmts[r]);
    }
    free(s);
}

const Merge *sheet_merge_of(const Sheet *s, int r, int c) {
    for (int i = 0; i < s->nmg; i++) {
        const Merge *m = &s->mg[i];
        if (r >= m->r1 && r <= m->r2 && c >= m->c1 && c <= m->c2) return m;
    }
    return NULL;
}

static void to_anchor(const Sheet *s, int *r, int *c) {
    const Merge *m = sheet_merge_of(s, *r, *c);
    if (m) { *r = m->r1; *c = m->c1; }
}

const char *sheet_get_raw(const Sheet *s, int r, int c) { return raw_at(s, r, c); }

const char *sheet_get(const Sheet *s, int r, int c) {
    to_anchor(s, &r, &c);
    return raw_at(s, r, c);
}

void sheet_put_raw(Sheet *s, int r, int c, const char *raw) {
    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) return;
    jrec(s, r, c, 0);
    if (!raw || !*raw) {
        if (s->rows[r]) { free(s->rows[r][c]); s->rows[r][c] = NULL; }
        return;
    }
    if (!s->rows[r]) {
        s->rows[r] = calloc(MAX_COLS, sizeof(char *));
        if (!s->rows[r]) return;
    }
    char *dup = strdup(raw);
    if (!dup) return;
    free(s->rows[r][c]);
    s->rows[r][c] = dup;
    if (r > s->max_r) s->max_r = r;
    if (c > s->max_c) s->max_c = c;
}

void sheet_set(Sheet *s, int r, int c, const char *raw) {
    to_anchor(s, &r, &c);
    sheet_put_raw(s, r, c, raw);
}

static void merges_remove(Sheet *s, int r1, int c1, int r2, int c2) {
    int w = 0;
    for (int i = 0; i < s->nmg; i++) {
        const Merge *m = &s->mg[i];
        if (m->r1 <= r2 && m->r2 >= r1 && m->c1 <= c2 && m->c2 >= c1) continue;
        s->mg[w++] = *m;
    }
    s->nmg = w;
}

/* Merges a rectangle. Overlapping blocks are unmerged first, and the cells other
   than the top-left one are emptied, as in a spreadsheet. */
int sheet_merge_add(Sheet *s, int r1, int c1, int r2, int c2) {
    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
    if (r1 == r2 && c1 == c2) return -1;
    if (r1 < 0 || c1 < 0 || r2 >= MAX_ROWS || c2 >= MAX_COLS) return -1;
    jrec(s, 0, 0, 2);
    merges_remove(s, r1, c1, r2, c2);
    for (int r = r1; r <= r2; r++)
        for (int c = c1; c <= c2; c++)
            if (r != r1 || c != c1) sheet_put_raw(s, r, c, NULL);
    if (s->nmg >= MAX_MERGE) return -1;
    s->mg[s->nmg].r1 = r1;
    s->mg[s->nmg].c1 = c1;
    s->mg[s->nmg].r2 = r2;
    s->mg[s->nmg].c2 = c2;
    s->nmg++;
    return 0;
}

void sheet_unmerge(Sheet *s, int r1, int c1, int r2, int c2) {
    if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
    if (c1 > c2) { int t = c1; c1 = c2; c2 = t; }
    jrec(s, 0, 0, 2);
    merges_remove(s, r1, c1, r2, c2);
}

void col_name(int c, char *buf) {
    char tmp[8];
    int i = 0;
    c++;                                  /* 1-based bijective base-26 */
    while (c > 0) {
        tmp[i++] = 'A' + (c - 1) % 26;
        c = (c - 1) / 26;
    }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

/* ---------- references ---------- */

typedef struct { int r, c, ar, ac; } Ref;   /* ar/ac: row/column is $-absolute */

/* Parses A1, $A$1, AB12 starting at p. On success *end points past the ref. */
static int parse_ref_at(const char *p, const char **end, Ref *ref) {
    const char *q = p;
    long col = 0, row = 0;
    int ac = 0, ar = 0, n = 0;
    if (*q == '$') { ac = 1; q++; }
    if (!isalpha((unsigned char)*q)) return 0;
    while (isalpha((unsigned char)*q)) {
        if (++n > 3) return 0;
        col = col * 26 + (toupper((unsigned char)*q) - 'A' + 1);
        q++;
    }
    if (*q == '$') { ar = 1; q++; }
    if (!isdigit((unsigned char)*q)) return 0;
    while (isdigit((unsigned char)*q)) {
        row = row * 10 + (*q - '0');
        if (row > MAX_ROWS) return 0;
        q++;
    }
    if (row < 1 || col < 1 || col > MAX_COLS) return 0;
    ref->r = (int)row - 1;
    ref->c = (int)col - 1;
    ref->ar = ar;
    ref->ac = ac;
    *end = q;
    return 1;
}

/* ---------- aggregate helpers ---------- */

typedef struct { double sum, mn, mx; int n; } Agg;

static void agg_add(Agg *A, double v) {
    if (A->n == 0 || v < A->mn) A->mn = v;
    if (A->n == 0 || v > A->mx) A->mx = v;
    A->sum += v;
    A->n++;
}

static double agg_result(int kind, const Agg *A) {
    switch (kind) {
    case 0: return A->sum;
    case 1: return A->n ? A->sum / A->n : 0;
    case 2: return A->n ? A->mn : 0;
    default: return A->n ? A->mx : 0;
    }
}

/* ---------- formula evaluator (recursive descent) ---------- */

typedef struct {
    const Sheet *s;
    const char *p;
    int depth;
    int err;
} Parser;

static double parse_expr(Parser *P);
static int formula_eval(const Sheet *s, const char *expr, int depth, double *out);

static void skip_ws(Parser *P) { while (*P->p == ' ' || *P->p == '\t') P->p++; }

int cell_num(const Sheet *s, int r, int c, int depth, double *out) {
    const char *raw = sheet_get(s, r, c);
    *out = 0;
    if (!raw || !*raw) return 3;
    if (raw[0] == '=') {
        if (formula_eval(s, raw + 1, depth + 1, out) < 0) return 2;
        return 0;
    }
    if (!(isdigit((unsigned char)raw[0]) || raw[0] == '-' || raw[0] == '+' || raw[0] == '.'))
        return 1;
    char *end;
    double v = strtod(raw, &end);
    if (end == raw || *end) return 1;
    *out = v;
    return 0;
}

static double cell_value(Parser *P, int r, int c) {
    double v = 0;
    int st = cell_num(P->s, r, c, P->depth, &v);
    if (st == 1 || st == 2) P->err = 1;     /* text in arithmetic -> #VALUE! */
    return v;
}

static double parse_func(Parser *P, size_t len) {
    char fn[8];
    int kind;
    if (len >= sizeof fn) { P->err = 1; return 0; }
    for (size_t i = 0; i < len; i++) fn[i] = (char)toupper((unsigned char)P->p[i]);
    fn[len] = '\0';
    if (!strcmp(fn, "SUM")) kind = 0;
    else if (!strcmp(fn, "AVERAGE")) kind = 1;
    else if (!strcmp(fn, "MIN")) kind = 2;
    else if (!strcmp(fn, "MAX")) kind = 3;
    else { P->err = 1; return 0; }

    P->p += len + 1;                        /* skip name and '(' */
    Agg A = {0, 0, 0, 0};
    skip_ws(P);
    if (*P->p != ')') {
        for (;;) {
            Ref ra, rb;
            const char *e;
            if (parse_ref_at(P->p, &e, &ra) && *e == ':') {
                P->p = e + 1;
                if (!parse_ref_at(P->p, &e, &rb)) { P->err = 1; return 0; }
                P->p = e;
                int r1 = ra.r < rb.r ? ra.r : rb.r, r2 = ra.r < rb.r ? rb.r : ra.r;
                int c1 = ra.c < rb.c ? ra.c : rb.c, c2 = ra.c < rb.c ? rb.c : ra.c;
                for (int r = r1; r <= r2; r++)
                    for (int c = c1; c <= c2; c++) {
                        double v;
                        int st = cell_num(P->s, r, c, P->depth, &v);
                        if (st == 2) { P->err = 1; return 0; }
                        if (st == 0) agg_add(&A, v);   /* text and blanks skipped */
                    }
            } else {
                double v = parse_expr(P);
                if (P->err) return 0;
                agg_add(&A, v);
            }
            skip_ws(P);
            if (*P->p == ',') { P->p++; continue; }
            break;
        }
    }
    skip_ws(P);
    if (*P->p != ')') { P->err = 1; return 0; }
    P->p++;
    return agg_result(kind, &A);
}

static double parse_factor(Parser *P) {
    skip_ws(P);
    if (P->err) return 0;
    int c = (unsigned char)*P->p;
    if (c == '+') { P->p++; return parse_factor(P); }
    if (c == '-') { P->p++; return -parse_factor(P); }
    if (c == '(') {
        P->p++;
        double v = parse_expr(P);
        skip_ws(P);
        if (*P->p == ')') P->p++; else P->err = 1;
        return v;
    }
    if (isdigit(c) || c == '.') {
        char *end;
        double v = strtod(P->p, &end);
        if (end == P->p) { P->err = 1; return 0; }
        P->p = end;
        return v;
    }
    if (isalpha(c) || c == '$') {
        const char *start = P->p, *q = start;
        while (isalnum((unsigned char)*q) || *q == '_' || *q == '$') q++;
        if (*q == '(') return parse_func(P, (size_t)(q - start));
        Ref ref = {0, 0, 0, 0};
        const char *e;
        if (!parse_ref_at(start, &e, &ref) || e != q) { P->err = 1; return 0; }
        P->p = e;
        return cell_value(P, ref.r, ref.c);
    }
    P->err = 1;
    return 0;
}

static double parse_term(Parser *P) {
    double v = parse_factor(P);
    for (;;) {
        skip_ws(P);
        if (*P->p == '*') { P->p++; v *= parse_factor(P); }
        else if (*P->p == '/') {
            P->p++;
            double d = parse_factor(P);
            if (d == 0) { P->err = 1; return 0; }
            v /= d;
        } else return v;
    }
}

static double parse_expr(Parser *P) {
    double v = parse_term(P);
    for (;;) {
        skip_ws(P);
        if (*P->p == '+') { P->p++; v += parse_term(P); }
        else if (*P->p == '-') { P->p++; v -= parse_term(P); }
        else return v;
    }
}

static int formula_eval(const Sheet *s, const char *expr, int depth, double *out) {
    if (depth > 64) return -1;                 /* circular reference guard */
    Parser P = {s, expr, depth, 0};
    double v = parse_expr(&P);
    skip_ws(&P);
    if (P.err || *P.p) return -1;
    *out = v;
    return 0;
}

/* ---------- display ---------- */

/* ---- per-cell format: optional align char ('<' '>' '^') then a body:
        "%..."  strftime pattern for unix-time values (UTC)
        ".N"    N decimal places
        ""      default (unix time / as typed) ---- */

/* ---- display formats ----
   A format is: optional 'b' (bold), optional 'h' (highlight), optional align
   char ('<' '>' '^'), then a body: "%..." strftime pattern for unix-time values
   (UTC), ".N" for N decimal places, or empty. */

void fmt_parse(const char *f, int *align, int *bold, int *hl, const char **body) {
    *align = 0;
    *bold = 0;
    *hl = 0;
    *body = "";
    if (!f) return;
    const char *p = f;
    while (*p == 'b' || *p == 'h') {
        if (*p == 'b') *bold = 1;
        else *hl = 1;
        p++;
    }
    if (*p == '<' || *p == '>' || *p == '^') *align = *p++;
    *body = p;
}

void fmt_build(char *out, size_t n, int align, int bold, int hl, const char *body) {
    size_t k = 0;
    if (bold && k + 1 < n) out[k++] = 'b';
    if (hl && k + 1 < n) out[k++] = 'h';
    if (align && k + 1 < n) out[k++] = (char)align;
    if (k < n) snprintf(out + k, n - k, "%s", body ? body : "");
    else out[n - 1] = '\0';
}

/* ---------- number formats ---------- */

#define NUM_SEP '\x1f'

void num_parse(const char *body, NumSpec *ns) {
    ns->style = '-';
    ns->dec = -1;
    ns->pre[0] = ns->suf[0] = '\0';
    if (!body) return;
    if (body[0] == '.' && body[1] >= '0' && body[1] <= '9') { ns->dec = body[1] - '0'; return; }
    if (body[0] != 'n' || !body[1] || !body[2]) return;
    ns->style = (unsigned char)body[1];
    ns->dec = body[2] >= '0' && body[2] <= '9' ? body[2] - '0' : -1;
    const char *p = body + 3;
    if (*p == NUM_SEP) p++;
    size_t k = 0;
    while (*p && *p != NUM_SEP) { if (k + 1 < sizeof ns->pre) ns->pre[k++] = *p; p++; }
    ns->pre[k] = '\0';
    if (*p == NUM_SEP) p++;
    k = 0;
    while (*p && *p != NUM_SEP) { if (k + 1 < sizeof ns->suf) ns->suf[k++] = *p; p++; }
    ns->suf[k] = '\0';
}

void num_build(char *out, size_t n, const NumSpec *ns) {
    if (ns->style == '-' && !ns->pre[0] && !ns->suf[0]) {
        if (ns->dec >= 0) snprintf(out, n, ".%d", ns->dec);
        else if (n) out[0] = '\0';
        return;
    }
    snprintf(out, n, "n%c%c%c%s%c%s%c", ns->style, ns->dec >= 0 ? '0' + ns->dec : '-', NUM_SEP,
             ns->pre, NUM_SEP, ns->suf, NUM_SEP);
}

/* Fixed-point text of m >= 0: dec >= 0 gives that many decimals, otherwise as
   many as needed (up to 9), trailing zeros dropped. Plain arithmetic only, no libm. */
static void fixed_text(char *out, size_t n, double m, int dec) {
    if (dec >= 0) { snprintf(out, n, "%.*f", dec, m); return; }
    snprintf(out, n, "%.9f", m);
    size_t l = strlen(out);
    if (strchr(out, '.')) {
        while (l > 0 && out[l - 1] == '0') out[--l] = '\0';
        if (l > 0 && out[l - 1] == '.') out[--l] = '\0';
    }
}

/* Inserts the separator every three digits of the integer part of a number text. */
static void group_text(char *out, size_t n, const char *num, char sep) {
    size_t ilen = strcspn(num, ".");
    size_t k = 0;
    for (size_t i = 0; i < ilen && k + 2 < n; i++) {
        if (i && (ilen - i) % 3 == 0) out[k++] = sep;
        out[k++] = num[i];
    }
    snprintf(out + k, n - k, "%s", num + ilen);
}

/* A number in one of the number styles, whole or decimal:
   ',' and ' ' group the digits in threes; 'e' writes mantissa*10^exp; 'g' writes
   the mantissa with an SI prefix (k M G T P E). dec >= 0 fixes the decimals. */
static void style_text(char *out, size_t n, int style, double v, int dec) {
    static const char *si = "kMGTPE";
    double av = v < 0 ? -v : v, m = av;
    int exp10 = 0, k = 0;
    if (style == 'e') {
        if (av != 0) {
            while (m >= 10) { m /= 10; exp10++; }
            while (m < 1) { m *= 10; exp10--; }
        }
    } else if (style == 'g') {
        while (m >= 1000 && k < 6) { m /= 1000; k++; }
    }
    char num[128], grp[256];
    fixed_text(num, sizeof num, m, dec);
    if (style == ',' || style == ' ') group_text(grp, sizeof grp, num, (char)style);
    else snprintf(grp, sizeof grp, "%s", num);
    const char *sign = v < 0 ? "-" : "";
    if (style == 'e' && exp10 != 0) snprintf(out, n, "%s%s*10^%d", sign, grp, exp10);
    else if (style == 'g' && k > 0) snprintf(out, n, "%s%s %c", sign, grp, si[k - 1]);
    else snprintf(out, n, "%s%s", sign, grp);
}

/* ---- LaTeX to UTF-8 for prefixes and suffixes. UTF-8 input passes through. ---- */

static const struct { const char *name; const char *u8; } ltx_sym[] = {
    {"circ", "\u00b0"}, {"degree", "\u00b0"}, {"pm", "\u00b1"}, {"mp", "\u2213"},
    {"times", "\u00d7"}, {"cdot", "\u00b7"}, {"div", "\u00f7"},
    {"mu", "\u03bc"}, {"Omega", "\u03a9"}, {"omega", "\u03c9"}, {"ohm", "\u03a9"},
    {"alpha", "\u03b1"}, {"beta", "\u03b2"}, {"gamma", "\u03b3"}, {"delta", "\u03b4"},
    {"Delta", "\u0394"}, {"lambda", "\u03bb"}, {"sigma", "\u03c3"}, {"Sigma", "\u03a3"},
    {"theta", "\u03b8"}, {"epsilon", "\u03b5"}, {"pi", "\u03c0"},
    {"infty", "\u221e"}, {"approx", "\u2248"}, {"neq", "\u2260"}, {"le", "\u2264"}, {"ge", "\u2265"},
    {"to", "\u2192"}, {"rightarrow", "\u2192"}, {"leftarrow", "\u2190"},
    {"euro", "\u20ac"}, {"EUR", "\u20ac"}, {"yen", "\u00a5"}, {"pounds", "\u00a3"}, {"cent", "\u00a2"},
    {"permil", "\u2030"}, {"permille", "\u2030"}, {"ldots", "\u2026"}, {"dots", "\u2026"},
    {"AA", "\u00c5"}, {"partial", "\u2202"}, {"sum", "\u2211"}, {"checkmark", "\u2713"},
};

/* commands that only change the font: their name is dropped, the text stays */
static const char *ltx_plain[] = {"text", "textrm", "mathrm", "mathbf", "textbf", "mathit",
                                  "textit", "mathsf", "texttt", "operatorname", "mbox", NULL};

static const char *sup_of(char c) {
    switch (c) {
    case '0': return "\u2070"; case '1': return "\u00b9"; case '2': return "\u00b2";
    case '3': return "\u00b3"; case '4': return "\u2074"; case '5': return "\u2075";
    case '6': return "\u2076"; case '7': return "\u2077"; case '8': return "\u2078";
    case '9': return "\u2079"; case '-': return "\u207b"; case '+': return "\u207a";
    case 'n': return "\u207f";
    default: return NULL;
    }
}

static const char *sub_of(char c) {
    switch (c) {
    case '0': return "\u2080"; case '1': return "\u2081"; case '2': return "\u2082";
    case '3': return "\u2083"; case '4': return "\u2084"; case '5': return "\u2085";
    case '6': return "\u2086"; case '7': return "\u2087"; case '8': return "\u2088";
    case '9': return "\u2089"; case '-': return "\u208b"; case '+': return "\u208a";
    default: return NULL;
    }
}

static void lo_put(char *out, size_t n, size_t *k, const char *s) {
    while (*s && *k + 1 < n) out[(*k)++] = *s++;
    out[*k] = '\0';
}

void latex_to_utf8(const char *in, char *out, size_t n) {
    size_t k = 0;
    if (n) out[0] = '\0';
    /* $ is a math delimiter only in pairs ($\mu$m); a lone $ is a currency sign */
    int dollars = 0;
    for (const char *q = in; *q; q++) {
        if (*q == '\\' && q[1]) q++;
        else if (*q == '$') dollars++;
    }
    for (const char *p = in; *p && n; ) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            if (strchr("$%&#_{}", *p)) { char t[2] = {*p, '\0'}; lo_put(out, n, &k, t); p++; continue; }
            if (strchr(",;: ", *p)) { lo_put(out, n, &k, " "); p++; continue; }
            if (*p == '!') { p++; continue; }
            const char *s = p;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) p++;
            size_t len = (size_t)(p - s);
            if (!len) { p++; continue; }
            char name[32];
            snprintf(name, sizeof name, "%.*s", (int)(len < 31 ? len : 31), s);
            int plain = 0;
            for (int i = 0; ltx_plain[i]; i++) if (!strcmp(name, ltx_plain[i])) plain = 1;
            if (plain) continue;
            if (!strcmp(name, "quad") || !strcmp(name, "qquad")) { lo_put(out, n, &k, "  "); continue; }
            const char *u = NULL;
            for (size_t i = 0; i < sizeof ltx_sym / sizeof ltx_sym[0]; i++)
                if (!strcmp(name, ltx_sym[i].name)) u = ltx_sym[i].u8;
            lo_put(out, n, &k, u ? u : name);
            continue;
        }
        if (*p == '^' || *p == '_') {
            int sup = *p == '^';
            p++;
            char body[32] = "";
            size_t bl = 0;
            if (*p == '{') {
                p++;
                while (*p && *p != '}' && bl + 1 < sizeof body) body[bl++] = *p++;
                if (*p == '}') p++;
            } else if (*p) { body[bl++] = *p++; }
            body[bl] = '\0';
            size_t start = k;
            int ok = 1;
            for (size_t i = 0; i < bl && ok; i++) {
                const char *m = sup ? sup_of(body[i]) : sub_of(body[i]);
                if (m) lo_put(out, n, &k, m); else ok = 0;
            }
            if (!ok) { k = start; lo_put(out, n, &k, sup ? "^" : "_"); lo_put(out, n, &k, body); }
            continue;
        }
        if ((*p == '$' && dollars >= 2) || *p == '{' || *p == '}') { p++; continue; }
        char t[2] = {*p, '\0'};
        lo_put(out, n, &k, t);
        p++;
    }
}

/* One number as text. Whole numbers take the integer styles; other numbers keep
   their decimals (or as typed). The prefix and suffix wrap the number. */
static void numeric_text(const NumSpec *ns, const char *raw, int formula, double v,
                         char *out, size_t n) {
    char core[512];
    if (ns->style == ',' || ns->style == ' ' || ns->style == 'e' || ns->style == 'g')
        style_text(core, sizeof core, ns->style, v, ns->dec);
    else if (ns->dec >= 0) snprintf(core, sizeof core, "%.*f", ns->dec, v);
    else if (formula) snprintf(core, sizeof core, "%.10g", v);
    else snprintf(core, sizeof core, "%s", raw);
    char pre[200], suf[200];
    latex_to_utf8(ns->pre, pre, sizeof pre);
    latex_to_utf8(ns->suf, suf, sizeof suf);
    /* "1.234 k" with suffix "kg": the suffix gets its own word, not "kkg" */
    size_t cl = strlen(core);
    int scaled = ns->style == 'g' && cl >= 3 && core[cl - 2] == ' ' && strchr("kMGTPE", core[cl - 1]);
    if (scaled && suf[0] && suf[0] != ' ') snprintf(out, n, "%s%s %s", pre, core, suf);
    else snprintf(out, n, "%s%s%s", pre, core, suf);
}

void sheet_style(const Sheet *s, int r, int c, int *bold, int *hl) {
    int align;
    const char *body;
    fmt_parse(sheet_fmt(s, r, c), &align, bold, hl, &body);
}

const char *sheet_fmt(const Sheet *s, int r, int c) {
    to_anchor(s, &r, &c);
    return fmt_at(s, r, c);
}

void sheet_put_fmt(Sheet *s, int r, int c, const char *fmt) {
    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) return;
    jrec(s, r, c, 1);
    if (!fmt || !*fmt) {
        if (s->fmts[r]) { free(s->fmts[r][c]); s->fmts[r][c] = NULL; }
        return;
    }
    if (!s->fmts[r]) {
        s->fmts[r] = calloc(MAX_COLS, sizeof(char *));
        if (!s->fmts[r]) return;
    }
    char *dup = strdup(fmt);
    if (!dup) return;
    free(s->fmts[r][c]);
    s->fmts[r][c] = dup;
    if (r > s->max_r) s->max_r = r;
    if (c > s->max_c) s->max_c = c;
}

void sheet_fmt_set(Sheet *s, int r, int c, const char *fmt) {
    to_anchor(s, &r, &c);
    sheet_put_fmt(s, r, c, fmt);
}

/* Parses a typed date/time into unix seconds (UTC).
   Accepts Y-M-D, Y/M/D, M/D/Y (US), optional time H:M[:S] after a space or 'T',
   or a time alone (kind 3: seconds since midnight). kind: 1 date, 2 date+time, 3 time. */
static int days_in_month(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

int parse_datetime(const char *in, long long *out, int *kind) {
    const char *p = in;
    int a = 0, b = 0, c = 0, n = 0, have_date = 0;
    while (*p == ' ') p++;
    if (sscanf(p, "%d-%d-%d%n", &a, &b, &c, &n) == 3 || sscanf(p, "%d/%d/%d%n", &a, &b, &c, &n) == 3) {
        have_date = 1;
        p += n;
    }
    int y = 0, mo = 0, d = 0;
    if (have_date) {
        if (a >= 1000) { y = a; mo = b; d = c; }                                      /* Y-M-D or Y/M/D */
        else if (c >= 1000) { mo = a; d = b; y = c; }                                 /* M/D/Y */
        else return -1;
        if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo)) return -1;
    }
    const char *q = p;
    while (*q == ' ' || *q == 'T') q++;
    int hh = 0, mm = 0, ss = 0, have_time = 0, m = 0;
    if (*q) {
        if (sscanf(q, "%d:%d%n", &hh, &mm, &m) != 2) return -1;
        q += m;
        have_time = 1;
        if (*q == ':') {
            if (sscanf(q, ":%d%n", &ss, &m) != 1) return -1;
            q += m;
        }
        while (*q == ' ') q++;
        if (*q) return -1;
        if (hh > 23 || mm > 59 || ss > 59) return -1;
    }
    if (!have_date && !have_time) return -1;
    if (!have_date) {
        *out = hh * 3600LL + mm * 60LL + ss;
        *kind = 3;
        return 0;
    }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    *out = (long long)timegm(&tm);
    *kind = have_time ? 2 : 1;
    return 0;
}

/* Kind of value in a cell, used to pick its colour. */
int cell_kind(const Sheet *s, int r, int c) {
    const char *raw = sheet_get(s, r, c);
    if (!raw || !*raw) return -1;
    double v = 0;
    int st = cell_num(s, r, c, 0, &v);
    if (raw[0] == '=') return st == 0 ? KIND_FORMULA : KIND_ERROR;
    if (st != 0) return KIND_TEXT;
    int align, bold, hl;
    const char *body;
    fmt_parse(sheet_fmt(s, r, c), &align, &bold, &hl, &body);
    return body[0] == '%' ? KIND_TIME : KIND_NUMBER;
}

/* Display text of a cell. align: 0 left, 1 right, 2 centre. */
void cell_display(const Sheet *s, int r, int c, char *out, size_t n, int *align) {
    const char *raw = sheet_get(s, r, c);
    int fa, bold, hl;
    const char *body;
    fmt_parse(sheet_fmt(s, r, c), &fa, &bold, &hl, &body);
    out[0] = '\0';
    *align = 0;
    if (!raw) return;
    double v = 0;
    int st = cell_num(s, r, c, 0, &v);
    if (st == 0) {
        *align = 1;                           /* numbers right-align by default */
        if (body[0] == '%') {
            time_t t = (time_t)(v < 0 ? v - 0.5 : v + 0.5);
            struct tm tm;
            gmtime_r(&t, &tm);
            if (!strftime(out, n, body, &tm)) out[0] = '\0';
        } else {                              /* decimals, integer style, prefix and suffix */
            NumSpec ns;
            num_parse(body, &ns);
            numeric_text(&ns, raw, raw[0] == '=', v, out, n);
        }
    } else if (raw[0] == '=') {
        snprintf(out, n, "#VALUE!");
    } else {
        snprintf(out, n, "%s", raw);
    }
    if (fa == '<') *align = 0;
    else if (fa == '>') *align = 1;
    else if (fa == '^') *align = 2;
}

/* ---------- text to numbers, with prefix and suffix (paste) ---------- */

/* A prefix is symbols and signs ($, €, (), or a currency code (USD, US$, EUR). */
static int prefix_ok(const char *p, size_t n) {
    static const char *codes[] = {"US", "USD", "EUR", "GBP", "CHF", "CAD", "AUD", "JPY", "CNY", "INR", "C", "A", NULL};
    char letters[8];
    size_t ln = 0;
    int other_ok = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (isalpha(ch)) { if (ln + 1 >= sizeof letters) return 0; letters[ln++] = (char)ch; }
        else if (ch == '$' || ch == ' ' || ch >= 0x80) { if (ch == '$') other_ok = 1; }
        else other_ok = 0;
    }
    if (!other_ok) return 0;
    if (!ln) return 1;
    letters[ln] = '\0';
    for (int k = 0; codes[k]; k++) {
        if (strcmp(letters, codes[k])) continue;
        /* C and A are only currencies as C$ and A$ */
        if (strlen(codes[k]) == 1) return memchr(p, '$', n) != NULL;
        return 1;
    }
    return 0;
}

/* Text such as "$1,234.50", "12 kg", "(1 234)", "25%" or "1.5E+03" as a number:
   raw gets the digits (with sign), ns the style, decimals, prefix and suffix. Returns
   1 when the text is one of these, 0 for anything else (text stays text). Digits
   in a suffix make it text, so "Room 12" and "3 apples 4" stay text. */
int num_from_text(const char *in, char *raw, size_t rawn, NumSpec *ns) {
    ns->style = '-';
    ns->dec = -1;
    ns->pre[0] = ns->suf[0] = '\0';
    if (!in || !raw || !rawn) return 0;
    const char *p = in, *e;
    while (isspace((unsigned char)*p)) p++;
    e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) e--;
    if (e == p || e - p > 96) return 0;
    const char *q = p;
    while (q < e && !isdigit((unsigned char)*q) && !(*q == '.' && q + 1 < e && isdigit((unsigned char)q[1]))) q++;
    if (q == e) return 0;
    int neg = 0, paren = 0;
    char pre[64];
    size_t pn = 0;
    for (const char *x = p; x < q; x++) {
        if (*x == '-') neg = 1;
        else if (*x == '(') paren = 1;
        else if (*x == '+') continue;
        else { if (pn + 1 >= sizeof pre) return 0; pre[pn++] = *x; }
    }
    size_t ps = 0;
    while (ps < pn && pre[ps] == ' ') ps++;
    if (paren) neg = 1;                          /* (1,234) is negative */
    if (!prefix_ok(pre + ps, pn - ps)) return 0;
    if (pn - ps >= sizeof ns->pre) return 0;
    memcpy(ns->pre, pre + ps, pn - ps);
    ns->pre[pn - ps] = '\0';

    /* integer digits; a space or comma is a group separator only before exactly three digits */
    const char *s = q;
    char ip[64];
    size_t ni = 0, glen = 0, groups = 0;
    char sep = 0;
    int grouped = 0;
    for (;;) {
        if (s < e && isdigit((unsigned char)*s)) {
            if (ni + 1 >= sizeof ip) return 0;
            ip[ni++] = *s++;
            glen++;
            continue;
        }
        if (ni && s < e && (*s == ',' || *s == ' ') && s + 4 <= e &&
            isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) &&
            (s + 4 == e || !isdigit((unsigned char)s[4]))) {
            if (!sep) sep = *s;
            else if (sep != *s) return 0;
            if (groups == 0 ? (glen < 1 || glen > 3) : glen != 3) return 0;
            groups++;
            grouped = 1;
            glen = 0;
            s++;
            continue;
        }
        break;
    }
    if (!ni || (grouped && glen != 3)) return 0;

    char fp[32];
    size_t fn = 0;
    if (s < e && *s == '.') {
        s++;
        while (s < e && isdigit((unsigned char)*s)) {
            if (fn + 1 >= sizeof fp) return 0;
            fp[fn++] = *s++;
        }
        if (!fn) return 0;                       /* "12." is not a number here */
    }

    char ex[16] = "";
    if (s < e && (*s == 'e' || *s == 'E') && !grouped) {
        const char *t = s + 1;
        size_t en = 0, dig = 0;
        ex[en++] = 'E';
        if (t < e && (*t == '+' || *t == '-')) ex[en++] = *t++;
        while (t < e && isdigit((unsigned char)*t) && en < sizeof ex - 1) { ex[en++] = *t++; dig++; }
        if (dig) { ex[en] = '\0'; s = t; }
        else ex[0] = '\0';
    }

    const char *sfx = s;
    if (paren) {
        if (sfx < e && *sfx == ')') sfx++;
        else return 0;
    } else if (sfx < e && *sfx == ')') return 0;
    size_t sn = (size_t)(e - sfx);
    if (sn >= sizeof ns->suf) return 0;
    for (size_t k = 0; k < sn; k++)
        if (isdigit((unsigned char)sfx[k]) || sfx[k] == '\t' || sfx[k] == '\n') return 0;
    memcpy(ns->suf, sfx, sn);
    ns->suf[sn] = '\0';

    char out[160];
    size_t rn = 0;
    if (neg) out[rn++] = '-';
    for (size_t k = 0; k < ni; k++) out[rn++] = ip[k];
    if (fn) { out[rn++] = '.'; for (size_t k = 0; k < fn; k++) out[rn++] = fp[k]; }
    for (size_t k = 0; ex[k]; k++) out[rn++] = ex[k];
    out[rn] = '\0';
    if (rn + 1 > rawn) return 0;
    memcpy(raw, out, rn + 1);

    if (grouped) ns->style = sep == ' ' ? ' ' : ',';
    ns->dec = fn ? (int)fn : -1;
    if (ex[0]) { ns->style = '-'; ns->dec = -1; }
    return 1;
}

/* ---------- reference shifting (copy/paste/fill) ---------- */

typedef struct { char *s; size_t n, cap; } Str;

static void str_add(Str *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        char *p = realloc(b->s, cap);
        if (!p) return;
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

char *formula_shift(const char *f, int dr, int dc) {
    Str b = {NULL, 0, 0};
    const char *p = f;
    while (*p) {
        if (isdigit((unsigned char)*p)) {          /* numbers pass through untouched */
            const char *q = p;
            while (isalnum((unsigned char)*q) || *q == '.') q++;
            str_add(&b, p, (size_t)(q - p));
            p = q;
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '$' || *p == '_') {
            const char *q = p;
            while (isalnum((unsigned char)*q) || *q == '_' || *q == '$') q++;
            Ref ref = {0, 0, 0, 0};
            const char *e;
            if (*q != '(' && parse_ref_at(p, &e, &ref) && e == q) {
                if (!ref.ac) ref.c += dc;
                if (!ref.ar) ref.r += dr;
                if (ref.r < 0 || ref.c < 0 || ref.r >= MAX_ROWS || ref.c >= MAX_COLS) {
                    str_add(&b, "#REF!", 5);
                } else {
                    char cn[8], num[16];
                    col_name(ref.c, cn);
                    snprintf(num, sizeof num, "%d", ref.r + 1);
                    if (ref.ac) str_add(&b, "$", 1);
                    str_add(&b, cn, strlen(cn));
                    if (ref.ar) str_add(&b, "$", 1);
                    str_add(&b, num, strlen(num));
                }
            } else {
                str_add(&b, p, (size_t)(q - p));  /* function name or plain word */
            }
            p = q;
            continue;
        }
        str_add(&b, p, 1);
        p++;
    }
    if (!b.s) return strdup("");
    return b.s;
}


/* The selection as rectangles. A whole-column or whole-row selection (it runs to
   the grid's edge) is clipped to the used area so it stays cheap; any other
   range keeps its full size, so empty cells in it are included. Returns the
   count (doc_nrects). */
int doc_rects(const Doc *d, Rect *out) {
    int n = doc_nrects(d);
    for (int i = 0; i < n; i++) {
        Rect *q = &out[i];
        doc_rect(d, i, q);
        if (q->r2 == MAX_ROWS - 1 && q->r2 > d->g->max_r) q->r2 = q->r1 > d->g->max_r ? q->r1 : d->g->max_r;
        if (q->c2 == MAX_COLS - 1 && q->c2 > d->g->max_c) q->c2 = q->c1 > d->g->max_c ? q->c1 : d->g->max_c;
    }
    return n;
}
