/* HTML tables: export of a sheet range (with bold, highlight and merged
   blocks as colspan/rowspan), and import of a table from the clipboard. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsheet.h"

static int bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        char *p = realloc(b->s, cap);
        if (!p) return 0;
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
    return 1;
}

static int bputs(Buf *b, const char *s) { return bput(b, s, strlen(s)); }

static int bput_escaped(Buf *b, char ch) {
    switch (ch) {
    case '&': return bputs(b, "&amp;");
    case '<': return bputs(b, "&lt;");
    case '>': return bputs(b, "&gt;");
    case '"': return bputs(b, "&quot;");
    default:  return bput(b, &ch, 1);
    }
}

/* ---------- export ---------- */

char *html_build(const Sheet *sh, int r1, int c1, int r2, int c2) {
    Buf b = {NULL, 0, 0};
    int ok = bputs(&b, "<html><head><meta charset=\"utf-8\"></head><body>"
                       "<!--StartFragment--><table border=\"1\" cellspacing=\"0\" cellpadding=\"2\">\n");
    for (int r = r1; ok && r <= r2; r++) {
        ok = bputs(&b, "<tr>");
        for (int c = c1; ok && c <= c2; c++) {
            const Merge *m = sheet_merge_of(sh, r, c);
            if (m && (m->r1 != r || m->c1 != c)) continue;        /* covered by a merged block */
            int rs = 1, cs = 1;
            if (m) {
                rs = (m->r2 < r2 ? m->r2 : r2) - r + 1;
                cs = (m->c2 < c2 ? m->c2 : c2) - c + 1;
            }
            char txt[256], attrs[128] = "";
            int align = 0, bold = 0, hl = 0;
            cell_display(sh, r, c, txt, sizeof txt, &align);
            sheet_style(sh, r, c, &bold, &hl);
            char al[24] = "";
            if (align == 1) snprintf(al, sizeof al, " align=\"right\"");
            else if (align == 2) snprintf(al, sizeof al, " align=\"center\"");
            char sp[48] = "";
            if (rs > 1) snprintf(sp + strlen(sp), sizeof sp - strlen(sp), " rowspan=\"%d\"", rs);
            if (cs > 1) snprintf(sp + strlen(sp), sizeof sp - strlen(sp), " colspan=\"%d\"", cs);
            snprintf(attrs, sizeof attrs, "%s%s%s", al, sp, hl ? " style=\"background-color:#ffff00\"" : "");
            ok = bputs(&b, "<td");
            if (ok) ok = bputs(&b, attrs);
            if (ok) ok = bputs(&b, ">");
            if (ok && bold) ok = bputs(&b, "<b>");
            for (const char *p = txt; ok && *p; p++) ok = bput_escaped(&b, *p);
            if (ok && bold) ok = bputs(&b, "</b>");
            if (ok) ok = bputs(&b, "</td>");
        }
        if (ok) ok = bputs(&b, "</tr>\n");
    }
    if (ok) ok = bputs(&b, "</table><!--EndFragment--></body></html>\n");
    if (!ok) { free(b.s); return NULL; }
    return b.s;
}

/* ---------- import ---------- */

