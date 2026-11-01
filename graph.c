/* Graphs: writes the selected areas as gnuplot data and a script, renders a PDF
   with gnuplot's pdf terminal, and opens it with xdg-open. The leftmost selected
   column is X; every other selected column is a series, and the selection need
   not be a rectangle. A text cell heading a series column is its name. All
   graph settings come from the command text (';' separated), which is kept in
   ~/.config/tsheet/plot for editing by hand. External tools: gnuplot, xdg-open. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "tsheet.h"

static void cache_dir(char *out, size_t n) {
    const char *x = getenv("XDG_CACHE_HOME");
    const char *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/tsheet", x);
    else snprintf(out, n, "%s/.cache/tsheet", h && *h ? h : ".");
}

/* mkdir -p */
static int make_dirs(const char *dir) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* Keeps letters, digits, '_' and '-' (and blanks and some punctuation if keep).
   Blanks at the ends are dropped (" energy" from a CSV header is "energy"). */
static void safe_text(char *out, size_t n, const char *in, int keep) {
    size_t k = 0;
    while (*in == ' ' || *in == '\t') in++;
    for (; *in && k + 1 < n; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isalnum(ch) || ch == '_' || ch == '-' ||
            (keep && (ch == ' ' || ch == '.' || ch == ',' || ch == '/')))
            out[k++] = (char)ch;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

/* ---------- the plot file: ~/.config/tsheet/plot ----------
   The graph commands, one gnuplot command per line. tsheet reads it at start,
   before each graph and when the graph options open, and writes it whenever
   the options change, so the file and the options editor always agree. */

static const char plot_header[] =
    "# tsheet plot command: the gnuplot script run by Graph > Draw (F9).\n"
    "# One gnuplot command per line; lines starting with # are comments. Edit freely:\n"
    "# tsheet rereads this file before every graph and when Graph options opens.\n"
    "# (Comments other than this header are not kept when the options editor saves.)\n"
    "#\n"
    "# set ... lines run first, in order; plot/splot/replot lines run last.\n"
    "# Placeholders, replaced when a graph is drawn:\n"
    "#   @SERIES  every selected series: 'data' using 1:2 title 'Name', ...\n"
    "#   @DATA    the data file, quoted; column 1 is X, columns 2.. are the series\n"
    "#   @N       the number of series\n"
    "#   @XNAME   the header of the X column (empty if it has none)\n"
    "# Example: plot for [i=2:@N+1] @DATA using 1:i with linespoints title columnhead(i)\n"
    "# (no plot line at all means: plot @SERIES)\n"
    "\n";

static void config_dir(char *out, size_t n) {
    const char *x = getenv("XDG_CONFIG_HOME");
    const char *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/tsheet", x);
    else snprintf(out, n, "%s/.config/tsheet", h && *h ? h : ".");
}

void plot_file_path(char *out, size_t n) {
    char dir[2048];
    config_dir(dir, sizeof dir);
    snprintf(out, n, "%s/plot", dir);
}

/* The file as ';'-separated commands. A line ending in \ continues on the next. */
int plot_file_read(char *cmds, size_t n) {
    char path[2200];
    plot_file_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t l = 0;
    char line[4096];
    int cont = 0;
    cmds[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        size_t k = strlen(line);
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r')) line[--k] = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!cont && (*p == '#' || !*p)) continue;
        k = strlen(p);
        int more = k && p[k - 1] == '\\';
        if (more) p[--k] = '\0';
        while (k && (isspace((unsigned char)p[k - 1]) || p[k - 1] == ';')) p[--k] = '\0';
        if (k) {
            int w = snprintf(cmds + l, n - l, "%s%s", l ? (cont ? " " : "; ") : "", p);
            if (w < 0 || (size_t)w >= n - l) break;
            l += (size_t)w;
        }
        cont = more;
    }
    fclose(f);
    return 0;
}

/* Writes the commands back, one per line, under the header. */
int plot_file_write(const char *cmds) {
    char dir[2048], path[2200], tmp[2300];
    config_dir(dir, sizeof dir);
    if (make_dirs(dir) < 0) return -1;
    plot_file_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs(plot_header, f);
    const char *p = cmds;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *q = p;
        while (len && isspace((unsigned char)*q)) { q++; len--; }
        while (len && isspace((unsigned char)q[len - 1])) len--;
        if (len) fprintf(f, "%.*s\n", (int)len, q);
        if (!e) break;
        p = e + 1;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* ---------- data: one or more selected areas ----------
   Every selected column is taken on its own: its selected cells, top to
   bottom. The leftmost column is X; the others are series. Leading blanks are
   skipped and a text cell first in a series column is its name (the X column's
   first text cell is a name only when some series has one). When every series
   cell lies on a row the X column also covers, values pair by sheet row (an L
   shape, a few columns picked with Ctrl+click); otherwise they pair by position
   (A1:A10 against C5:C14). A single column is plotted against 1, 2, 3, ... */

typedef struct { int c, *r, n, hdr; } GCol;

static int in_rects(const Rect *q, int nq, int r, int c) {
    for (int i = 0; i < nq; i++) if (rect_has(&q[i], r, c)) return 1;
    return 0;
}

static int has_row(const GCol *g, int r) {
    int lo = 0, hi = g->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g->r[m] == r) return 1;
        if (g->r[m] < r) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

static void put_val(FILE *f, const Sheet *g, int r, int c) {
    double v;
    if (r >= 0 && cell_num(g, r, c, 0, &v) == 0) fprintf(f, " %.17g", v);
    else fputs(" ?", f);
}

static int is_plot_cmd(const char *t) {
    static const char *const w[] = {"plot", "splot", "replot", NULL};
    for (int i = 0; w[i]; i++) {
        size_t l = strlen(w[i]);
        if (!strncmp(t, w[i], l) && (!t[l] || isspace((unsigned char)t[l]))) return 1;
    }
    return 0;
}

/* Writes one command with the placeholders filled in. */
static void put_cmd(FILE *f, const char *t, size_t len, const char *series,
                    const char *data, int nser, const char *xname) {
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '@') {
            const char *rest = t + i + 1;
            size_t left = len - i - 1;
            if (left >= 6 && !strncmp(rest, "SERIES", 6)) { fputs(series, f); i += 6; continue; }
            if (left >= 5 && !strncmp(rest, "XNAME", 5)) { fputs(xname, f); i += 5; continue; }
            if (left >= 4 && !strncmp(rest, "DATA", 4)) { fprintf(f, "'%s'", data); i += 4; continue; }
            if (left >= 1 && rest[0] == 'N' && (left == 1 || !isalnum((unsigned char)rest[1]))) {
                fprintf(f, "%d", nser);
                i += 1;
                continue;
            }
        }
        fputc(t[i], f);
    }
    fputc('\n', f);
}

