/* File formats: CSV/TSV (RFC 4180 quoting), flat XML, and xlsx/ods writers.
   Zip containers are written with stored (uncompressed) entries and CRC-32,
   which every spreadsheet reader accepts. Reading xlsx/ods is not implemented. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "tsheet.h"

static const char *fmt_names[FMT_COUNT] = {"csv", "tsv", "xml", "xlsx", "ods", "json"};

const char *io_fmt_name(int fmt) { return fmt_names[fmt]; }

void buf_put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2 + 256;
        char *p = realloc(b->s, cap);
        if (!p) return;
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void bputs(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static void bputc(Buf *b, char c) { buf_put(b, &c, 1); }
void buf_free(Buf *b) { free(b->s); memset(b, 0, sizeof *b); }

/* ---------- reading ---------- */

int io_read_fd(int fd, char **out, size_t *len) {
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap + 1);
    if (!b) return -1;
    for (;;) {
        if (n == cap) {
            cap *= 2;
            char *p = realloc(b, cap + 1);
            if (!p) { free(b); return -1; }
            b = p;
        }
        ssize_t k = read(fd, b + n, cap - n);
        if (k < 0) {
            if (errno == EINTR) continue;
            free(b);
            return -1;
        }
        if (k == 0) break;
        n += (size_t)k;
    }
    b[n] = '\0';
    *out = b;
    *len = n;
    return 0;
}

int io_read_file(const char *path, char **out, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = io_read_fd(fd, out, len);
    close(fd);
    return r;
}

int io_fmt_from_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return -1;
    for (int i = 0; i < FMT_COUNT; i++)
        if (!strcasecmp(dot + 1, fmt_names[i])) return i;
    return -1;
}

int io_fmt_from_text(const char *b, size_t n) {
    size_t i = 0;
    if (n >= 3 && !memcmp(b, "\xEF\xBB\xBF", 3)) i = 3;
    while (i < n && isspace((unsigned char)b[i])) i++;
    if (i < n && b[i] == '<') return FMT_XML;
    if (i < n && b[i] == '{') return FMT_JSON;
    size_t e = i;
    while (e < n && b[e] != '\n' && b[e] != '\r') e++;
    return memchr(b + i, '\t', e - i) ? FMT_TSV : FMT_CSV;
}

/* "AB12" -> 0-based row/col. */
static int parse_ref(const char *s, int *r, int *c) {
    int col = 0, row = 0, n = 0;
    const char *p = s;
    while (isalpha((unsigned char)*p)) {
        if (++n > 3) return 0;
        col = col * 26 + (toupper((unsigned char)*p) - 'A' + 1);
        p++;
    }
    if (!n || !isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) {
        row = row * 10 + (*p - '0');
        if (row > MAX_ROWS) return 0;
        p++;
    }
    if (*p || col < 1 || col > MAX_COLS || row < 1) return 0;
    *r = row - 1;
    *c = col - 1;
    return 1;
}

static int set_err(char *err, size_t errn, const char *msg) {
    snprintf(err, errn, "%s", msg);
    return -1;
}

/* Stores the field in buf at (r,c); a zero-length field leaves the cell empty. */
static int commit(Sheet *g, Buf *f, int r, int c) {
    if (!f->n) return 0;
    if (r >= MAX_ROWS || c >= MAX_COLS) return -1;
    bputc(f, '\0');
    sheet_set(g, r, c, f->s);
    f->n = 0;
    return 0;
}

/* Delimited text. With delim ' ' a run of spaces is one separator, blanks at the
   ends of a line are ignored, and a quoted empty field ("") still takes a column.
   inf: the current field has started (text or quotes), so it must be stored or
   counted even when it is empty. */
