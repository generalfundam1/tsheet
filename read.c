/* Reading xlsx and ods. Both are zip files of XML. This file has the pieces
   they need and nothing more: an inflate (RFC 1951) for deflated entries, a zip
   reader that finds entries by name, and a scanner for the few elements a sheet
   uses. Formulas are kept as the spreadsheet wrote them (converted to tsheet's
   A1 form); values are read as text and the sheet computes them. Only the first
   sheet of a workbook is read. */
#include "tsheet.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void bput(Buf *b, char c) {
    if (b->n + 2 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char *p = realloc(b->s, cap);
        if (!p) return;
        b->s = p;
        b->cap = cap;
    }
    b->s[b->n++] = c;
}

static void bput_utf8(Buf *b, unsigned long cp) {
    if (cp < 0x80) bput(b, (char)cp);
    else if (cp < 0x800) { bput(b, (char)(0xC0 | (cp >> 6))); bput(b, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        bput(b, (char)(0xE0 | (cp >> 12))); bput(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        bput(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        bput(b, (char)(0xF0 | (cp >> 18))); bput(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        bput(b, (char)(0x80 | ((cp >> 6) & 0x3F))); bput(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* ---------- inflate ---------- */

typedef struct {
    const unsigned char *in;
    size_t n, pos;
    unsigned bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t outn, outcap;
    int err;
} Inf;

#define INF_MAX ((size_t)256 << 20)     /* a sheet file this big is refused */

static int bits(Inf *s, int need) {
    while (s->bitcnt < need) {
        if (s->pos >= s->n) { s->err = 1; return 0; }
        s->bitbuf |= (unsigned)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

static int put(Inf *s, unsigned char c) {
    if (s->outn >= INF_MAX) { s->err = 1; return -1; }
    if (s->outn == s->outcap) {
        size_t cap = s->outcap ? s->outcap * 2 : 4096;
        unsigned char *p = realloc(s->out, cap);
        if (!p) { s->err = 1; return -1; }
        s->out = p;
        s->outcap = cap;
    }
    s->out[s->outn++] = c;
    return 0;
}

typedef struct { short count[16]; short symbol[320]; } Huff;

/* Canonical Huffman table from code lengths. Returns 0 when complete, >0 when
   incomplete, <0 when over-subscribed. */
static int construct(Huff *h, const short *len, int n) {
    for (int l = 0; l < 16; l++) h->count[l] = 0;
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int s = 0; s < n; s++)
        if (len[s]) h->symbol[offs[len[s]]++] = (short)s;
    return left;
}

static int decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                               3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                8193, 12289, 16385, 24577};
static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                               7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Inf *s, const Huff *lc, const Huff *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (s->err || sym < 0) return -1;
        if (sym < 256) {
            if (put(s, (unsigned char)sym)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = lbase[sym] + bits(s, lext[sym]);
            int ds = decode(s, dc);
            if (s->err || ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)dbase[ds] + (size_t)bits(s, dext[ds]);
            if (s->err || dist > s->outn) return -1;
            for (int i = 0; i < len; i++)
                if (put(s, s->out[s->outn - dist])) return -1;
        }
    }
}

static void fixed_tables(Huff *lc, Huff *dc) {
    short l[288];
    int s = 0;
    for (; s < 144; s++) l[s] = 8;
    for (; s < 256; s++) l[s] = 9;
    for (; s < 280; s++) l[s] = 7;
    for (; s < 288; s++) l[s] = 8;
    construct(lc, l, 288);
    for (s = 0; s < 30; s++) l[s] = 5;
    construct(dc, l, 30);
}

static int dynamic_tables(Inf *s, Huff *lc, Huff *dc) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    for (int i = 0; i < 19; i++) lengths[i] = 0;
    for (int i = 0; i < ncode; i++) lengths[order[i]] = (short)bits(s, 3);
    if (s->err) return -1;
    Huff cl;
    if (construct(&cl, lengths, 19) != 0) return -1;
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(s, &cl);
        if (s->err || sym < 0) return -1;
        if (sym < 16) { lengths[idx++] = (short)sym; continue; }
        int rep, len = 0;
        if (sym == 16) {
            if (idx == 0) return -1;
            len = lengths[idx - 1];
            rep = 3 + bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + bits(s, 3);
        } else {
            rep = 11 + bits(s, 7);
        }
        if (s->err || idx + rep > nlen + ndist) return -1;
        while (rep--) lengths[idx++] = (short)len;
    }
    if (lengths[256] == 0) return -1;
    int e = construct(lc, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lc->count[0] != 1)) return -1;
    e = construct(dc, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - dc->count[0] != 1)) return -1;
    return 0;
}

/* Inflates a raw deflate stream. The result is malloc'd into *out. */
static int inflate_raw(const unsigned char *in, size_t n, unsigned char **out, size_t *outn) {
    Inf s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.n = n;
    Huff lc, dc;
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.err) goto fail;
        if (type == 0) {                            /* stored: byte aligned */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.pos + 4 > n) goto fail;
            unsigned len = in[s.pos] | (in[s.pos + 1] << 8);
            unsigned nlen = in[s.pos + 2] | (in[s.pos + 3] << 8);
            if (len != (~nlen & 0xFFFFu)) goto fail;
            s.pos += 4;
            if (s.pos + len > n) goto fail;
            for (unsigned i = 0; i < len; i++)
                if (put(&s, in[s.pos + i])) goto fail;
            s.pos += len;
        } else if (type == 1) {
            fixed_tables(&lc, &dc);
            if (codes(&s, &lc, &dc)) goto fail;
        } else if (type == 2) {
            if (dynamic_tables(&s, &lc, &dc)) goto fail;
            if (codes(&s, &lc, &dc)) goto fail;
        } else {
            goto fail;
        }
    } while (!last);
    *out = s.out;
    *outn = s.outn;
    return 0;