/* pass 0: every command that is not a plot; pass 1: the plot commands */
static int write_commands(FILE *f, const char *cmds, int pass, const char *series,
                          const char *data, int nser, const char *xname) {
    int any = 0;
    const char *p = cmds;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *q = p;
        while (len && isspace((unsigned char)*q)) { q++; len--; }
        while (len && isspace((unsigned char)q[len - 1])) len--;
        if (len) {
            char head[16];
            snprintf(head, sizeof head, "%.*s", (int)(len < 15 ? len : 15), q);
            if (is_plot_cmd(head) == pass) {
                put_cmd(f, q, len, series, data, nser, xname);
                any = 1;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return any;
}

static void gcols_free(GCol *gc, int n) {
    for (int i = 0; i < n; i++) free(gc[i].r);
    free(gc);
}

int graph_draw(const Doc *d, const Rect *q, int nq, const char *cmds,
               const char *base_name, char *pdf, size_t pdfn, char *msg, size_t msgn, int view) {
    const Sheet *g = d->g;
    /* the selected columns, left to right, each with its selected rows */
    unsigned char *cm = calloc(MAX_COLS, 1);
    if (!cm) { snprintf(msg, msgn, "out of memory"); return -1; }
    int rmin = MAX_ROWS, rmax = -1, ncol = 0;
    for (int i = 0; i < nq; i++) {
        for (int c = q[i].c1; c <= q[i].c2 && c < MAX_COLS; c++) if (!cm[c]++) ncol++;
        if (q[i].r1 < rmin) rmin = q[i].r1;
        if (q[i].r2 > rmax) rmax = q[i].r2;
    }
    GCol *gc = calloc((size_t)(ncol ? ncol : 1), sizeof *gc);
    if (!gc) { free(cm); snprintf(msg, msgn, "out of memory"); return -1; }
    int k = 0;
    for (int c = 0; c < MAX_COLS && k < ncol; c++) {
        if (!cm[c]) continue;
        GCol *x = &gc[k++];
        x->c = c;
        x->r = malloc((size_t)(rmax - rmin + 1 > 0 ? rmax - rmin + 1 : 1) * sizeof *x->r);
        if (!x->r) { free(cm); gcols_free(gc, k); snprintf(msg, msgn, "out of memory"); return -1; }
        for (int r = rmin; r <= rmax; r++) {
            if (!in_rects(q, nq, r, c)) continue;
            const Merge *m = sheet_merge_of(g, r, c);
            if (m && (m->r1 != r || m->c1 != c)) continue;      /* covered by a merge */
            x->r[x->n++] = r;
        }
    }
    free(cm);
    /* leading blanks out; a first text cell is a name */
    int any_hdr = 0;
    for (int i = 0; i < ncol; i++) {
        GCol *x = &gc[i];
        double v;
        int s = 0;
        while (s < x->n && cell_num(g, x->r[s], x->c, 0, &v) == 3) s++;
        if (s < x->n) {
            int st = cell_num(g, x->r[s], x->c, 0, &v);
            x->hdr = (st == 1 || st == 2) ? x->r[s] : -1;
        } else x->hdr = -1;
        if (i > 0 && x->hdr >= 0) any_hdr = 1;
        memmove(x->r, x->r + s, (size_t)(x->n - s) * sizeof *x->r);
        x->n -= s;
    }
    int noX = ncol == 1;
    GCol *X = noX ? NULL : &gc[0];
    GCol *S = noX ? gc : gc + 1;
    int nser = noX ? 1 : ncol - 1;
    if (X && X->hdr >= 0 && !any_hdr) X->hdr = -1;           /* text X values, not a name */
    for (int i = 0; i < ncol; i++)
        if (gc[i].hdr >= 0 && gc[i].n && gc[i].r[0] == gc[i].hdr) {
            memmove(gc[i].r, gc[i].r + 1, (size_t)(gc[i].n - 1) * sizeof *gc[i].r);
            gc[i].n--;
        }
    int maxn = 0;
    for (int i = 0; i < nser; i++) if (S[i].n > maxn) maxn = S[i].n;
    if (!ncol || !maxn) {
        gcols_free(gc, ncol);
        snprintf(msg, msgn, "graph needs at least one data cell under the headers");
        return -1;
    }
    /* pair by row when X covers every series row */
    int byrow = X != NULL;
    for (int i = 0; byrow && i < nser; i++)
        for (int j = 0; byrow && j < S[i].n; j++)
            if (!has_row(X, S[i].r[j])) byrow = 0;
    /* X: numbers, time, or text (then 1, 2, 3, ...) */
    int tx = 0, timex = 0;
    if (X) {
        for (int j = 0; j < X->n && !tx; j++) {
            double v;
            int st = cell_num(g, X->r[j], X->c, 0, &v);
            if (st != 0 && st != 3) tx = 1;
        }
        timex = !tx && X->n && cell_kind(g, X->r[0], X->c) == KIND_TIME;
    }

    char dir[2048], base[160] = "", dat[2200], gp[2200], log[2200];
    cache_dir(dir, sizeof dir);
    if (make_dirs(dir) < 0) {
        gcols_free(gc, ncol);
        snprintf(msg, msgn, "cannot create %s", dir);
        return -1;
    }
    safe_text(base, sizeof base, base_name ? base_name : "graph", 0);
    if (!base[0]) snprintf(base, sizeof base, "graph");
    snprintf(dat, sizeof dat, "%s/%s-graph.dat", dir, base);
    snprintf(gp, sizeof gp, "%s/%s-graph.gp", dir, base);
    snprintf(log, sizeof log, "%s/%s-graph.log", dir, base);
    snprintf(pdf, pdfn, "%s/%s-graph.pdf", dir, base);

    FILE *f = fopen(dat, "w");
    if (!f) { gcols_free(gc, ncol); snprintf(msg, msgn, "cannot write %s", dat); return -1; }
    int rows = byrow ? X->n : (X && X->n > maxn ? X->n : maxn);
    int *pos = calloc((size_t)nser, sizeof *pos);           /* by row: next index per series */
    for (int j = 0; j < rows; j++) {
        if (!X || tx) fprintf(f, "%d", j + 1);
        else {
            double v;
            int r = j < X->n ? X->r[j] : -1;
            if (r >= 0 && cell_num(g, r, X->c, 0, &v) == 0) fprintf(f, "%.17g", v);
            else fputc('?', f);
        }
        for (int i = 0; i < nser; i++) {
            int r = -1;
            if (byrow) {
                int row = X->r[j];
                while (pos && pos[i] < S[i].n && S[i].r[pos[i]] < row) pos[i]++;
                if (pos && pos[i] < S[i].n && S[i].r[pos[i]] == row) r = row;
            } else if (j < S[i].n) r = S[i].r[j];
            put_val(f, g, r, S[i].c);
        }
        fputc('\n', f);
    }
    free(pos);
    fclose(f);

    /* the default plot: every series, named by its header */
    size_t sl = 0, scap = (size_t)nser * 2400 + 1;
    char *series = malloc(scap);
    if (!series) { gcols_free(gc, ncol); snprintf(msg, msgn, "out of memory"); return -1; }
    series[0] = '\0';
    for (int i = 0; i < nser; i++) {
        char name[160] = "";
        if (S[i].hdr >= 0) {
            const char *t = sheet_get(g, S[i].hdr, S[i].c);
            safe_text(name, sizeof name, t ? t : "", 1);
        }
        if (!name[0]) { col_name(S[i].c, name); }
        sl += (size_t)snprintf(series + sl, scap - sl, "%s'%s' using 1:%d title '%s'",
                               i ? ", " : "", dat, i + 2, name);
    }
    char xname[160] = "";
    if (X && X->hdr >= 0) {
        const char *t = sheet_get(g, X->hdr, X->c);
        safe_text(xname, sizeof xname, t ? t : "", 1);
    }
    gcols_free(gc, ncol);

    f = fopen(gp, "w");
    if (!f) {
        free(series);
        snprintf(msg, msgn, "cannot write %s", gp);
        return -1;
    }
    /* a pdf file, or (view) gnuplot's own interactive window: no terminal line */
    if (!view) {
        fprintf(f, "set terminal pdf size 9,5.5 font 'Sans,10'\n");
        fprintf(f, "set output '%s'\n", pdf);
    }
    fprintf(f, "set datafile missing '?'\n");
    if (timex) fprintf(f, "set xdata time\nset timefmt '%%s'\nset format x '%%Y-%%m-%%d'\n");
    const char *c0 = cmds ? cmds : "";
    write_commands(f, c0, 0, series, dat, nser, xname);       /* settings, in order */
    if (!write_commands(f, c0, 1, series, dat, nser, xname))  /* then the plot */
        fprintf(f, "plot %s\n", series);
    free(series);
    fclose(f);

    if (view) {
        /* gnuplot -persist keeps its window open after the script ends; the
           double fork leaves it to init, so tsheet does not wait for it */
        if (system("command -v gnuplot >/dev/null 2>&1") != 0) {
            snprintf(msg, msgn, "gnuplot is not installed");
            return -1;
        }
        pid_t p = fork();
        if (p == 0) {
            pid_t gc = fork();
            if (gc == 0) {
                int fd = open("/dev/null", O_RDWR);
                if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
                setsid();
                execlp("gnuplot", "gnuplot", "-persist", gp, (char *)NULL);
                _exit(127);
            }
            _exit(0);
        }
        if (p > 0) waitpid(p, NULL, 0);
        snprintf(msg, msgn, "graph opened in a gnuplot window");
        return 0;
    }

    /* run gnuplot quietly; its messages go to the log */
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "gnuplot '%s' >'%s' 2>&1", gp, log);
    int rc = system(cmd);
    if (rc != 0) {
        if (WIFEXITED(rc) && WEXITSTATUS(rc) == 127)
            snprintf(msg, msgn, "gnuplot is not installed");
        else
            snprintf(msg, msgn, "gnuplot failed, see %s", log);
        return -1;
    }
    snprintf(msg, msgn, "graph written: %s", pdf);
    return 0;
}

/* Opens a file with the desktop default via xdg-open. Double fork: the
   grandchild is reparented to init, so nothing has to reap it. */
void graph_open(const char *path) {
    pid_t p = fork();
    if (p == 0) {
        pid_t g = fork();
        if (g == 0) {
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) {
                dup2(fd, 0);
                dup2(fd, 1);
                dup2(fd, 2);
            }
            setsid();
            execlp("xdg-open", "xdg-open", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}