static int parse_delim(Sheet *g, const char *b, size_t n, char delim, char *err, size_t errn) {
    Buf f = {NULL, 0, 0};
    int r = 0, c = 0, inq = 0, rc = 0, inf = 0;
    int sp = delim == ' ';
    for (size_t i = 0; i < n && rc == 0; i++) {
        char ch = b[i];
        if (inq) {
            if (ch == '"') {
                if (i + 1 < n && b[i + 1] == '"') { bputc(&f, '"'); i++; }
                else inq = 0;
            } else bputc(&f, ch);
            continue;
        }
        if (ch == '"' && !inf) { inq = 1; inf = 1; continue; }
        if (ch == delim) {
            if (sp) {
                if (inf) {
                    if (commit(g, &f, r, c) < 0 || c + 1 >= MAX_COLS) rc = -1;
                    c++;
                    inf = 0;
                }
                continue;                      /* the rest of a run of spaces */
            }
            if (commit(g, &f, r, c) < 0 || c + 1 >= MAX_COLS) rc = -1;
            c++;
            inf = 0;
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && i + 1 < n && b[i + 1] == '\n') i++;
            if (commit(g, &f, r, c) < 0) rc = -1;
            r++;
            c = 0;
            inf = 0;
            if (r >= MAX_ROWS) rc = -1;
        } else {
            bputc(&f, ch);
            inf = 1;
        }
    }
    if (rc == 0 && inq) { set_err(err, errn, "unterminated quoted field"); rc = -1; }
    if (rc == 0 && (sp ? inf : (f.n || c > 0)) && commit(g, &f, r, c) < 0) rc = -1;
    buf_free(&f);
    if (rc) { if (!err[0]) set_err(err, errn, "data exceeds sheet limits"); return -1; }
    return 0;
}

/* Attribute lookup inside one tag: name="value". */
static int get_attr(const char *tag, const char *end, const char *name, char *out, size_t n) {
    size_t l = strlen(name);
    for (const char *p = tag; p + l + 2 <= end; p++) {
        if (!strncmp(p, name, l) && p[l] == '=' && p[l + 1] == '"' &&
            (p == tag || isspace((unsigned char)p[-1]))) {
            const char *v = p + l + 2;
            const char *q = memchr(v, '"', (size_t)(end - v));
            if (!q) return 0;
            size_t k = (size_t)(q - v);
            if (k >= n) k = n - 1;
            memcpy(out, v, k);
            out[k] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Cell text: drops inner tags, decodes the five XML entities. */
static char *xml_text(const char *s, size_t n) {
    Buf b = {NULL, 0, 0};
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '<') {
            while (i < n && s[i] != '>') i++;
            continue;
        }
        if (s[i] == '&') {
            static const struct { const char *ent; char ch; } ents[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            int hit = 0;
            for (size_t k = 0; k < sizeof ents / sizeof ents[0]; k++) {
                size_t l = strlen(ents[k].ent);
                if (i + l <= n && !strncmp(s + i, ents[k].ent, l)) {
                    bputc(&b, ents[k].ch);
                    i += l - 1;
                    hit = 1;
                    break;
                }
            }
            if (hit) continue;
        }
        bputc(&b, s[i]);
    }
    if (!b.s) return strdup("");
    return b.s;
}

static int parse_xml(Sheet *g, const char *b, char *err, size_t errn) {
    if (!strstr(b, "<sheet") || !strstr(b, "</sheet>"))
        return set_err(err, errn, "not a sheet XML document (missing <sheet> root)");
    const char *p = b;
    while ((p = strstr(p, "<c ")) != NULL) {
        const char *gt = strchr(p, '>');
        if (!gt) return set_err(err, errn, "unterminated <c> tag");
        char ref[32], f[8] = "", fmt[32] = "";
        int r, c;
        if (!get_attr(p, gt, "r", ref, sizeof ref) || !parse_ref(ref, &r, &c))
            return set_err(err, errn, "invalid cell reference in <c> tag");
        int isf = get_attr(p, gt, "f", f, sizeof f) && f[0] == '1';
        if (gt[-1] == '/') { p = gt + 1; continue; }     /* empty cell */
        const char *close = strstr(gt, "</c>");
        if (!close) return set_err(err, errn, "missing </c>");
        char *t = xml_text(gt + 1, (size_t)(close - gt - 1));
        if (isf) {
            size_t l = strlen(t);
            char *eq = malloc(l + 2);
            if (eq) { eq[0] = '='; memcpy(eq + 1, t, l + 1); free(t); t = eq; }
        }
        if (r < MAX_ROWS && c < MAX_COLS) {
            sheet_set(g, r, c, t);
            if (get_attr(p, gt, "s", fmt, sizeof fmt)) sheet_fmt_set(g, r, c, fmt);
        }
        free(t);
        p = close + 4;
    }
    /* merged blocks come after the cells, so they can empty the covered ones */
    p = b;
    while ((p = strstr(p, "<merge ")) != NULL) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char ref[48];
        int r1, c1, r2, c2;
        if (get_attr(p, gt, "ref", ref, sizeof ref)) {
            char *colon = strchr(ref, ':');
            if (colon) {
                *colon = '\0';
                if (parse_ref(ref, &r1, &c1) && parse_ref(colon + 1, &r2, &c2))
                    sheet_merge_add(g, r1, c1, r2, c2);
            }
        }
        p = gt + 1;
    }
    return 0;
}

static int parse_json(Sheet *g, const char *b, size_t n, char *err, size_t errn);

int io_parse(Sheet *g, const char *buf, size_t len, int fmt, char *err, size_t errn) {
    return io_parse_delim(g, buf, len, fmt, 0, err, errn);
}

/* delim 0 means the format default; it applies to CSV only (TSV is always tab) */
int io_parse_delim(Sheet *g, const char *buf, size_t len, int fmt, int delim, char *err, size_t errn) {
    err[0] = '\0';
    if (fmt == FMT_XLSX) return read_xlsx(g, buf, len, err, errn);     /* zip files: they have NUL bytes */
    if (fmt == FMT_ODS) return read_ods(g, buf, len, err, errn);
    if (memchr(buf, '\0', len)) return set_err(err, errn, "binary data (NUL byte) is not a sheet");
    switch (fmt) {
    case FMT_CSV: return parse_delim(g, buf, len, delim ? (char)delim : ',', err, errn);
    case FMT_TSV: return parse_delim(g, buf, len, '\t', err, errn);
    case FMT_XML: return parse_xml(g, buf, err, errn);
    case FMT_JSON: return parse_json(g, buf, len, err, errn);
    default:
        return set_err(err, errn, "unknown file format");
    }
}

/* ---------- XML and delimited writers ---------- */

static void xml_esc(Buf *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': bputs(b, "&amp;"); break;
        case '<': bputs(b, "&lt;"); break;
        case '>': bputs(b, "&gt;"); break;
        case '"': bputs(b, "&quot;"); break;
        case '\'': bputs(b, "&apos;"); break;
        default: bputc(b, *s);
        }
    }
}

