#include "tsheet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_XLIB
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

/* Backend selection:
     WAYLAND_DISPLAY set and wl-copy/wl-paste installed -> wl-clipboard
     else DISPLAY set and xclip or xsel installed        -> X11
     else                                                -> OSC 52 + internal
   With libX11 available, X11 copies are served by a small forked owner process
   that offers text/html and text/plain together (xclip can only offer one type).
   The internal copy is always kept so paste works even if the system read fails. */

#define MAX_NATIVE_BYTES 200000     /* keep under the X request size limit */

static int backend = -1;            /* 0 none, 1 wayland, 2 x11 */
static const char *copy_cmd = NULL;
static const char *paste_cmd = NULL;
static const char *html_cmd = NULL;   /* reads text/html, NULL if unsupported */
static const char *bname = "internal (OSC 52)";
static char *internal_copy = NULL;
static pid_t owner_pid = 0;

static int have(const char *cmd) {
    char buf[128];
    snprintf(buf, sizeof buf, "command -v %s >/dev/null 2>&1", cmd);
    return system(buf) == 0;
}

static void detect(void) {
    if (backend >= 0) return;
    const char *wl = getenv("WAYLAND_DISPLAY");
    const char *x = getenv("DISPLAY");
    backend = 0;
    if (wl && *wl && have("wl-copy") && have("wl-paste")) {
        backend = 1;
        copy_cmd = "wl-copy";
        paste_cmd = "wl-paste --no-newline 2>/dev/null";
        html_cmd = "wl-paste --no-newline --type text/html 2>/dev/null";
        bname = "wayland (wl-clipboard)";
    } else if (x && *x && have("xclip")) {
        backend = 2;
        copy_cmd = "xclip -selection clipboard -i";
        paste_cmd = "xclip -selection clipboard -o 2>/dev/null";
        html_cmd = "xclip -selection clipboard -t text/html -o 2>/dev/null";
        bname = "x11 (xclip)";
    } else if (x && *x && have("xsel")) {
        backend = 2;
        copy_cmd = "xsel --clipboard --input";
        paste_cmd = "xsel --clipboard --output 2>/dev/null";
        bname = "x11 (xsel)";
    }
}

/* OSC 52: asks the terminal emulator to set the system clipboard (works over SSH). */
static void osc52(const char *text) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(text);
    fputs("\033]52;c;", stdout);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)text[i] << 16;
        if (i + 1 < n) v |= (unsigned char)text[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)text[i + 2];
        fputc(tbl[(v >> 18) & 63], stdout);
        fputc(tbl[(v >> 12) & 63], stdout);
        fputc(i + 1 < n ? tbl[(v >> 6) & 63] : '=', stdout);
        fputc(i + 2 < n ? tbl[v & 63] : '=', stdout);
    }
    fputs("\a", stdout);
    fflush(stdout);
}

static int pipe_to(const char *cmd, const char *text) {
    FILE *f = popen(cmd, "w");
    if (!f) return -1;
    fwrite(text, 1, strlen(text), f);
    return pclose(f) == 0 ? 0 : -1;
}

static char *read_cmd(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(f); return NULL; }
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) { free(buf); pclose(f); return NULL; }
            buf = p;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        if (n == 0) break;
        len += n;
    }
    buf[len] = '\0';
    if (pclose(f) != 0) { free(buf); return NULL; }
    return buf;
}

/* ---------- native X11 multi-format owner ---------- */

#ifdef HAVE_XLIB
/* Runs in the forked child: owns CLIPBOARD until another client takes it. */
static void x11_serve(const char *text, size_t tlen, const char *html, size_t hlen) {
    Display *d = XOpenDisplay(NULL);
    if (!d) _exit(1);
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    Atom clip = XInternAtom(d, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
    Atom targets = XInternAtom(d, "TARGETS", False);
    Atom html_a = XInternAtom(d, "text/html", False);
    Atom plain = XInternAtom(d, "text/plain;charset=utf-8", False);

    XSetSelectionOwner(d, clip, w, CurrentTime);
    XFlush(d);
    if (XGetSelectionOwner(d, clip) != w) _exit(1);

    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == SelectionClear) break;
        if (ev.type != SelectionRequest) continue;

        XSelectionRequestEvent *rq = &ev.xselectionrequest;
        XEvent resp;
        memset(&resp, 0, sizeof resp);
        resp.xselection.type = SelectionNotify;
        resp.xselection.display = rq->display;
        resp.xselection.requestor = rq->requestor;
        resp.xselection.selection = rq->selection;
        resp.xselection.target = rq->target;
        resp.xselection.time = rq->time;
        resp.xselection.property = rq->property ? rq->property : rq->target;

        if (rq->target == targets) {
            Atom list[5] = {targets, utf8, XA_STRING, plain, html_a};
            int n = hlen ? 5 : 4;
            XChangeProperty(d, rq->requestor, resp.xselection.property, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)list, n);
        } else if (rq->target == html_a && hlen) {
            XChangeProperty(d, rq->requestor, resp.xselection.property, html_a, 8,
                            PropModeReplace, (const unsigned char *)html, (int)hlen);
        } else if (rq->target == utf8 || rq->target == XA_STRING || rq->target == plain) {
            XChangeProperty(d, rq->requestor, resp.xselection.property, rq->target, 8,
                            PropModeReplace, (const unsigned char *)text, (int)tlen);
        } else {
            resp.xselection.property = None;
        }
        XSendEvent(d, rq->requestor, False, 0, &resp);
        XFlush(d);
    }
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    _exit(0);
}
#endif

/* ---------- public API ---------- */

int clip_set_rich(const char *text, const char *html) {
    detect();
    free(internal_copy);
    internal_copy = strdup(text);

#ifdef HAVE_XLIB
    if (backend == 2 && strlen(text) + (html ? strlen(html) : 0) < MAX_NATIVE_BYTES) {
        if (owner_pid > 0) waitpid(owner_pid, NULL, WNOHANG);   /* reap old owner */
        pid_t pid = fork();
        if (pid == 0) {
            x11_serve(text, strlen(text), html ? html : "", html ? strlen(html) : 0);
        }
        if (pid > 0) {
            owner_pid = pid;
            return 0;
        }
        /* fork failed: fall through to xclip */
    }
#else
    (void)html;
#endif

    if (backend == 0) { osc52(text); return 0; }
    if (backend == 1 && html) {
        /* wl-copy offers one type per call: with formatting we publish the HTML table. */
        if (pipe_to("wl-copy --type text/html", html) == 0) return 0;
    }
    /* plain text only (one type per invocation) */
    return pipe_to(copy_cmd, text);
}

int clip_set(const char *text) { return clip_set_rich(text, NULL); }

char *clip_get(void) {
    detect();
    char *r = NULL;
    if (backend) r = read_cmd(paste_cmd);
    if (!r && internal_copy) r = strdup(internal_copy);
    return r;
}

char *clip_get_html(void) {
    detect();
    if (!html_cmd) return NULL;
    return read_cmd(html_cmd);
}

const char *clip_backend(void) {
    detect();
#ifdef HAVE_XLIB
    if (backend == 2) return "x11 native";
#endif
    return bname;
}