static void utf8_put(Buf *b, unsigned cp) {
    char out[4];
    int n;
    if (cp < 0x80) { out[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    bput(b, out, (size_t)n);
}

/* Decodes entities and collapses whitespace; returns a malloc'd trimmed string. */
static char *html_text(const char *in, size_t len) {
    Buf b = {NULL, 0, 0};
    int space = 0;
    for (size_t i = 0; i < len;) {
        unsigned cp = 0;
        size_t adv = 1;
        if (in[i] == '&') {
            size_t j = i + 1;
            while (j < len && j - i < 10 && in[j] != ';') j++;
            if (j < len && in[j] == ';' && j > i + 1) {
                const char *nm = in + i + 1;
                size_t nlen = j - i - 1;
                if (nlen == 3 && !strncmp(nm, "amp", 3)) cp = '&';
                else if (nlen == 2 && !strncmp(nm, "lt", 2)) cp = '<';
                else if (nlen == 2 && !strncmp(nm, "gt", 2)) cp = '>';
                else if (nlen == 4 && !strncmp(nm, "quot", 4)) cp = '"';
                else if (nlen == 4 && !strncmp(nm, "apos", 4)) cp = '\'';
                else if (nlen == 4 && !strncmp(nm, "nbsp", 4)) cp = ' ';
                else if (nlen > 1 && nm[0] == '#') {
                    char *e = NULL;
                    unsigned long v = (nm[1] == 'x' || nm[1] == 'X')
                                          ? strtoul(nm + 2, &e, 16)
                                          : strtoul(nm + 1, &e, 10);
                    if (e == nm + nlen) cp = (unsigned)v;
                }
                if (cp) adv = j - i + 1;
            }
        }
        if (cp) {
            if (cp == ' ') {
                if (!space && b.n) { bput(&b, " ", 1); space = 1; }
            } else {
                utf8_put(&b, cp);
                space = 0;
            }
        } else {
            char ch = in[i];
            if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
                if (!space && b.n) { bput(&b, " ", 1); space = 1; }
            } else {
                bput(&b, &ch, 1);
                space = 0;
            }
        }
        i += adv;
    }
    if (b.n && b.s[b.n - 1] == ' ') b.n--;
    if (!b.s) return strdup("");
    b.s[b.n] = '\0';
    return b.s;
}

/* A number attribute (colspan, rowspan) inside one tag, default 1. */
static int tag_number(const char *tag, const char *end, const char *name) {
    size_t l = strlen(name);
    for (const char *p = tag; p + l < end; p++) {
        if (!strncmp(p, name, l) && p[l] == '=' && (p == tag || isspace((unsigned char)p[-1]))) {
            const char *v = p + l + 1;
            if (*v == '"' || *v == '\'') v++;
            int n = atoi(v);
            return n > 0 && n < 1000 ? n : 1;
        }
    }
    return 1;
}

typedef struct { int r, c; } Pos;

static int occupied(const Pos *occ, int n, int r, int c) {
    for (int i = 0; i < n; i++)
        if (occ[i].r == r && occ[i].c == c) return 1;
    return 0;
}

/* A tag attribute's value, lowercased (quoted or bare). Returns 1 when present. */
static int tag_attr_str(const char *tag, const char *end, const char *name, char *out, size_t n) {
    size_t l = strlen(name);
    if (n) out[0] = '\0';
    for (const char *p = tag; p + l < end; p++) {
        if (strncasecmp(p, name, l) || p[l] != '=' || (p != tag && !isspace((unsigned char)p[-1]))) continue;
        const char *v = p + l + 1;
        char q = 0;
        if (*v == '"' || *v == '\'') q = *v++;
        size_t k = 0;
        while (v < end && k + 1 < n && (q ? *v != q : !isspace((unsigned char)*v) && *v != '>')) {
            out[k++] = (char)tolower((unsigned char)*v++);
        }
        out[k] = '\0';
        return 1;
    }
    return 0;
}

/* A colour that is no highlight: white, transparent or none. */
static int colour_is_plain(const char *v) {
    return !*v || !strcmp(v, "#fff") || !strcmp(v, "#ffffff") || !strcmp(v, "white") ||
           !strcmp(v, "transparent") || !strcmp(v, "none") || !strcmp(v, "rgb(255,255,255)");
}

/* The formatting a <td> asks for: bold weight, a background other than white,
   and alignment (from style="text-align:" or the align attribute). */
static void td_style(const char *tag, const char *end, int *bold, int *hl, char *align) {
    char st[256], s2[256], v[64];
    if (tag_attr_str(tag, end, "style", st, sizeof st)) {
        size_t k = 0;
        for (const char *x = st; *x && k + 1 < sizeof s2; x++)
            if (!isspace((unsigned char)*x)) s2[k++] = *x;
        s2[k] = '\0';
        if (strstr(s2, "font-weight:bold") || strstr(s2, "font-weight:bolder") || strstr(s2, "font-weight:700") ||
            strstr(s2, "font-weight:800") || strstr(s2, "font-weight:900"))
            *bold = 1;
        const char *bg = strstr(s2, "background");
        const char *colon = bg ? strchr(bg, ':') : NULL;
        if (colon) {
            char col[32];
            size_t cn = 0;
            for (const char *x = colon + 1; *x && *x != ';' && cn + 1 < sizeof col; x++) col[cn++] = *x;
            col[cn] = '\0';
            if (!colour_is_plain(col)) *hl = 1;
        }
        const char *ta = strstr(s2, "text-align:");
        if (ta) {
            const char *a = ta + 11;
            if (!strncmp(a, "right", 5)) *align = '>';
            else if (!strncmp(a, "center", 6)) *align = '^';
            else if (!strncmp(a, "left", 4)) *align = '<';
        }
    }
    if (tag_attr_str(tag, end, "bgcolor", v, sizeof v) && !colour_is_plain(v)) *hl = 1;
    if (tag_attr_str(tag, end, "align", v, sizeof v)) {
        if (!strcmp(v, "right")) *align = '>';
        else if (!strcmp(v, "center")) *align = '^';
        else if (!strcmp(v, "left")) *align = '<';
    }
}

static void cell_push(HTable *t, int r, int c, int rs, int cs, char *text, int bold, int hl, char align) {
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 64;
        HCell *p = realloc(t->cells, cap * sizeof *p);
        if (!p) { free(text); return; }
        t->cells = p;
        t->cap = cap;
    }
    t->cells[t->n].r = r;
    t->cells[t->n].c = c;
    t->cells[t->n].rs = rs;
    t->cells[t->n].cs = cs;
    t->cells[t->n].text = text;
    t->cells[t->n].bold = (unsigned char)bold;
    t->cells[t->n].hl = (unsigned char)hl;
    t->cells[t->n].align = align;
    t->n++;
    if (r + rs > t->rows) t->rows = r + rs;
    if (c + cs > t->cols) t->cols = c + cs;
}