static void build_delim(const Sheet *g, Buf *o, char delim, int crlf) {
    for (int r = 0; r <= g->max_r; r++) {
        for (int c = 0; c <= g->max_c; c++) {
            if (c) bputc(o, delim);
            const char *raw = sheet_get(g, r, c);
            if (!raw) { if (delim == ' ') bputs(o, "\"\""); continue; }
            if (strchr(raw, delim) || strpbrk(raw, "\"\r\n")) {
                bputc(o, '"');
                for (const char *p = raw; *p; p++) {
                    if (*p == '"') bputc(o, '"');
                    bputc(o, *p);
                }
                bputc(o, '"');
            } else bputs(o, raw);
        }
        if (crlf) bputc(o, '\r');
        bputc(o, '\n');
    }
}

static void cellref(char *out, size_t n, int r, int c) {
    char cn[8];
    col_name(c, cn);
    snprintf(out, n, "%s%d", cn, r + 1);
}

static void build_xml(const Sheet *g, const char *name, Buf *o) {
    bputs(o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sheet name=\"");
    xml_esc(o, name);
    bputs(o, "\">\n");
    char ref[24];
    for (int r = 0; r <= g->max_r; r++)
        for (int c = 0; c <= g->max_c; c++) {
            const char *raw = sheet_get(g, r, c);
            if (!raw) continue;
            cellref(ref, sizeof ref, r, c);
            bputs(o, "<c r=\"");
            bputs(o, ref);
            const char *fmt = sheet_fmt(g, r, c);
            if (fmt && *fmt) {
                bputs(o, "\" s=\"");
                xml_esc(o, fmt);
            }
            if (raw[0] == '=') {
                bputs(o, "\" f=\"1\">");
                xml_esc(o, raw + 1);
            } else {
                bputs(o, "\">");
                xml_esc(o, raw);
            }
            bputs(o, "</c>\n");
        }
    char a1[24], b1[24];
    for (int i = 0; i < g->nmg; i++) {
        const Merge *m = &g->mg[i];
        cellref(a1, sizeof a1, m->r1, m->c1);
        cellref(b1, sizeof b1, m->r2, m->c2);
        bputs(o, "<merge ref=\"");
        bputs(o, a1);
        bputs(o, ":");
        bputs(o, b1);
        bputs(o, "\"/>\n");
    }
    bputs(o, "</sheet>\n");
}

