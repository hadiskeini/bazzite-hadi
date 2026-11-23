// holdsuper — tap/hold for one mouse button: tap = the button itself, hold = Super.
//
// Grabs every pointer node of the mouse (the Scyrox dongle has two, and the one that carries the events
// is not fixed), forwards every event verbatim to a uinput clone per node with identical
// capabilities (hi-res wheel kept, no ABS axes, so no phantom joystick), and swallows one button:
// released before the threshold -> the clone emits a normal click of that button; held past it,
// or another button/wheel used while it is down -> a separate uinput keyboard holds Super.
//
// A desktop front end (in Plasma: /usr/libexec/holdsuper-kde) drives it over $XDG_RUNTIME_DIR/holdsuper.sock with
// newline-terminated commands: "pass 0|1", "hold <ms>", "overview 0|1", "combos 0|1". "pass 1" (a game is
// focused) CLOSES the device: the mouse then runs the stock kernel -> libinput path with nothing of
// ours in it. With no front end connected the daemon also stays in pass mode.
//
// Build: cc -O2 -Wall -Wextra -o holdsuper holdsuper.c   (kernel headers only, no libraries)
// Usage: holdsuper [-d VID:PID[,VID:PID...]] [-b BUTTON_CODE] [-v]

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PHYS_TAG "holdsuper"
#define MAX_IDS 8
#define MAX_SRC 4
#define NBITS(x) ((((x) - 1) / (8 * sizeof(unsigned long))) + 1)
#define TEST_BIT(bit, arr) (((arr)[(bit) / (8 * sizeof(unsigned long))] >> ((bit) % (8 * sizeof(unsigned long)))) & 1UL)

static struct { unsigned short vendor, product; } ids[MAX_IDS] = {{0x3554, 0xf5f7}, {0x3554, 0xf5f6}};
static int n_ids = 2;
static int button = BTN_EXTRA;
static bool verbose;

// runtime settings (from the front end)
static bool pass = true;          // true = device closed, mouse fully native
static int hold_ms = 200;
static bool overview = false;     // let a lone hold-and-release act as a real Super tap (Plasma: the launcher)
// During a hold, the middle button sends Super+F12 and the other side button Super+F11 in place of the click:
// the front end binds them to minimize and close the window under the pointer. The compositor takes
// Super+button for its own window grabs, so the front end cannot see the clicks themselves. Not Super+Q: Plasma
// binds it to the activity switcher.
static bool combos = false;
static bool swallowed_middle, swallowed_side;

// A clone outlives pass mode and is reused when the node with the same phys comes back.
struct source {
    int fd;                                   // grabbed node, -1 while passing through
    dev_t rdev;
    int clone;
    char phys[64];
    unsigned long keys[NBITS(KEY_CNT)];       // buttons the clone currently reports down
    bool dropping;                            // SYN_DROPPED seen, skipping to the next SYN_REPORT
};
static struct source srcs[MAX_SRC] = {[0 ... MAX_SRC - 1] = {.fd = -1, .clone = -1}};
static struct source *btn_src;    // node the held button went down on
static int settle_ticks;          // rescans after a new grab, for nodes that enumerate a moment later
static int kbd_fd = -1;
static int timer_fd = -1, listen_fd = -1, client_fd = -1, pending_fd = -1;
static char client_buf[256], pending_buf[256];
static size_t client_len, pending_len;

static enum { IDLE, PENDING, HELD } state = IDLE;
static bool used;                 // another button/wheel happened during the hold
static bool want_grab_retry;      // device missing or a button held: retry each second

static volatile sig_atomic_t quit;

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define dbg(...) do { if (verbose) logmsg(__VA_ARGS__); } while (0)

