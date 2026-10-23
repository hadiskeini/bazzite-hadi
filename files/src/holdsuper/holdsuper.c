// holdsuper — tap/hold for one mouse button: tap = the button itself, hold = Super.
//
// Grabs the mouse's pointer node, forwards every event verbatim to a uinput clone with identical
// capabilities (hi-res wheel kept, no ABS axes, so no phantom joystick), and swallows one button:
// released before the threshold -> the clone emits a normal click of that button; held past it,
// or another button/wheel used while it is down -> a separate uinput keyboard holds Super.
//
// The GNOME extension hold-super@hadi.local drives it over $XDG_RUNTIME_DIR/holdsuper.sock with
// newline-terminated commands: "pass 0|1", "hold <ms>", "overview 0|1". "pass 1" (a game is
// focused) CLOSES the device: the mouse then runs the stock kernel -> libinput path with nothing of
// ours in it. With no extension connected the daemon also stays in pass mode.
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
#define NBITS(x) ((((x) - 1) / (8 * sizeof(unsigned long))) + 1)
#define TEST_BIT(bit, arr) (((arr)[(bit) / (8 * sizeof(unsigned long))] >> ((bit) % (8 * sizeof(unsigned long)))) & 1UL)

static struct { unsigned short vendor, product; } ids[MAX_IDS] = {{0x3554, 0xf5f7}, {0x3554, 0xf5f6}};
static int n_ids = 2;
static int button = BTN_EXTRA;
static bool verbose;

// runtime settings (from the extension)
static bool pass = true;          // true = device closed, mouse fully native
static int hold_ms = 200;
static bool overview = false;     // let a lone hold-and-release open Activities, like a real Super tap

static int src = -1;              // grabbed source device, -1 while passing through
static int clone_fd = -1, kbd_fd = -1;
static unsigned short clone_vendor, clone_product;
static int timer_fd = -1, listen_fd = -1, client_fd = -1;
static char client_buf[256];
static size_t client_len;

static enum { IDLE, PENDING, HELD } state = IDLE;
static bool used;                 // another button/wheel happened during the hold
static bool dropping;             // SYN_DROPPED seen, skipping to the next SYN_REPORT
static unsigned long clone_keys[NBITS(KEY_CNT)];  // buttons the clone currently reports down
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

static void set_clone_key(int code, int value) {
    unsigned long m = 1UL << (code % (8 * sizeof(unsigned long)));
    if (value) clone_keys[code / (8 * sizeof(unsigned long))] |= m;
    else clone_keys[code / (8 * sizeof(unsigned long))] &= ~m;
}

// ---------------------------------------------------------------- uinput devices

static int make_kbd(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { logmsg("open /dev/uinput: %s", strerror(errno)); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTMETA);
    ioctl(fd, UI_SET_KEYBIT, KEY_UNKNOWN);
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
    clone_vendor = id.vendor;
    clone_product = id.product;
    memset(clone_keys, 0, sizeof clone_keys);
    logmsg("clone created for \"%s\" %04x:%04x", name, id.vendor, id.product);
    return fd;
}

// ---------------------------------------------------------------- source device

static int find_source(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
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
        if (ok) { found = fd; dbg("source: %s", path); }
        else close(fd);
    }
    closedir(d);
    return found;
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
    if (clone_fd >= 0) {
        bool any = false;
        for (int c = 0; c < KEY_CNT; c++)
            if (TEST_BIT(c, clone_keys)) { emit(clone_fd, EV_KEY, c, 0); any = true; }
        if (any) syn(clone_fd);
        memset(clone_keys, 0, sizeof clone_keys);
    }
}

static void close_source(void) {
    if (src < 0) return;
    release_all();
    ioctl(src, EVIOCGRAB, 0);
    close(src);
    src = -1;
    logmsg("released mouse (native path)");
}

// Grab only when no button is physically down: a press libinput saw would otherwise never see its
// release and stay stuck. If something is held, try again on the next one-second tick.
static void try_grab(void) {
    want_grab_retry = false;
    if (src >= 0 || pass) return;
    int fd = find_source();
    if (fd < 0) { want_grab_retry = true; return; }
    if (any_button_down(fd)) { close(fd); want_grab_retry = true; return; }
    struct input_id id;
    ioctl(fd, EVIOCGID, &id);
    if (clone_fd >= 0 && (id.vendor != clone_vendor || id.product != clone_product)) {
        ioctl(clone_fd, UI_DEV_DESTROY);
        close(clone_fd);
        clone_fd = -1;
    }
    if (clone_fd < 0 && (clone_fd = make_clone(fd)) < 0) { close(fd); want_grab_retry = true; return; }
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        logmsg("grab: %s", strerror(errno));
        close(fd);
        want_grab_retry = true;
        return;
    }
    // drain whatever queued up before the grab
    struct input_event evs[64];
    while (read(fd, evs, sizeof evs) > 0) {}
    src = fd;
    state = IDLE;
    dropping = false;
    logmsg("grabbed mouse (tap/hold active, hold %d ms)", hold_ms);
}