/* ---------- JSON (sparse) ----------
   {"sheet": "name",
    "cells": [{"ref": "A1", "v": "text or formula", "f": "format"}, ...],
    "merges": ["A1:B2", ...]}
   Only cells with a value or a format are written. "f" is the format string
   (bold, highlight, alignment, number or date format). */

static void json_str(Buf *o, const char *s) {
    bputc(o, '"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        switch (ch) {
        case '"':  bputs(o, "\\\""); break;
        case '\\': bputs(o, "\\\\"); break;
        case '\n': bputs(o, "\\n"); break;
        case '\r': bputs(o, "\\r"); break;
        case '\t': bputs(o, "\\t"); break;
        default:
            if (ch < 0x20) {
                char u[8];
                snprintf(u, sizeof u, "\\u%04x", ch);
                bputs(o, u);
            } else bputc(o, (char)ch);
        }
    }
    bputc(o, '"');
}

static void build_json(const Sheet *g, const char *name, Buf *o) {
    bputs(o, "{\n  \"sheet\": ");
    json_str(o, name);
    bputs(o, ",\n  \"cells\": [");
    int first = 1;
    char ref[24];
    for (int r = 0; r <= g->max_r; r++)
        for (int c = 0; c <= g->max_c; c++) {
            const Merge *m = sheet_merge_of(g, r, c);
            if (m && (m->r1 != r || m->c1 != c)) continue;      /* covered cells are empty */
            const char *raw = sheet_get_raw(g, r, c);
            const char *fmt = sheet_fmt(g, r, c);
            if (!raw && !fmt) continue;
            cellref(ref, sizeof ref, r, c);
            bputs(o, first ? "\n    {\"ref\": " : ",\n    {\"ref\": ");
            first = 0;
            json_str(o, ref);
            if (raw) { bputs(o, ", \"v\": "); json_str(o, raw); }
            if (fmt && *fmt) { bputs(o, ", \"f\": "); json_str(o, fmt); }
            bputc(o, '}');
        }
    bputs(o, "\n  ],\n  \"merges\": [");
    char a1[24], b1[24];
    for (int i = 0; i < g->nmg; i++) {
        const Merge *m = &g->mg[i];
        cellref(a1, sizeof a1, m->r1, m->c1);
        cellref(b1, sizeof b1, m->r2, m->c2);
        char both[56];
        snprintf(both, sizeof both, "%s:%s", a1, b1);
        bputs(o, i ? ", " : "");
        json_str(o, both);
    }
    bputs(o, "]\n}\n");
}

/* Reads one JSON string at *pp into out (appended). */
static int jstr(const char **pp, const char *end, Buf *out) {
    const char *p = *pp;
    if (p >= end || *p != '"') return -1;
    p++;
    while (p < end && *p != '"') {
        if (*p != '\\') { bputc(out, *p++); continue; }
        p++;
        if (p >= end) return -1;
        switch (*p) {
        case '"': case '\\': case '/': bputc(out, *p); break;
        case 'n': bputc(out, '\n'); break;
        case 't': bputc(out, '\t'); break;
        case 'r': bputc(out, '\r'); break;
        case 'b': bputc(out, '\b'); break;
        case 'f': bputc(out, '\f'); break;
        case 'u': {
            if (end - p < 5) return -1;
            unsigned cp = 0;
            for (int k = 1; k <= 4; k++) {
                char h = p[k];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                else return -1;
            }
            p += 4;
            if (cp < 0x80) bputc(out, (char)cp);
            else if (cp < 0x800) {
                bputc(out, (char)(0xC0 | (cp >> 6)));
                bputc(out, (char)(0x80 | (cp & 0x3F)));
            } else {
                bputc(out, (char)(0xE0 | (cp >> 12)));
                bputc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
                bputc(out, (char)(0x80 | (cp & 0x3F)));
            }
            break;
        }
        default: return -1;
        }
        p++;
    }
    if (p >= end) return -1;
    *pp = p + 1;
    return 0;
}

static const char *jws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static int json_fail(char *err, size_t errn, const char *msg) {
    snprintf(err, errn, "%s", msg);
    return -1;
}