static void emit(int fd, int type, int code, int value) {
    struct input_event ev = {.type = type, .code = code, .value = value};
    if (write(fd, &ev, sizeof ev) != sizeof ev)
        logmsg("uinput write: %s", strerror(errno));
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static void set_clone_key(struct source *s, int code, int value) {
    unsigned long m = 1UL << (code % (8 * sizeof(unsigned long)));
    if (value) s->keys[code / (8 * sizeof(unsigned long))] |= m;
    else s->keys[code / (8 * sizeof(unsigned long))] &= ~m;
}

// ---------------------------------------------------------------- uinput devices

static int make_kbd(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { logmsg("open /dev/uinput: %s", strerror(errno)); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTMETA);
    ioctl(fd, UI_SET_KEYBIT, KEY_UNKNOWN);
    ioctl(fd, UI_SET_KEYBIT, KEY_F12);
    ioctl(fd, UI_SET_KEYBIT, KEY_F11);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFT);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHT);
    ioctl(fd, UI_SET_PHYS, PHYS_TAG "/kbd");
    struct uinput_setup us = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0x5355, .version = 1}};
    snprintf(us.name, sizeof us.name, "holdsuper Super key");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        logmsg("create kbd uinput: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// A uinput device that looks like the source: same name, ids, props, keys, rels, msc.
static int make_clone(int s) {
    unsigned long bits[NBITS(KEY_CNT)];
    char name[UINPUT_MAX_NAME_SIZE] = "";
    struct input_id id;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { logmsg("open /dev/uinput: %s", strerror(errno)); return -1; }

    ioctl(s, EVIOCGNAME(sizeof name - 1), name);
    ioctl(s, EVIOCGID, &id);

    static const struct { int ev, max, set; } types[] = {
        {EV_KEY, KEY_MAX, UI_SET_KEYBIT}, {EV_REL, REL_MAX, UI_SET_RELBIT}, {EV_MSC, MSC_MAX, UI_SET_MSCBIT}};
    for (size_t t = 0; t < sizeof types / sizeof *types; t++) {
        memset(bits, 0, sizeof bits);
        if (ioctl(s, EVIOCGBIT(types[t].ev, sizeof bits), bits) < 0) continue;
        ioctl(fd, UI_SET_EVBIT, types[t].ev);
        for (int c = 0; c <= types[t].max; c++)
            if (TEST_BIT(c, bits)) ioctl(fd, types[t].set, c);
    }
    memset(bits, 0, sizeof bits);
    if (ioctl(s, EVIOCGPROP(sizeof bits), bits) >= 0)
        for (int p = 0; p <= INPUT_PROP_MAX; p++)
            if (TEST_BIT(p, bits)) ioctl(fd, UI_SET_PROPBIT, p);

    ioctl(fd, UI_SET_PHYS, PHYS_TAG "/pointer");
    struct uinput_setup us = {.id = id};
    snprintf(us.name, sizeof us.name, "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        logmsg("create clone uinput: %s", strerror(errno));
        close(fd);
        return -1;
    }
    logmsg("clone created for \"%s\" %04x:%04x", name, id.vendor, id.product);
    return fd;
}

// ---------------------------------------------------------------- source device

static bool any_button_down(int fd);
static void release_all(void);

static int n_grabbed(void) {
    int n = 0;
    for (int i = 0; i < MAX_SRC; i++) n += srcs[i].fd >= 0;
    return n;
}

static struct source *slot_for(const char *phys) {
    for (int i = 0; i < MAX_SRC; i++)
        if (srcs[i].fd < 0 && srcs[i].clone >= 0 && !strcmp(srcs[i].phys, phys)) return &srcs[i];
    for (int i = 0; i < MAX_SRC; i++)
        if (srcs[i].fd < 0 && srcs[i].clone < 0) return &srcs[i];
    for (int i = 0; i < MAX_SRC; i++)
        if (srcs[i].fd < 0) {
            ioctl(srcs[i].clone, UI_DEV_DESTROY);
            close(srcs[i].clone);
            srcs[i].clone = -1;
            return &srcs[i];
        }
    return NULL;
}

// Grab each matching node that is not grabbed yet. A node with a button physically down is left for the
// next tick: a press libinput saw would otherwise never see its release and stay stuck.
static void scan_sources(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) < 0) continue;
        bool have = false;
        for (int i = 0; i < MAX_SRC; i++) have |= srcs[i].fd >= 0 && srcs[i].rdev == st.st_rdev;
        if (have) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct input_id id;
        char phys[64] = "";
        unsigned long keys[NBITS(KEY_CNT)] = {0}, rels[NBITS(REL_CNT)] = {0};
        bool ok = ioctl(fd, EVIOCGID, &id) >= 0;
        ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
        ok = ok && strncmp(phys, PHYS_TAG, strlen(PHYS_TAG)) != 0;   // never our own clone
        if (ok) {
            bool idmatch = false;
            for (int i = 0; i < n_ids; i++)
                idmatch |= id.vendor == ids[i].vendor && id.product == ids[i].product;
            ok = idmatch;
        }
        ok = ok && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 && TEST_BIT(button, keys)
                && ioctl(fd, EVIOCGBIT(EV_REL, sizeof rels), rels) >= 0 && TEST_BIT(REL_X, rels);
        if (!ok) { close(fd); continue; }
        if (any_button_down(fd)) { close(fd); want_grab_retry = true; continue; }
        struct source *s = slot_for(phys);
        if (!s) { close(fd); continue; }
        if (s->clone < 0) {
            snprintf(s->phys, sizeof s->phys, "%s", phys);
            memset(s->keys, 0, sizeof s->keys);
            if ((s->clone = make_clone(fd)) < 0) { close(fd); want_grab_retry = true; continue; }
        }
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            logmsg("grab %s: %s", path, strerror(errno));
            close(fd);
            want_grab_retry = true;
            continue;
        }
        // drain whatever queued up before the grab
        struct input_event evs[64];
        while (read(fd, evs, sizeof evs) > 0) {}
        s->fd = fd;
        s->rdev = st.st_rdev;
        s->dropping = false;
        logmsg("grabbed %s (%s)", path, phys);
    }
    closedir(d);
}