static void set_pass(bool p) {
    if (p == pass) return;
    pass = p;
    if (pass) close_source();
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

static void on_button(int value) {
    if (value == 1) {
        if (state != IDLE) return;
        state = PENDING;
        used = false;
        struct itimerspec its = {.it_value = {.tv_sec = hold_ms / 1000, .tv_nsec = (long)(hold_ms % 1000) * 1000000L}};
        timerfd_settime(timer_fd, 0, &its, NULL);
    } else if (value == 0) {
        if (state == PENDING) {
            disarm_timer();
            emit(clone_fd, EV_KEY, button, 1); syn(clone_fd);
            emit(clone_fd, EV_KEY, button, 0); syn(clone_fd);
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
    }
    // value 2 (autorepeat) is ignored
}

// After SYN_DROPPED: make the clone's button state match the hardware again.
static void resync(void) {
    unsigned long keys[NBITS(KEY_CNT)] = {0};
    if (ioctl(src, EVIOCGKEY(sizeof keys), keys) < 0) return;
    bool any = false;
    for (int c = 0; c < KEY_CNT; c++) {
        if (c == button) continue;
        bool hw = TEST_BIT(c, keys), cl = TEST_BIT(c, clone_keys);
        if (hw != cl) { emit(clone_fd, EV_KEY, c, hw); set_clone_key(c, hw); any = true; }
    }
    if (any) syn(clone_fd);
    if (!TEST_BIT(button, keys) && state != IDLE) on_button(0);
}

static void read_source(void) {
    struct input_event evs[256];
    for (;;) {
        ssize_t n = read(src, evs, sizeof evs);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            logmsg("mouse read: %s (unplugged?)", strerror(errno));
            release_all();
            close(src);
            src = -1;
            want_grab_retry = true;
            return;
        }
        if (n == 0) return;
        size_t count = (size_t)n / sizeof *evs;
        for (size_t i = 0; i < count; i++) {
            struct input_event *ev = &evs[i];
            if (ev->type == EV_SYN && ev->code == SYN_DROPPED) { dropping = true; continue; }
            if (dropping) {
                if (ev->type == EV_SYN && ev->code == SYN_REPORT) { dropping = false; resync(); }
                continue;
            }
            if (ev->type == EV_KEY && ev->code == button) { on_button(ev->value); continue; }
            if (state != IDLE) {
                bool other = (ev->type == EV_KEY && ev->value == 1) ||
                             (ev->type == EV_REL && (ev->code == REL_WHEEL || ev->code == REL_HWHEEL ||
                                                     ev->code == REL_WHEEL_HI_RES || ev->code == REL_HWHEEL_HI_RES));
                if (other) {
                    used = true;
                    if (state == PENDING) resolve_hold();
                }
            }
            if (ev->type == EV_KEY) set_clone_key(ev->code, ev->value);
            if (write(clone_fd, ev, sizeof *ev) != sizeof *ev && errno != EAGAIN)
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
    else { logmsg("unknown command: %s", line); return; }
    dbg("cmd: %s", line);
}

static void drop_client(void) {
    if (client_fd < 0) return;
    close(client_fd);
    client_fd = -1;
    client_len = 0;
    set_pass(true);   // no extension = no game detection: stay native
}

static void read_client(void) {
    ssize_t n = read(client_fd, client_buf + client_len, sizeof client_buf - 1 - client_len);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        logmsg("extension disconnected");
        drop_client();
        return;
    }
    client_len += (size_t)n;
    client_buf[client_len] = 0;
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
    logmsg("holdsuper ready: button %d, waiting for the extension", button);

    while (!quit) {
        struct pollfd pf[4];
        int n = 0, i_src = -1, i_client = -1;
        pf[n++] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
        pf[n++] = (struct pollfd){.fd = timer_fd, .events = POLLIN};
        if (src >= 0) { i_src = n; pf[n++] = (struct pollfd){.fd = src, .events = POLLIN}; }
        if (client_fd >= 0) { i_client = n; pf[n++] = (struct pollfd){.fd = client_fd, .events = POLLIN}; }

        int r = poll(pf, n, want_grab_retry ? 1000 : -1);
        if (r < 0) { if (errno == EINTR) continue; logmsg("poll: %s", strerror(errno)); break; }
        if (r == 0) { try_grab(); continue; }

        if (pf[1].revents & POLLIN) {
            uint64_t exp;
            if (read(timer_fd, &exp, sizeof exp) == sizeof exp && state == PENDING) resolve_hold();
        }
        if (i_src >= 0 && pf[i_src].revents) read_source();
        if (i_client >= 0 && pf[i_client].revents) read_client();
        if (pf[0].revents & POLLIN) {
            int c = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (c >= 0) {
                if (client_fd >= 0) { close(client_fd); client_len = 0; }   // newest extension wins
                client_fd = c;
                logmsg("extension connected");
            }
        }
    }

    close_source();
    if (clone_fd >= 0) { ioctl(clone_fd, UI_DEV_DESTROY); close(clone_fd); }
    ioctl(kbd_fd, UI_DEV_DESTROY);
    close(kbd_fd);
    return 0;
}