static int parse_cells_json(Sheet *g, const char **pp, const char *end, char *err, size_t errn) {
    const char *p = jws(*pp, end);
    if (p >= end || *p != '[') return json_fail(err, errn, "\"cells\" must be an array");
    p++;
    for (;;) {
        p = jws(p, end);
        if (p < end && *p == ']') { p++; break; }
        if (p >= end || *p != '{') return json_fail(err, errn, "a cell must be an object");
        p++;
        Buf ref = {NULL, 0, 0}, v = {NULL, 0, 0}, f = {NULL, 0, 0};
        for (;;) {
            p = jws(p, end);
            if (p < end && *p == '}') { p++; break; }
            Buf key = {NULL, 0, 0};
            if (jstr(&p, end, &key) < 0) { buf_free(&key); return json_fail(err, errn, "bad key in a cell"); }
            p = jws(p, end);
            if (p >= end || *p != ':') { buf_free(&key); return json_fail(err, errn, "expected ':' in a cell"); }
            p = jws(p + 1, end);
            Buf *dst = NULL;
            if (key.s && !strcmp(key.s, "ref")) dst = &ref;
            else if (key.s && !strcmp(key.s, "v")) dst = &v;
            else if (key.s && !strcmp(key.s, "f")) dst = &f;
            Buf skip = {NULL, 0, 0};
            if (jstr(&p, end, dst ? dst : &skip) < 0) {
                buf_free(&key); buf_free(&skip);
                return json_fail(err, errn, "cell values must be strings");
            }
            buf_free(&skip);
            buf_free(&key);
            p = jws(p, end);
            if (p < end && *p == ',') p++;
        }
        int r, c;
        if (!ref.s || !parse_ref(ref.s, &r, &c)) {
            buf_free(&ref); buf_free(&v); buf_free(&f);
            return json_fail(err, errn, "a cell has a bad \"ref\"");
        }
        if (v.s && v.n) sheet_put_raw(g, r, c, v.s);
        if (f.s && f.n) sheet_put_fmt(g, r, c, f.s);
        buf_free(&ref); buf_free(&v); buf_free(&f);
        p = jws(p, end);
        if (p < end && *p == ',') p++;
    }
    *pp = p;
    return 0;
}

static int parse_merges_json(Sheet *g, const char **pp, const char *end, char *err, size_t errn) {
    const char *p = jws(*pp, end);
    if (p >= end || *p != '[') return json_fail(err, errn, "\"merges\" must be an array");
    p++;
    for (;;) {
        p = jws(p, end);
        if (p < end && *p == ']') { p++; break; }
        Buf s = {NULL, 0, 0};
        if (jstr(&p, end, &s) < 0) { buf_free(&s); return json_fail(err, errn, "merges must be strings"); }
        char *colon = s.s ? strchr(s.s, ':') : NULL;
        int r1, c1, r2, c2;
        if (!colon) { buf_free(&s); return json_fail(err, errn, "a merge needs \"A1:B2\""); }
        *colon = '\0';
        if (!parse_ref(s.s, &r1, &c1) || !parse_ref(colon + 1, &r2, &c2)) {
            buf_free(&s);
            return json_fail(err, errn, "a merge has a bad reference");
        }
        sheet_merge_add(g, r1, c1, r2, c2);
        buf_free(&s);
        p = jws(p, end);
        if (p < end && *p == ',') p++;
    }
    *pp = p;
    return 0;
}

static int parse_json(Sheet *g, const char *b, size_t n, char *err, size_t errn) {
    const char *p = jws(b, b + n), *end = b + n;
    if (p >= end || *p != '{') return json_fail(err, errn, "not a JSON sheet (no opening brace)");
    p++;
    for (;;) {
        p = jws(p, end);
        if (p < end && *p == '}') break;
        Buf key = {NULL, 0, 0};
        if (jstr(&p, end, &key) < 0) { buf_free(&key); return json_fail(err, errn, "bad key"); }
        p = jws(p, end);
        if (p >= end || *p != ':') { buf_free(&key); return json_fail(err, errn, "expected ':'"); }
        p = jws(p + 1, end);
        int rc = 0;
        if (key.s && !strcmp(key.s, "cells")) rc = parse_cells_json(g, &p, end, err, errn);
        else if (key.s && !strcmp(key.s, "merges")) rc = parse_merges_json(g, &p, end, err, errn);
        else {                                       /* other string values, e.g. "sheet" */
            Buf skip = {NULL, 0, 0};
            if (jstr(&p, end, &skip) < 0) rc = json_fail(err, errn, "unsupported value");
            buf_free(&skip);
        }
        buf_free(&key);
        if (rc < 0) return -1;
        p = jws(p, end);
        if (p < end && *p == ',') p++;
    }
    return 0;
}

