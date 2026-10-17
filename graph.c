/* Graphs: writes the selected block as gnuplot data and a script, renders a PDF
   with gnuplot's pdf terminal, and opens it with xdg-open. The first column is X;
   every other column is a series. A first row with text in the series columns
   is taken as headers (series names). All graph settings come from the command
   text (';' separated), so every option is visible there. External tools:
   gnuplot and xdg-open. */
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

/* Keeps letters, digits, '_' and '-' (and blanks and some punctuation if keep). */
static void safe_text(char *out, size_t n, const char *in, int keep) {
    size_t k = 0;
    for (; *in && k + 1 < n; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isalnum(ch) || ch == '_' || ch == '-' ||
            (keep && (ch == ' ' || ch == '.' || ch == ',' || ch == '/')))
            out[k++] = (char)ch;
    }
    out[k] = '\0';
}

/* Writes the block as whitespace-separated numbers; '?' marks a missing value. */
static int write_data(const Sheet *g, int r1, int c1, int r2, int c2, const char *path,
                      int *headers) {
    int hdr = 0;
    for (int c = c1 + 1; c <= c2 && !hdr; c++) {
        double v;
        int st = cell_num(g, r1, c, 0, &v);
        if (st == 1 || st == 2) hdr = 1;
    }
    int first = hdr ? r1 + 1 : r1;
    if (first > r2) return -1;
    int tx = 0;
    for (int r = first; r <= r2 && !tx; r++) {
        double v;
        int st = cell_num(g, r, c1, 0, &v);
        if (st != 0 && st != 3) tx = 1;
    }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int seq = 0;
    for (int r = first; r <= r2; r++) {
        seq++;
        if (tx) fprintf(f, "%d", seq);
        else {
            double v;
            if (cell_num(g, r, c1, 0, &v) == 0) fprintf(f, "%.17g", v);
            else fputc('?', f);
        }
        for (int c = c1 + 1; c <= c2; c++) {
            double v;
            if (cell_num(g, r, c, 0, &v) == 0) fprintf(f, " %.17g", v);
            else fprintf(f, " ?");
        }
        fputc('\n', f);
    }
    fclose(f);
    *headers = hdr;
    return 0;
}

/* Writes each ';'-separated command on its own line, trimmed. */
static void write_commands(FILE *f, const char *cmds) {
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
}

int graph_draw(const Doc *d, int r1, int c1, int r2, int c2, const char *cmds,
               const char *base_name, char *pdf, size_t pdfn, char *msg, size_t msgn, int view) {
    if (c2 - c1 < 1) {
        snprintf(msg, msgn, "graph needs at least two columns (first is X)");
        return -1;
    }
    char dir[2048], base[160] = "", dat[2200], gp[2200], log[2200];
    cache_dir(dir, sizeof dir);
    if (make_dirs(dir) < 0) {
        snprintf(msg, msgn, "cannot create %s", dir);
        return -1;
    }
    safe_text(base, sizeof base, base_name ? base_name : "graph", 0);
    if (!base[0]) snprintf(base, sizeof base, "graph");
    snprintf(dat, sizeof dat, "%s/%s-graph.dat", dir, base);
    snprintf(gp, sizeof gp, "%s/%s-graph.gp", dir, base);
    snprintf(log, sizeof log, "%s/%s-graph.log", dir, base);
    snprintf(pdf, pdfn, "%s/%s-graph.pdf", dir, base);

    int headers = 0;
    if (write_data(d->g, r1, c1, r2, c2, dat, &headers) < 0) {
        snprintf(msg, msgn, "graph needs at least one data row under the header");
        return -1;
    }
    /* time on the X axis: detected from the first data cell */
    int first = headers ? r1 + 1 : r1;
    int timex = cell_kind(d->g, first, c1) == KIND_TIME;

    FILE *f = fopen(gp, "w");
    if (!f) {
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
    write_commands(f, cmds ? cmds : "");          /* every graph option, in order */
    fprintf(f, "plot ");
    for (int c = c1 + 1; c <= c2; c++) {
        char name[160];
        if (headers) {
            const char *t = sheet_get(d->g, r1, c);
            safe_text(name, sizeof name, t ? t : "", 1);
        } else name[0] = '\0';
        if (!name[0]) snprintf(name, sizeof name, "Series %d", c - c1);
        fprintf(f, "%s'%s' using 1:%d title '%s'", c > c1 + 1 ? ", " : "", dat, c - c1 + 1, name);
    }
    fprintf(f, "\n");
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