static bool any_button_down(int fd) {
    unsigned long keys[NBITS(KEY_CNT)] = {0};
    if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0) return false;
    for (size_t i = 0; i < NBITS(KEY_CNT); i++)
        if (keys[i]) return true;
    return false;
}

static void disarm_timer(void) {
    struct itimerspec its = {0};
    timerfd_settime(timer_fd, 0, &its, NULL);
}

// Put the clone and the Super key back to "nothing pressed" so nothing sticks across a hand-over.
static void release_all(void) {
    disarm_timer();
    if (state == HELD) { emit(kbd_fd, EV_KEY, KEY_LEFTMETA, 0); syn(kbd_fd); }
    state = IDLE;
    btn_src = NULL;
    for (int i = 0; i < MAX_SRC; i++) {
        struct source *s = &srcs[i];
        if (s->clone < 0) continue;
        bool any = false;
        for (int c = 0; c < KEY_CNT; c++)
            if (TEST_BIT(c, s->keys)) { emit(s->clone, EV_KEY, c, 0); any = true; }
        if (any) syn(s->clone);
        memset(s->keys, 0, sizeof s->keys);
    }
}

static void close_sources(void) {
    if (!n_grabbed()) return;
    release_all();
    for (int i = 0; i < MAX_SRC; i++) {
        if (srcs[i].fd < 0) continue;
        ioctl(srcs[i].fd, EVIOCGRAB, 0);
        close(srcs[i].fd);
        srcs[i].fd = -1;
    }
    logmsg("released mouse (native path)");
}

static void try_grab(void) {
    want_grab_retry = false;
    if (pass) return;
    int before = n_grabbed();
    scan_sources();
    int after = n_grabbed();
    if (after > before) {
        if (!before) logmsg("grabbed mouse (tap/hold active, hold %d ms)", hold_ms);
        settle_ticks = 3;
    }
    if (!after) want_grab_retry = true;
    else if (settle_ticks > 0) { settle_ticks--; want_grab_retry = true; }
}