/* ---------- zip (stored entries) ---------- */

typedef struct { const char *name; Buf data; } Part;

static uint32_t crc32_of(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void le16(Buf *b, uint16_t v) { char x[2] = {(char)v, (char)(v >> 8)}; buf_put(b, x, 2); }
static void le32(Buf *b, uint32_t v) {
    char x[4] = {(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)};
    buf_put(b, x, 4);
}

static void zip_build(Buf *out, Part *parts, int np) {
    uint32_t *off = calloc((size_t)np, sizeof *off), *crc = calloc((size_t)np, sizeof *crc);
    if (!off || !crc) { free(off); free(crc); return; }
    for (int i = 0; i < np; i++) {
        size_t nl = strlen(parts[i].name);
        off[i] = (uint32_t)out->n;
        crc[i] = crc32_of((const unsigned char *)parts[i].data.s, parts[i].data.n);
        le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 0);
        le16(out, 0); le16(out, 0x21);                      /* time 0, date 1980-01-01 */
        le32(out, crc[i]); le32(out, (uint32_t)parts[i].data.n); le32(out, (uint32_t)parts[i].data.n);
        le16(out, (uint16_t)nl); le16(out, 0);
        buf_put(out, parts[i].name, nl);
        buf_put(out, parts[i].data.s ? parts[i].data.s : "", parts[i].data.n);
    }
    uint32_t cd = (uint32_t)out->n;
    for (int i = 0; i < np; i++) {
        size_t nl = strlen(parts[i].name);
        le32(out, 0x02014b50); le16(out, 20); le16(out, 20); le16(out, 0); le16(out, 0);
        le16(out, 0); le16(out, 0x21);
        le32(out, crc[i]); le32(out, (uint32_t)parts[i].data.n); le32(out, (uint32_t)parts[i].data.n);
        le16(out, (uint16_t)nl); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
        le32(out, 0); le32(out, off[i]);
        buf_put(out, parts[i].name, nl);
    }
    uint32_t cdsize = (uint32_t)out->n - cd;
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0);
    le16(out, (uint16_t)np); le16(out, (uint16_t)np);
    le32(out, cdsize); le32(out, cd); le16(out, 0);
    free(off);
    free(crc);
}

/* ---------- xlsx ---------- */

static void sheet_name_clean(const char *in, char *out, size_t n) {
    size_t k = 0;
    for (; *in && k + 1 < n && k < 31; in++)
        if (!strchr("[]:*?/\\", *in)) out[k++] = *in;
    out[k] = '\0';
    if (!k) strcpy(out, "Sheet1");
}

static void num_str(char *out, size_t n, double v) { snprintf(out, n, "%.15g", v); }