fail:
    free(s.out);
    return -1;
}

/* ---------- zip ---------- */

static uint32_t rd16(const unsigned char *p) { return (uint32_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Finds an entry by name and returns its bytes (malloc'd). Returns 0 when found,
   1 when there is no such entry, -1 when the archive is broken or unsupported. */
static int zip_entry(const char *zb, size_t zn, const char *name, unsigned char **out, size_t *outn) {
    const unsigned char *z = (const unsigned char *)zb;
    *out = NULL;                                 /* the caller frees *out only when this returns 0 */
    *outn = 0;
    if (zn < 22) return -1;
    size_t lo = zn > 22 + 65535 ? zn - 22 - 65535 : 0, eocd = (size_t)-1;
    for (size_t i = zn - 22; ; i--) {
        if (rd32(z + i) == 0x06054b50) { eocd = i; break; }
        if (i == lo) break;
    }
    if (eocd == (size_t)-1) return -1;
    size_t count = rd16(z + eocd + 10), cdsize = rd32(z + eocd + 12), cdoff = rd32(z + eocd + 16);
    if (cdoff + cdsize > zn) return -1;
    size_t p = cdoff, nl = strlen(name);
    for (size_t k = 0; k < count; k++) {
        if (p + 46 > cdoff + cdsize || rd32(z + p) != 0x02014b50) return -1;
        uint32_t method = rd16(z + p + 10);
        size_t csize = rd32(z + p + 20), usize = rd32(z + p + 24);
        size_t nlen = rd16(z + p + 28), elen = rd16(z + p + 30), clen = rd16(z + p + 32);
        size_t loff = rd32(z + p + 42);
        if (p + 46 + nlen > zn) return -1;
        if (nlen == nl && !memcmp(z + p + 46, name, nl)) {
            if (loff + 30 > zn || rd32(z + loff) != 0x04034b50) return -1;
            size_t data = loff + 30 + rd16(z + loff + 26) + rd16(z + loff + 28);
            if (data + csize > zn) return -1;
            if (method == 0) {
                *out = malloc(csize ? csize : 1);
                if (!*out) return -1;
                memcpy(*out, z + data, csize);
                *outn = csize;
                return 0;
            }
            if (method == 8) {
                if (inflate_raw(z + data, csize, out, outn)) return -1;
                if (*outn != usize) { free(*out); *out = NULL; *outn = 0; return -1; }
                return 0;
            }
            return -1;                           /* encrypted or another method */
        }
        p += 46 + nlen + elen + clen;
    }
    return 1;
}

/* ---------- XML scanning ---------- */

/* First occurrence of s in [p, end), or NULL. */
static const char *find(const char *p, const char *end, const char *s) {
    size_t l = strlen(s);
    for (; p + l <= end; p++)
        if (*p == *s && !memcmp(p, s, l)) return p;
    return NULL;
}

/* One XML entity at s (s points at '&'): writes its UTF-8 to out (olen bytes) and
   returns the characters used, or 0 when it is not an entity. */
static size_t entity(const char *s, const char *e, char *out, size_t *olen) {
    const char *semi = memchr(s, ';', (size_t)(e - s) < 12 ? (size_t)(e - s) : 12);
    if (!semi) return 0;
    const char *x = s + 1;
    size_t l = (size_t)(semi - x);
    unsigned long cp;
    if (l == 3 && !memcmp(x, "amp", 3)) cp = '&';
    else if (l == 2 && !memcmp(x, "lt", 2)) cp = '<';
    else if (l == 2 && !memcmp(x, "gt", 2)) cp = '>';
    else if (l == 4 && !memcmp(x, "quot", 4)) cp = '"';
    else if (l == 4 && !memcmp(x, "apos", 4)) cp = '\'';
    else if (l > 1 && x[0] == '#') {
        int hex = x[1] == 'x';
        const char *d = x + (hex ? 2 : 1);
        if (d >= semi) return 0;
        cp = 0;
        for (const char *q = d; q < semi; q++) {
            int v;
            if (isdigit((unsigned char)*q)) v = *q - '0';
            else if (hex && isxdigit((unsigned char)*q)) v = tolower((unsigned char)*q) - 'a' + 10;
            else return 0;
            cp = cp * (hex ? 16 : 10) + (unsigned long)v;
            if (cp > 0x10FFFF) return 0;
        }
    } else return 0;
    Buf b = {NULL, 0, 0};
    bput_utf8(&b, cp);
    size_t k = b.n < 4 ? b.n : 4;
    memcpy(out, b.s, k);
    *olen = k;
    free(b.s);
    return (size_t)(semi - s) + 1;
}

/* The text of XML content [s, e): tags are dropped, entities decoded. Spaces,
   tabs and line breaks written as tags (ODS) become the characters they stand
   for, and a paragraph end adds a line break. Returns malloc'd text. */
static char *etext(const char *s, const char *e) {
    Buf b = {NULL, 0, 0};
    int gap = 0;                                    /* a paragraph ended: a space goes before more text */
    while (s < e) {
        if (*s == '<') {
            const char *t = memchr(s, '>', (size_t)(e - s));
            if (!t) break;
            if (!strncmp(s + 1, "text:s", 6) && (s[7] == ' ' || s[7] == '/' || s[7] == '>')) {
                char cnt[16] = "";
                int c = 1;
                if (strncmp(s + 1, "text:s", 6) == 0) {
                    /* text:c="N" gives N spaces */
                    for (const char *q = s + 7; q + 9 < t; q++)
                        if (!memcmp(q, "text:c=\"", 8)) {
                            size_t k = 0;
                            for (const char *d = q + 8; d < t && *d != '"' && k + 1 < sizeof cnt; d++) cnt[k++] = *d;
                            cnt[k] = '\0';
                            c = atoi(cnt);
                            break;
                        }
                }
                for (int i = 0; i < c && c < 10000; i++) bput(&b, ' ');
            } else if (!strncmp(s + 1, "text:tab", 8)) bput(&b, '\t');
            else if (!strncmp(s + 1, "text:line-break", 15)) bput(&b, '\n');
            else if (!strncmp(s + 1, "/text:p", 7)) gap = b.n > 0;      /* paragraphs: one line, a gap before more text */
            s = t + 1;
            continue;
        }
        if (*s == '&') {
            char u[4];
            size_t ul = 0;
            size_t used = entity(s, e, u, &ul);
            if (used) {
                for (size_t i = 0; i < ul; i++) bput(&b, u[i]);
                s += used;
                continue;
            }
        }
        if (gap) { bput(&b, ' '); gap = 0; }
        bput(&b, *s++);
    }
    bput(&b, '\0');
    if (!b.s) { b.s = malloc(1); if (b.s) b.s[0] = '\0'; }
    return b.s;
}

/* Value of attribute name in the tag [tag, end), entities decoded, into out.
   Returns 1 when present. */
static int attr(const char *tag, const char *end, const char *name, char *out, size_t n) {
    size_t l = strlen(name);
    if (n) out[0] = '\0';
    for (const char *p = tag + 1; p + l + 2 <= end; p++) {
        if (memcmp(p, name, l) || p[l] != '=' || p[l + 1] != '"') continue;
        if (!isspace((unsigned char)p[-1])) continue;
        const char *v = p + l + 2, *q = v;
        while (q < end && *q != '"') q++;
        if (q >= end) return 0;
        char *d = etext(v, q);
        if (d) { snprintf(out, n, "%s", d); free(d); }
        return 1;
    }
    return 0;
}

/* Text of the first <name ...>...</name> element in [s, e): NULL when there is
   none, "" for a self-closing element. */
static char *child(const char *s, const char *e, const char *name) {
    char pat[40], close[48];
    snprintf(pat, sizeof pat, "<%s", name);
    snprintf(close, sizeof close, "</%s>", name);
    for (const char *p = s; (p = find(p, e, pat)); p++) {
        const char *q = p + strlen(pat);
        if (q >= e) return NULL;
        if (*q != '>' && *q != ' ' && *q != '/') continue;
        const char *gt = memchr(q, '>', (size_t)(e - q));
        if (!gt) return NULL;
        if (gt[-1] == '/') return strdup("");
        const char *ce = find(gt + 1, e, close);
        if (!ce) ce = e;
        return etext(gt + 1, ce);
    }
    return NULL;
}

/* The next element named prefix (e.g. "<table:table-row") in [p, e): the prefix
   must be followed by a space, '>' or '/'. */
static const char *next_tag(const char *p, const char *e, const char *prefix) {
    size_t l = strlen(prefix);
    for (;;) {
        const char *q = find(p, e, prefix);
        if (!q) return NULL;
        char c = q + l < e ? q[l] : '\0';
        if (c == ' ' || c == '>' || c == '/') return q;
        p = q + 1;
    }
}

/* A1 reference (letters then digits) to 0-based row and column. */
static int parse_cell(const char *s, int *r, int *c) {
    int col = 0, nl = 0;
    while (isalpha((unsigned char)*s)) {
        col = col * 26 + (toupper((unsigned char)*s) - 'A' + 1);
        if (col > MAX_COLS) return 0;
        s++;
        nl++;
    }
    if (!nl || *s < '0' || *s > '9') return 0;
    long row = 0;
    while (*s >= '0' && *s <= '9') {
        row = row * 10 + (*s - '0');
        if (row > MAX_ROWS) return 0;
        s++;
    }
    if (*s) return 0;
    *r = (int)row - 1;
    *c = col - 1;
    return row >= 1 && col >= 1;
}

/* ---------- xlsx ---------- */

typedef struct { char **s; size_t n, cap; } Sst;

static void sst_free(Sst *t) {
    for (size_t i = 0; i < t->n; i++) free(t->s[i]);
    free(t->s);
}

/* The shared strings: each <si> is one string, the text of all its <t> parts. */
static void read_sst(const char *x, size_t n, Sst *t) {
    const char *p = x, *end = x + n;
    for (;;) {
        p = find(p, end, "<si");
        if (!p || p + 3 >= end) break;
        char c = p[3];
        if (c != '>' && c != ' ' && c != '/') { p += 3; continue; }
        char *txt;
        if (c == '/') {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            txt = strdup("");
            p = gt ? gt + 1 : end;
        } else {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            if (!gt) break;
            const char *ce = find(gt + 1, end, "</si>");
            if (!ce) ce = end;
            txt = etext(gt + 1, ce);
            p = ce < end ? ce + 5 : end;
        }
        if (!txt) break;
        if (t->n == t->cap) {
            size_t cap = t->cap ? t->cap * 2 : 64;
            char **q = realloc(t->s, cap * sizeof *q);
            if (!q) { free(txt); break; }
            t->s = q;
            t->cap = cap;
        }
        t->s[t->n++] = txt;
    }
}

static void xlsx_cells(Sheet *g, const char *x, size_t n, const Sst *t) {
    const char *p = x, *end = x + n;
    while ((p = find(p, end, "<c ")) != NULL) {
        const char *tg = memchr(p, '>', (size_t)(end - p));
        if (!tg) break;
        int self = tg[-1] == '/';
        const char *ce = self ? tg + 1 : find(tg, end, "</c>");
        if (!ce) ce = end;
        char ref[32], ty[16];
        int r, c;
        if (attr(p, tg, "r", ref, sizeof ref) && parse_cell(ref, &r, &c)) {
            attr(p, tg, "t", ty, sizeof ty);
            char *fx = child(tg + 1, ce, "f");
            char *v = child(tg + 1, ce, "v");
            char *is = child(tg + 1, ce, "is");
            char *raw = NULL;
            if (fx && fx[0]) {
                size_t l = strlen(fx) + 2;
                raw = malloc(l);
                if (raw) snprintf(raw, l, "=%s", fx);
            } else if (!strcmp(ty, "s")) {
                long idx = v ? atol(v) : -1;
                if (idx >= 0 && (size_t)idx < t->n) raw = strdup(t->s[idx]);
            } else if (!strcmp(ty, "b")) {
                raw = strdup(v && v[0] == '1' ? "TRUE" : "FALSE");
            } else if (!strcmp(ty, "inlineStr") || !strcmp(ty, "str")) {
                raw = is ? strdup(is) : v ? strdup(v) : NULL;
            } else if (v) {
                raw = strdup(v);            /* numbers, errors and formula results */
            }
            if (raw && raw[0]) sheet_set(g, r, c, raw);
            free(raw);
            free(fx);
            free(v);
            free(is);
        }
        p = ce;
    }
}

static void xlsx_merges(Sheet *g, const char *x, size_t n) {
    const char *p = x, *end = x + n;
    while ((p = find(p, end, "<mergeCell ")) != NULL) {
        const char *tg = memchr(p, '>', (size_t)(end - p));
        if (!tg) break;
        char ref[48];
        int r1, c1, r2, c2;
        const char *colon;
        if (attr(p, tg, "ref", ref, sizeof ref) && (colon = strchr(ref, ':')) != NULL) {
            *(char *)colon = '\0';
            if (parse_cell(ref, &r1, &c1) && parse_cell(colon + 1, &r2, &c2) && r1 <= r2 && c1 <= c2)
                sheet_merge_add(g, r1, c1, r2, c2);
        }
        p = tg + 1;
    }
}

static const char *rel_target(const char *x, size_t n, const char *id, char *out, size_t outn) {
    const char *p = x, *end = x + n;
    while ((p = next_tag(p, end, "<Relationship")) != NULL) {
        const char *tg = memchr(p, '>', (size_t)(end - p));
        if (!tg) return NULL;
        char rid[64];
        if (attr(p, tg, "Id", rid, sizeof rid) && !strcmp(rid, id) && attr(p, tg, "Target", out, outn))
            return out;
        p = tg + 1;
    }
    return NULL;
}

int read_xlsx(Sheet *g, const char *buf, size_t len, char *err, size_t errn) {
    unsigned char *wb = NULL, *rels = NULL, *sh = NULL, *sst = NULL;
    size_t wbn = 0, relsn = 0, shn = 0, sstn = 0;
    Sst t = {NULL, 0, 0};
    int rc = -1;
    char rid[64] = "", target[256] = "", path[300];
    int f;
    f = zip_entry(buf, len, "xl/workbook.xml", &wb, &wbn);
    if (f < 0) { snprintf(err, errn, "not an xlsx file (no readable workbook)"); return -1; }
    if (f == 0) {
        const char *p = find((const char *)wb, (const char *)wb + wbn, "<sheet ");
        if (p) {
            const char *tg = memchr(p, '>', wbn - (size_t)(p - (const char *)wb));
            if (tg) attr(p, tg, "r:id", rid, sizeof rid);
        }
    }
    if (zip_entry(buf, len, "xl/_rels/workbook.xml.rels", &rels, &relsn) == 0 && rid[0])
        rel_target((const char *)rels, relsn, rid, target, sizeof target);
    if (!target[0]) snprintf(target, sizeof target, "worksheets/sheet1.xml");
    if (target[0] == '/') snprintf(path, sizeof path, "%s", target + 1);
    else if (!strncmp(target, "xl/", 3)) snprintf(path, sizeof path, "%s", target);
    else snprintf(path, sizeof path, "xl/%s", target);
    f = zip_entry(buf, len, path, &sh, &shn);
    if (f != 0) {
        snprintf(err, errn, f < 0 ? "damaged worksheet in the xlsx file" : "no worksheet in the xlsx file");
        goto done;
    }
    if (zip_entry(buf, len, "xl/sharedStrings.xml", &sst, &sstn) == 0) read_sst((const char *)sst, sstn, &t);
    xlsx_cells(g, (const char *)sh, shn, &t);
    xlsx_merges(g, (const char *)sh, shn);
    rc = 0;
done:
    sst_free(&t);
    free(wb);
    free(rels);
    free(sh);
    free(sst);
    return rc;
}

/* ---------- ods ---------- */

/* An ODS formula as tsheet writes it: "of:=[.A1]+[.B1]" becomes "A1+B1"; ranges
   [.A1:.B2] become A1:B2; ';' separators become ','. */
static void ods_formula(Buf *o, const char *f) {
    const char *p = f;
    if (!strncmp(p, "of:=", 4)) p += 4;
    else if (!strncmp(p, "oooc:=", 6)) p += 6;
    else if (*p == '=') p++;
    bput(o, '=');
    while (*p) {
        if (*p == '[') {
            p++;
            char prev = '[';
            while (*p && *p != ']') {
                if (*p == '.' && (prev == '[' || prev == ':')) { prev = *p++; continue; }
                if (*p == '.') bput(o, '!');           /* Sheet.A1 is Sheet!A1 */
                else bput(o, *p);
                prev = *p++;
            }
            if (*p == ']') p++;
        } else {
            bput(o, *p == ';' ? ',' : *p);
            p++;
        }
    }
    bput(o, '\0');
}

/* The value of one ODS cell (its tag [q, tg] and content [cs, ce)). malloc'd, or NULL when empty. */
static char *ods_value(const char *q, const char *tg, const char *cs, const char *ce) {
    char vt[24] = "", fm[8192] = "", ov[512] = "", bv[16] = "";
    attr(q, tg, "office:value-type", vt, sizeof vt);
    attr(q, tg, "table:formula", fm, sizeof fm);
    if (fm[0]) {
        Buf o = {NULL, 0, 0};
        ods_formula(&o, fm);
        return o.s;
    }
    char *text = etext(cs, ce);
    char *out = NULL;
    if (!strcmp(vt, "float") || !strcmp(vt, "percentage") || !strcmp(vt, "currency")) {
        if (attr(q, tg, "office:value", ov, sizeof ov) && ov[0]) out = strdup(ov);
    } else if (!strcmp(vt, "boolean")) {
        attr(q, tg, "office:boolean-value", bv, sizeof bv);
        out = strdup(bv[0] == 't' ? "TRUE" : "FALSE");
    } else if (!strcmp(vt, "date")) {
        if (attr(q, tg, "office:date-value", ov, sizeof ov) && ov[0]) out = strdup(ov);
    }
    if (!out && text && text[0]) out = strdup(text);
    free(text);
    return out;
}

static void ods_row(Sheet *g, const char *s, const char *e, int row) {
    long col = 0;
    const char *p = s;
    while (p < e && col < MAX_COLS) {
        const char *a = next_tag(p, e, "<table:table-cell");
        const char *b = next_tag(p, e, "<table:covered-table-cell");
        const char *q = a;
        int covered = 0;
        if (!q || (b && b < q)) { q = b; covered = 1; }
        if (!q) break;
        const char *tg = memchr(q, '>', (size_t)(e - q));
        if (!tg) break;
        int self = tg[-1] == '/';
        const char *cs = tg + 1, *ce = tg + 1;
        const char *close = covered ? "</table:covered-table-cell>" : "</table:table-cell>";
        size_t closel = strlen(close);
        if (!self) {
            ce = find(tg + 1, e, close);
            if (!ce) ce = e;
        }
        char tmp[32];
        long crep = 1, rs = 1, cs2 = 1;
        if (attr(q, tg, "table:number-columns-repeated", tmp, sizeof tmp)) crep = atol(tmp);
        if (attr(q, tg, "table:number-columns-spanned", tmp, sizeof tmp)) cs2 = atol(tmp);
        if (attr(q, tg, "table:number-rows-spanned", tmp, sizeof tmp)) rs = atol(tmp);
        if (crep < 1) crep = 1;
        if (cs2 < 1) cs2 = 1;
        if (rs < 1) rs = 1;
        if (!covered) {
            char *raw = ods_value(q, tg, cs, ce);
            if (raw) {
                int formula = raw[0] == '=';
                for (long k = 0; k < crep && col + k < MAX_COLS; k++) {
                    if (formula && k > 0) break;    /* a copied formula would point at the wrong cells */
                    sheet_set(g, row, (int)(col + k), raw);
                }
                free(raw);
            }
            if (cs2 > 1 || rs > 1)
                sheet_merge_add(g, row, (int)col, row + (int)rs - 1, (int)(col + cs2 - 1));
        }
        col += crep;
        p = self ? tg + 1 : ce + closel;
    }
}

int read_ods(Sheet *g, const char *buf, size_t len, char *err, size_t errn) {
    unsigned char *cx = NULL;
    size_t cn = 0;
    int f = zip_entry(buf, len, "content.xml", &cx, &cn);
    if (f != 0) {
        snprintf(err, errn, f < 0 ? "damaged ods file" : "not an ods file (no content.xml)");
        return -1;
    }
    const char *x = (const char *)cx, *end = x + cn;
    const char *ts = next_tag(x, end, "<table:table");
    if (!ts) {
        free(cx);
        snprintf(err, errn, "no table in the ods file");
        return -1;
    }
    const char *te = find(ts, end, "</table:table>");
    if (!te) te = end;
    long r = 0;
    const char *p = ts;
    while (r < MAX_ROWS && (p = next_tag(p, te, "<table:table-row")) != NULL) {
        const char *tg = memchr(p, '>', (size_t)(te - p));
        if (!tg) break;
        char tmp[32];
        long rep = 1;
        if (attr(p, tg, "table:number-rows-repeated", tmp, sizeof tmp)) rep = atol(tmp);
        if (rep < 1) rep = 1;
        const char *re = tg + 1;
        if (tg[-1] != '/') {
            re = find(tg + 1, te, "</table:table-row>");
            if (!re) re = te;
        }
        /* a row with nothing in it is skipped in one step, however often it repeats */
        int content = find(tg + 1, re, "office:value-type") || find(tg + 1, re, "table:formula") ||
                      find(tg + 1, re, "<text:p");
        if (tg[-1] != '/' && content)
            for (long i = 0; i < rep && r + i < MAX_ROWS; i++) ods_row(g, tg + 1, re, (int)(r + i));
        r += rep;
        p = re;
    }
    free(cx);
    return 0;
}