/* Reads the first <table> in h, honouring colspan and rowspan. Returns 1 if any cell. */
int html_parse_table(const char *h, HTable *t) {
    memset(t, 0, sizeof *t);
    Buf text = {NULL, 0, 0};
    Pos *occ = NULL;
    int nocc = 0, cocc = 0;
    int row = -1, col = -1, in_cell = 0, skip = 0, in_table = 0, done = 0;
    int cr = 0, cc = 0, crs = 1, ccs = 1;             /* the open cell */
    int cur_bold = 0, cur_hl = 0;
    char cur_align = 0;
    const char *p = h;
    while (*p && !done) {
        if (*p != '<') {
            if (in_table && in_cell && !skip) bput(&text, p, 1);
            p++;
            continue;
        }
        if (!strncmp(p, "<!--", 4)) {
            const char *e = strstr(p + 4, "-->");
            p = e ? e + 3 : p + strlen(p);
            continue;
        }
        const char *end = strchr(p, '>');
        if (!end) break;
        int closing = p[1] == '/';
        const char *q = p + 1 + closing;
        char name[12];
        size_t k = 0;
        while (q < end && isalpha((unsigned char)*q) && k < sizeof name - 1)
            name[k++] = (char)tolower((unsigned char)*q++);
        name[k] = '\0';

        if (in_cell && (!strcmp(name, "td") || !strcmp(name, "th") ||
                        !strcmp(name, "tr") || !strcmp(name, "table"))) {
            char *tx = html_text(text.s ? text.s : "", text.n);
            cell_push(t, cr, cc, crs, ccs, tx, cur_bold, cur_hl, cur_align);
            text.n = 0;
            in_cell = 0;
        }
        if (!strcmp(name, "table")) {
            if (!closing) in_table = 1;
            else if (in_table) done = 1;
        } else if (!in_table) {
            /* outside the first table: ignored */
        } else if (!strcmp(name, "tr")) {
            if (!closing) { row++; col = -1; }
        } else if (!strcmp(name, "td") || !strcmp(name, "th")) {
            if (!closing) {
                if (row < 0) row = 0;
                col++;
                while (occupied(occ, nocc, row, col)) col++;   /* skip cells covered by spans */
                int rs = tag_number(p, end, "rowspan"), cs = tag_number(p, end, "colspan");
                for (int dr = 0; dr < rs; dr++)
                    for (int dc = 0; dc < cs; dc++) {
                        if (dr == 0 && dc == 0) continue;
                        if (nocc == cocc) {
                            cocc = cocc ? cocc * 2 : 32;
                            Pos *np = realloc(occ, (size_t)cocc * sizeof *np);
                            if (!np) break;
                            occ = np;
                        }
                        occ[nocc].r = row + dr;
                        occ[nocc].c = col + dc;
                        nocc++;
                    }
                cr = row;
                cc = col;
                crs = rs;
                ccs = cs;
                in_cell = 1;
                text.n = 0;
                cur_bold = cur_hl = 0;
                cur_align = 0;
                td_style(p, end, &cur_bold, &cur_hl, &cur_align);
            }
        } else if (in_cell && !closing && (!strcmp(name, "b") || !strcmp(name, "strong"))) {
            cur_bold = 1;                       /* <b> and <strong> inside a cell */
        } else if (!strcmp(name, "style") || !strcmp(name, "script")) {
            skip = !closing;
        } else if (in_cell && (!strcmp(name, "br") || !strcmp(name, "p") || !strcmp(name, "div"))) {
            bput(&text, " ", 1);
        }
        p = end + 1;
    }
    if (in_cell) {
        char *tx = html_text(text.s ? text.s : "", text.n);
        cell_push(t, cr, cc, crs, ccs, tx, cur_bold, cur_hl, cur_align);
    }
    free(occ);
    free(text.s);
    return t->n > 0;
}

void html_table_free(HTable *t) {
    if (!t) return;
    for (size_t i = 0; i < t->n; i++) free(t->cells[i].text);
    free(t->cells);
    memset(t, 0, sizeof *t);
}