static void build_xlsx(const Sheet *g, const char *name, Part *p) {
    static const char *ct =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "</Types>";
    static const char *rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>";
    static const char *wbrels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "</Relationships>";
    char clean[40];
    sheet_name_clean(name, clean, sizeof clean);

    Buf wb = {NULL, 0, 0};
    bputs(&wb, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
               "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
               "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
               "<sheets><sheet name=\"");
    xml_esc(&wb, clean);
    bputs(&wb, "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");

    Buf ws = {NULL, 0, 0};
    bputs(&ws, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
               "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>\n");
    char ref[24], num[32];
    for (int r = 0; r <= g->max_r; r++) {
        int any = 0;
        for (int c = 0; c <= g->max_c && !any; c++) any = sheet_get_raw(g, r, c) != NULL;
        if (!any) continue;
        bputs(&ws, "<row r=\"");
        snprintf(num, sizeof num, "%d", r + 1);
        bputs(&ws, num);
        bputs(&ws, "\">");
        for (int c = 0; c <= g->max_c; c++) {
            const char *raw = sheet_get_raw(g, r, c);     /* a merged block keeps its value in its top-left cell */
            if (!raw) continue;
            cellref(ref, sizeof ref, r, c);
            double v;
            int st = cell_num(g, r, c, 0, &v);
            bputs(&ws, "<c r=\""); bputs(&ws, ref);
            if (raw[0] == '=') {
                bputs(&ws, "\"><f>");
                xml_esc(&ws, raw + 1);
                bputs(&ws, "</f>");
                if (st == 0) { num_str(num, sizeof num, v); bputs(&ws, "<v>"); bputs(&ws, num); bputs(&ws, "</v>"); }
                bputs(&ws, "</c>");
            } else if (st == 0) {
                num_str(num, sizeof num, v);
                bputs(&ws, "\"><v>"); bputs(&ws, num); bputs(&ws, "</v></c>");
            } else {
                bputs(&ws, "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">");
                xml_esc(&ws, raw);
                bputs(&ws, "</t></is></c>");
            }
        }
        bputs(&ws, "</row>\n");
    }
    bputs(&ws, "</sheetData>");
    if (g->nmg) {
        snprintf(num, sizeof num, "%d", g->nmg);
        bputs(&ws, "<mergeCells count=\""); bputs(&ws, num); bputs(&ws, "\">");
        for (int i = 0; i < g->nmg; i++) {
            const Merge *m = &g->mg[i];
            char a1[24], a2[24];
            cellref(a1, sizeof a1, m->r1, m->c1);
            cellref(a2, sizeof a2, m->r2, m->c2);
            bputs(&ws, "<mergeCell ref=\""); bputs(&ws, a1); bputs(&ws, ":"); bputs(&ws, a2); bputs(&ws, "\"/>");
        }
        bputs(&ws, "</mergeCells>");
    }
    bputs(&ws, "</worksheet>");

    p[0].name = "[Content_Types].xml"; p[0].data = (Buf){(char *)ct, strlen(ct), 0};
    p[1].name = "_rels/.rels";         p[1].data = (Buf){(char *)rels, strlen(rels), 0};
    p[2].name = "xl/workbook.xml";     p[2].data = wb;
    p[3].name = "xl/_rels/workbook.xml.rels"; p[3].data = (Buf){(char *)wbrels, strlen(wbrels), 0};
    p[4].name = "xl/worksheets/sheet1.xml";   p[4].data = ws;
}

/* ---------- ods ---------- */

/* Length of an A1-style reference at p (with optional $ anchors), 0 if none. */
static size_t ref_span(const char *p) {
    const char *q = p;
    int letters = 0;
    if (*q == '$') q++;
    while (isalpha((unsigned char)*q)) { q++; letters++; }
    if (!letters || letters > 3) return 0;
    if (*q == '$') q++;
    if (!isdigit((unsigned char)*q)) return 0;
    while (isdigit((unsigned char)*q)) q++;
    if (isalnum((unsigned char)*q) || *q == '(' || *q == '_') return 0;
    return (size_t)(q - p);
}

/* Converts A1 references to ODS form [.A1]; ranges become [.A1:.B2]. */
static void ods_formula(Buf *o, const char *f) {
    const char *p = f;
    char prev = 0;
    while (*p) {
        size_t l = 0;
        if (!isalnum((unsigned char)prev) && prev != '_' && (isalpha((unsigned char)*p) || *p == '$'))
            l = ref_span(p);
        if (!l) {
            bputc(o, *p);
            prev = *p++;
            continue;
        }
        bputs(o, "[.");
        buf_put(o, p, l);
        p += l;
        if (*p == ':') {
            size_t l2 = (isalpha((unsigned char)p[1]) || p[1] == '$') ? ref_span(p + 1) : 0;
            if (l2) {
                bputs(o, ":.");
                buf_put(o, p + 1, l2);
                p += 1 + l2;
            }
        }
        bputc(o, ']');
        prev = p[-1];
    }
}

static void build_ods(const Sheet *g, const char *name, Part *p) {
    static const char *mime = "application/vnd.oasis.opendocument.spreadsheet";
    static const char *manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "</manifest:manifest>";
    char clean[40];
    sheet_name_clean(name, clean, sizeof clean);
    Buf c = {NULL, 0, 0};
    bputs(&c, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
              "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
              "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" office:version=\"1.2\">"
              "<office:body><office:spreadsheet><table:table table:name=\"");
    xml_esc(&c, clean);
    bputs(&c, "\">\n");
    int cols = g->max_c + 1;
    char num[32];
    for (int r = 0; r <= g->max_r; r++) {
        bputs(&c, "<table:table-row>");
        for (int col = 0; col < cols; col++) {
            const Merge *m = sheet_merge_of(g, r, col);
            if (m && (m->r1 != r || m->c1 != col)) { bputs(&c, "<table:covered-table-cell/>"); continue; }
            char sp[96] = "";                    /* spans of a merged block, on its top-left cell */
            if (m) snprintf(sp, sizeof sp, " table:number-columns-spanned=\"%d\" table:number-rows-spanned=\"%d\"",
                            m->c2 - m->c1 + 1, m->r2 - m->r1 + 1);
            const char *raw = sheet_get_raw(g, r, col);
            if (!raw) { bputs(&c, "<table:table-cell"); bputs(&c, sp); bputs(&c, "/>"); continue; }
            double v;
            int st = cell_num(g, r, col, 0, &v);
            if (raw[0] == '=') {
                bputs(&c, "<table:table-cell"); bputs(&c, sp); bputs(&c, " table:formula=\"of:=");
                Buf fx = {NULL, 0, 0};
                ods_formula(&fx, raw + 1);
                xml_esc(&c, fx.s ? fx.s : "");
                buf_free(&fx);
                bputs(&c, "\"");
                if (st == 0) {
                    num_str(num, sizeof num, v);
                    bputs(&c, " office:value-type=\"float\" office:value=\""); bputs(&c, num);
                    bputs(&c, "\"><text:p>"); bputs(&c, num); bputs(&c, "</text:p></table:table-cell>");
                } else bputs(&c, "/>");
            } else if (st == 0) {
                num_str(num, sizeof num, v);
                bputs(&c, "<table:table-cell"); bputs(&c, sp);
                bputs(&c, " office:value-type=\"float\" office:value=\"");
                bputs(&c, num); bputs(&c, "\"><text:p>"); xml_esc(&c, raw); bputs(&c, "</text:p></table:table-cell>");
            } else {
                bputs(&c, "<table:table-cell"); bputs(&c, sp);
                bputs(&c, " office:value-type=\"string\"><text:p>");
                xml_esc(&c, raw);
                bputs(&c, "</text:p></table:table-cell>");
            }
        }
        bputs(&c, "</table:table-row>\n");
    }
    bputs(&c, "</table:table></office:spreadsheet></office:body></office:document-content>\n");

    p[0].name = "mimetype";                 p[0].data = (Buf){(char *)mime, strlen(mime), 0};
    p[1].name = "META-INF/manifest.xml";    p[1].data = (Buf){(char *)manifest, strlen(manifest), 0};
    p[2].name = "content.xml";              p[2].data = c;
}

/* ---------- saving ---------- */

static int write_file(const char *path, const Buf *b, char *err, size_t errn) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { snprintf(err, errn, "cannot write %s: %s", path, strerror(errno)); return -1; }
    size_t off = 0;
    while (off < b->n) {
        ssize_t k = write(fd, b->s + off, b->n - off);
        if (k < 0) {
            if (errno == EINTR) continue;
            snprintf(err, errn, "write failed: %s", strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)k;
    }
    if (close(fd) < 0 || rename(tmp, path) < 0) {
        snprintf(err, errn, "cannot finish %s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int io_save(const Sheet *g, const char *name, const char *path, int fmt, const ExpOpt *o, char *err, size_t errn) {
    Buf out = {NULL, 0, 0};
    int rc;
    ExpOpt def = {',', 0, 0};
    if (!o) o = &def;
    err[0] = '\0';
    if (fmt == FMT_CSV) build_delim(g, &out, o->delim ? (char)o->delim : ',', o->crlf);
    else if (fmt == FMT_TSV) build_delim(g, &out, '\t', o->crlf);
    else if (fmt == FMT_XML) build_xml(g, name, &out);
    else if (fmt == FMT_JSON) build_json(g, name, &out);
    else {
        Part parts[5];
        int np = fmt == FMT_XLSX ? 5 : 3;
        if (fmt == FMT_XLSX) build_xlsx(g, name, parts);
        else build_ods(g, name, parts);
        zip_build(&out, parts, np);
        for (int i = 0; i < np; i++) if (parts[i].data.s && parts[i].data.cap) free(parts[i].data.s);
        /* the static parts above are not heap-owned; free only the generated ones */
    }
    rc = write_file(path, &out, err, errn);
    buf_free(&out);
    return rc;
}