static void set_pass(bool p) {
    if (p == pass) return;
    pass = p;
    if (pass) close_sources();
    else try_grab();
}

// ---------------------------------------------------------------- tap / hold

static void resolve_hold(void) {
    disarm_timer();
    state = HELD;
    emit(kbd_fd, EV_KEY, KEY_LEFTMETA, 1);
    syn(kbd_fd);
    dbg("hold -> Super down");
}

static void on_button(struct source *s, int value) {
    if (value == 1) {
        if (state != IDLE) return;
        state = PENDING;
        btn_src = s;
        used = false;
        struct itimerspec its = {.it_value = {.tv_sec = hold_ms / 1000, .tv_nsec = (long)(hold_ms % 1000) * 1000000L}};
        timerfd_settime(timer_fd, 0, &its, NULL);
    } else if (value == 0) {
        if (state == PENDING) {
            disarm_timer();
            emit(btn_src->clone, EV_KEY, button, 1); syn(btn_src->clone);
            emit(btn_src->clone, EV_KEY, button, 0); syn(btn_src->clone);
            dbg("tap -> button %d", button);
        } else if (state == HELD) {
            // Mutter opens Activities when Super goes down and up with nothing in between; any other
            // key press cancels that. KEY_UNKNOWN has no keysym, so it does nothing else.
            if (!used && !overview) {
                emit(kbd_fd, EV_KEY, KEY_UNKNOWN, 1); syn(kbd_fd);
                emit(kbd_fd, EV_KEY, KEY_UNKNOWN, 0); syn(kbd_fd);
            }
            emit(kbd_fd, EV_KEY, KEY_LEFTMETA, 0);
            syn(kbd_fd);
            dbg("release -> Super up");
        }
        state = IDLE;
        btn_src = NULL;
    }
    // value 2 (autorepeat) is ignored
}

// After SYN_DROPPED: make the clone's button state match the hardware again.
static void resync(struct source *s) {
    unsigned long keys[NBITS(KEY_CNT)] = {0};
    if (ioctl(s->fd, EVIOCGKEY(sizeof keys), keys) < 0) return;
    bool any = false;
    for (int c = 0; c < KEY_CNT; c++) {
        if (c == button) continue;
        bool hw = TEST_BIT(c, keys), cl = TEST_BIT(c, s->keys);
        if (hw != cl) { emit(s->clone, EV_KEY, c, hw); set_clone_key(s, c, hw); any = true; }
    }
    if (any) syn(s->clone);
    if (!TEST_BIT(button, keys) && state != IDLE && btn_src == s) on_button(s, 0);
}

