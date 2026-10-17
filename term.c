/* Terminal I/O: raw termios mode, buffered ANSI output, key and SGR mouse input. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "tsheet.h"

static int tin = -1, owned_fd;
static struct termios orig;
static volatile sig_atomic_t winch;
static unsigned char ib[1024];
static size_t ibn;
static char *ob;
static size_t obn, obcap;

static void on_winch(int sig) { (void)sig; winch = 1; }

void term_write(const char *s, size_t n) {
    if (obn + n > obcap) {
        size_t cap = (obn + n) * 2 + 4096;
        char *p = realloc(ob, cap);
        if (!p) return;
        ob = p;
        obcap = cap;
    }
    memcpy(ob + obn, s, n);
    obn += n;
}

void term_flush(void) {
    size_t off = 0;
    while (off < obn) {
        ssize_t k = write(tin, ob + off, obn - off);
        if (k < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        off += (size_t)k;
    }
    obn = 0;
}

int term_init(void) {
    tin = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tin >= 0) owned_fd = 1;
    else if (isatty(0)) tin = 0;
    else return -1;
    if (tcgetattr(tin, &orig) < 0) return -1;
    struct termios raw = orig;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(tin, TCSAFLUSH, &raw);
    signal(SIGWINCH, on_winch);
    /* alternate screen, hidden cursor, all pointer motion (1003) with SGR coordinates (1006) */
    const char *on = "\x1b[?1049h\x1b[?25l\x1b[?1003h\x1b[?1006h\x1b[2J";
    term_write(on, strlen(on));
    term_flush();
    return 0;
}

void term_fini(void) {
    if (tin < 0) return;
    const char *off = "\x1b[0m\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?25h\x1b[?1049l";
    term_write(off, strlen(off));
    term_flush();
    tcsetattr(tin, TCSAFLUSH, &orig);
    if (owned_fd) close(tin);
    tin = -1;
}

void term_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(tin, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

/* ---------- input parsing ---------- */

static void consume(size_t n) {
    memmove(ib, ib + n, ibn - n);
    ibn -= n;
}

/* CSI parameters: up to 4 numbers. Returns 1 event, 0 incomplete, 2 ignored. */
static int parse_csi(const unsigned char *p, size_t n, Event *e, size_t *used) {
    size_t j = 2;
    while (j < n && p[j] >= 0x20 && p[j] <= 0x3f) j++;
    if (j >= n) return 0;
    if (p[j] < 0x40 || p[j] > 0x7e) { *used = j; return 2; }
    unsigned char fin = p[j];
    *used = j + 1;
    size_t plen = j - 2;
    const unsigned char *par = p + 2;
    int mouse = plen && par[0] == '<';
    int nums[4] = {0, 0, 0, 0}, nn = 0, cur = 0;
    for (size_t k = mouse ? 1 : 0; k < plen; k++) {
        if (par[k] >= '0' && par[k] <= '9') cur = cur * 10 + (par[k] - '0');
        else if (par[k] == ';') { if (nn < 4) nums[nn++] = cur; cur = 0; }
    }
    if (nn < 4) nums[nn++] = cur;

    if (mouse && (fin == 'M' || fin == 'm')) {
        int b = nums[0];
        e->type = EV_MOUSE;
        e->mx = nums[1] - 1;
        e->my = nums[2] - 1;
        e->mods = ((b & 4) ? MOD_SHIFT : 0) | ((b & 16) ? MOD_CTRL : 0);
        if (b & 64) {
            e->mtype = (b & 1) ? MS_WHEEL_DOWN : MS_WHEEL_UP;
            e->btn = 0;
        } else {
            e->btn = b & 3;
            e->mtype = (b & 32) ? MS_MOVE : (fin == 'm' ? MS_RELEASE : MS_PRESS);
        }
        return 1;
    }
    int mods = 0;
    if (nn >= 2 && nums[1]) {
        int m = nums[1] - 1;
        if (m & 1) mods |= MOD_SHIFT;
        if (m & 4) mods |= MOD_CTRL;
    }
    int key;
    switch (fin) {
    case 'A': key = K_UP; break;
    case 'B': key = K_DOWN; break;
    case 'C': key = K_RIGHT; break;
    case 'D': key = K_LEFT; break;
    case 'H': key = K_HOME; break;
    case 'F': key = K_END; break;
    case 'Z': key = K_BTAB; mods |= MOD_SHIFT; break;
    case '~':
        switch (nums[0]) {
        case 1: case 7: key = K_HOME; break;
        case 4: case 8: key = K_END; break;
        case 3: key = K_DEL; break;
        case 5: key = K_PGUP; break;
        case 6: key = K_PGDN; break;
        case 11: case 12: case 13: case 14: case 15:
            key = K_F1 + (nums[0] - 11); break;
        case 17: case 18: case 19: case 20: case 21:
            key = K_F6 + (nums[0] - 17); break;
        default: return 2;
        }
        break;
    default: return 2;
    }
    e->type = EV_KEY;
    e->key = key;
    e->mods = mods;
    return 1;
}

static int parse_one(const unsigned char *p, size_t n, Event *e, size_t *used) {
    if (n == 0) return 0;
    if (p[0] != 0x1b) {
        e->type = EV_KEY;
        e->key = p[0];
        e->mods = 0;
        *used = 1;
        return 1;
    }
    if (n == 1) return 0;
    if (p[1] == '[') return parse_csi(p, n, e, used);
    if (p[1] == 'O') {                          /* SS3: application cursor / F1-F4 */
        if (n < 3) return 0;
        int key;
        switch (p[2]) {
        case 'A': key = K_UP; break;
        case 'B': key = K_DOWN; break;
        case 'C': key = K_RIGHT; break;
        case 'D': key = K_LEFT; break;
        case 'H': key = K_HOME; break;
        case 'F': key = K_END; break;
        case 'P': key = K_F1; break;
        case 'Q': key = K_F2; break;
        case 'R': key = K_F3; break;
        case 'S': key = K_F4; break;
        default: *used = 3; return 2;
        }
        e->type = EV_KEY;
        e->key = key;
        e->mods = 0;
        *used = 3;
        return 1;
    }
    e->type = EV_KEY;                           /* lone ESC (alt-keys are not used) */
    e->key = 0x1b;
    e->mods = 0;
    *used = 1;
    return 1;
}

int term_poll(Event *e, int timeout_ms) {
    for (;;) {
        if (winch) {
            winch = 0;
            e->type = EV_RESIZE;
            return 1;
        }
        if (ibn) {
            size_t used = 0;
            int r = parse_one(ib, ibn, e, &used);
            if (r == 1) { consume(used); return 1; }
            if (r == 2) { consume(used); continue; }
        }
        struct pollfd pfd = {tin, POLLIN, 0};
        int pr = poll(&pfd, 1, ibn ? 25 : timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            if (ibn) {                          /* incomplete sequence: it was a bare ESC */
                e->type = EV_KEY;
                e->key = 0x1b;
                e->mods = 0;
                consume(1);
                return 1;
            }
            return 0;
        }
        ssize_t k = read(tin, ib + ibn, sizeof ib - ibn);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (k == 0) return -1;
        ibn += (size_t)k;
    }
}