static void read_source(struct source *s) {
    struct input_event evs[256];
    for (;;) {
        ssize_t n = read(s->fd, evs, sizeof evs);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            logmsg("mouse read %s: %s (unplugged?)", s->phys, strerror(errno));
            release_all();
            close(s->fd);
            s->fd = -1;
            want_grab_retry = true;
            return;
        }
        if (n == 0) return;
        size_t count = (size_t)n / sizeof *evs;
        for (size_t i = 0; i < count; i++) {
            struct input_event *ev = &evs[i];
            if (ev->type == EV_SYN && ev->code == SYN_DROPPED) { s->dropping = true; continue; }
            if (s->dropping) {
                if (ev->type == EV_SYN && ev->code == SYN_REPORT) { s->dropping = false; resync(s); }
                continue;
            }
            if (ev->type == EV_KEY && ev->code == button) { on_button(s, ev->value); continue; }
            if (state != IDLE) {
                bool other = (ev->type == EV_KEY && ev->value == 1) ||
                             (ev->type == EV_REL && (ev->code == REL_WHEEL || ev->code == REL_HWHEEL ||
                                                     ev->code == REL_WHEEL_HI_RES || ev->code == REL_HWHEEL_HI_RES));
                if (other) {
                    used = true;
                    if (state == PENDING) resolve_hold();
                }
            }
            if (ev->type == EV_KEY && (ev->code == BTN_MIDDLE || ev->code == BTN_SIDE)) {
                bool *swallowed = ev->code == BTN_MIDDLE ? &swallowed_middle : &swallowed_side;
                if (ev->value == 1 && combos && state == HELD) {
                    int key = ev->code == BTN_MIDDLE ? KEY_F12 : KEY_F11;
                    emit(kbd_fd, EV_KEY, key, 1); syn(kbd_fd);
                    emit(kbd_fd, EV_KEY, key, 0); syn(kbd_fd);
                    *swallowed = true;
                    continue;
                }
                if (ev->value == 0 && *swallowed) { *swallowed = false; continue; }
            }
            if (ev->type == EV_KEY) set_clone_key(s, ev->code, ev->value);
            if (write(s->clone, ev, sizeof *ev) != sizeof *ev && errno != EAGAIN)
                logmsg("clone write: %s", strerror(errno));
        }
    }
}

// ---------------------------------------------------------------- control socket

static void handle_line(char *line) {
    int v;
    if (sscanf(line, "pass %d", &v) == 1) set_pass(v != 0);
    else if (sscanf(line, "hold %d", &v) == 1 && v >= 50 && v <= 2000) hold_ms = v;
    else if (sscanf(line, "overview %d", &v) == 1) overview = v != 0;
    else if (sscanf(line, "combos %d", &v) == 1) combos = v != 0;
    else { logmsg("unknown command: %s", line); return; }
    dbg("cmd: %s", line);
}

static void handle_client_buf(void) {
    char *start = client_buf, *nl;
    while ((nl = strchr(start, '\n'))) {
        *nl = 0;
        handle_line(start);
        start = nl + 1;
    }
    client_len = strlen(start);
    memmove(client_buf, start, client_len + 1);
    if (client_len == sizeof client_buf - 1) client_len = 0;   // garbage line, drop it
}

static void drop_client(void) {
    if (client_fd < 0) return;
    close(client_fd);
    client_fd = -1;
    client_len = 0;
    combos = false;
    set_pass(true);   // no front end = no game detection: stay native
}

static void read_client(void) {
    ssize_t n = read(client_fd, client_buf + client_len, sizeof client_buf - 1 - client_len);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        logmsg("front end disconnected");
        drop_client();
        return;
    }
    client_len += (size_t)n;
    client_buf[client_len] = 0;
    handle_client_buf();
}

static bool is_front_end_line(const char *line) {
    return !strncmp(line, "pass ", 5) || !strncmp(line, "hold ", 5) || !strncmp(line, "overview ", 9) ||
           !strncmp(line, "combos ", 7);
}

// A new connection replaces the front end only if its first line is a front-end command. A probe (a client
// that sends "ping") must not drop the front end: without one, holdsuper releases the mouse.
static void read_pending(void) {
    ssize_t n = read(pending_fd, pending_buf + pending_len, sizeof pending_buf - 1 - pending_len);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n > 0) {
        pending_len += (size_t)n;
        pending_buf[pending_len] = 0;
        char *nl = strchr(pending_buf, '\n');
        if (!nl && pending_len < sizeof pending_buf - 1) return;
        if (nl) *nl = 0;
        bool front_end = nl && is_front_end_line(pending_buf);
        if (nl) *nl = '\n';
        if (front_end) {
            if (client_fd >= 0) close(client_fd);
            client_fd = pending_fd;
            memcpy(client_buf, pending_buf, pending_len + 1);
            client_len = pending_len;
            pending_fd = -1;
            pending_len = 0;
            logmsg("front end connected");
            handle_client_buf();
            return;
        }
        if (nl) *nl = 0;
        logmsg("not a front end, closed: %s", pending_buf);
    }
    close(pending_fd);
    pending_fd = -1;
    pending_len = 0;
}

static int make_listener(void) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) { logmsg("XDG_RUNTIME_DIR not set"); return -1; }
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s/holdsuper.sock", rt);
    unlink(sa.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 2) < 0) {
        logmsg("socket %s: %s", sa.sun_path, strerror(errno));
        return -1;
    }
    chmod(sa.sun_path, 0600);
    return fd;
}

// ---------------------------------------------------------------- main

static void on_signal(int sig) { (void)sig; quit = 1; }

static int parse_ids(const char *arg) {
    n_ids = 0;
    char *copy = strdup(arg), *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok && n_ids < MAX_IDS; tok = strtok_r(NULL, ",", &save)) {
        unsigned v, p;
        if (sscanf(tok, "%x:%x", &v, &p) != 2) { free(copy); return -1; }
        ids[n_ids].vendor = (unsigned short)v;
        ids[n_ids].product = (unsigned short)p;
        n_ids++;
    }
    free(copy);
    return n_ids ? 0 : -1;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "d:b:v")) != -1) {
        switch (opt) {
        case 'd': if (parse_ids(optarg) < 0) { logmsg("bad -d, want VID:PID[,VID:PID]"); return 2; } break;
        case 'b': button = atoi(optarg); break;
        case 'v': verbose = true; break;
        default: logmsg("usage: %s [-d VID:PID[,...]] [-b BUTTON_CODE] [-v]", argv[0]); return 2;
        }
    }

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if ((kbd_fd = make_kbd()) < 0) return 1;
    if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) < 0) return 1;
    if ((listen_fd = make_listener()) < 0) return 1;
    logmsg("holdsuper ready: button %d, waiting for a front end", button);

    while (!quit) {
        struct pollfd pf[4 + MAX_SRC];
        struct source *pf_src[4 + MAX_SRC] = {0};
        int n = 0, i_client = -1, i_pending = -1;
        pf[n++] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
        pf[n++] = (struct pollfd){.fd = timer_fd, .events = POLLIN};
        for (int i = 0; i < MAX_SRC; i++)
            if (srcs[i].fd >= 0) { pf_src[n] = &srcs[i]; pf[n++] = (struct pollfd){.fd = srcs[i].fd, .events = POLLIN}; }
        if (client_fd >= 0) { i_client = n; pf[n++] = (struct pollfd){.fd = client_fd, .events = POLLIN}; }
        if (pending_fd >= 0) { i_pending = n; pf[n++] = (struct pollfd){.fd = pending_fd, .events = POLLIN}; }

        int r = poll(pf, n, want_grab_retry ? 1000 : -1);
        if (r < 0) { if (errno == EINTR) continue; logmsg("poll: %s", strerror(errno)); break; }
        if (r == 0) { try_grab(); continue; }

        if (pf[1].revents & POLLIN) {
            uint64_t exp;
            if (read(timer_fd, &exp, sizeof exp) == sizeof exp && state == PENDING) resolve_hold();
        }
        for (int i = 0; i < n; i++)
            if (pf_src[i] && pf_src[i]->fd >= 0 && pf[i].revents) read_source(pf_src[i]);
        if (i_client >= 0 && pf[i_client].revents) read_client();
        if (i_pending >= 0 && pf[i_pending].revents) read_pending();
        if (pf[0].revents & POLLIN) {
            int c = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (c >= 0) {
                if (pending_fd >= 0) close(pending_fd);
                pending_fd = c;
                pending_len = 0;
            }
        }
    }

    close_sources();
    for (int i = 0; i < MAX_SRC; i++)
        if (srcs[i].clone >= 0) { ioctl(srcs[i].clone, UI_DEV_DESTROY); close(srcs[i].clone); }
    ioctl(kbd_fd, UI_DEV_DESTROY);
    close(kbd_fd);
    return 0;
}
