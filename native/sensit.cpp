// sensit: no-root touch sensitivity booster.
// Grabs the real touchscreen (EVIOCGRAB), scales finger movement, and re-emits
// events through /dev/uinput. If this process dies, the kernel releases the
// grab automatically and normal touch returns.
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXSLOT 32
#define BITS(n) ((n) / 8 + 1)

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

// ---------- config (key=value file, sens in 1.0..5.0, axis 0=all 1=up/down 2=left/right)
struct Config { int enabled = 1; int pm = 1000; int axis = 0; };  // pm = sensitivity in per-mille

static void load_config(const char *path, Config &c) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (*v < '0' || *v > '9') continue;  // ignore malformed values, keep previous
        if (!strcmp(line, "enabled")) c.enabled = atoi(v) != 0;
        else if (!strcmp(line, "sens")) {
            int pm = (int)(atof(v) * 1000.0 + 0.5);
            c.pm = pm < 1000 ? 1000 : (pm > 5000 ? 5000 : pm);
        } else if (!strcmp(line, "axis")) {
            int a = atoi(v);
            c.axis = (a < 0 || a > 2) ? 0 : a;
        }
    }
    fclose(f);
}

// ---------- device setup
static bool has_bit(const unsigned char *b, int n) { return b[n / 8] & (1 << (n % 8)); }

static input_absinfo g_ai[ABS_CNT];
static bool g_vert_is_x = true;  // landscape up/down maps to raw X on portrait-native panels

static int find_touch(char *path, size_t len) {
    for (int i = 0; i < 32; i++) {
        snprintf(path, len, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char bits[BITS(ABS_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof bits), bits) >= 0 && has_bit(bits, ABS_MT_SLOT) &&
            has_bit(bits, ABS_MT_POSITION_X) && has_bit(bits, ABS_MT_POSITION_Y))
            return fd;
        close(fd);
    }
    return -1;
}

static int create_clone(int src) {
    int u = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (u < 0) { fprintf(stderr, "ERR open /dev/uinput errno=%d\n", errno); return -1; }
    unsigned char bits[BITS(KEY_MAX)];
    bool have[ABS_CNT] = {false};

    if (ioctl(u, UI_SET_EVBIT, EV_SYN) < 0 || ioctl(u, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(u, UI_SET_EVBIT, EV_ABS) < 0) {
        fprintf(stderr, "ERR uinput ioctl blocked errno=%d\n", errno);
        close(u);
        return -1;
    }
    memset(bits, 0, sizeof bits);
    ioctl(src, EVIOCGBIT(EV_KEY, sizeof bits), bits);
    for (int k = 0; k <= KEY_MAX; k++) if (has_bit(bits, k)) ioctl(u, UI_SET_KEYBIT, k);

    memset(bits, 0, sizeof bits);
    ioctl(src, EVIOCGBIT(EV_ABS, sizeof bits), bits);
    for (int a = 0; a <= ABS_MAX; a++)
        if (has_bit(bits, a)) {
            ioctl(u, UI_SET_ABSBIT, a);
            ioctl(src, EVIOCGABS(a), &g_ai[a]);
            have[a] = true;
        }

    memset(bits, 0, sizeof bits);
    if (ioctl(src, EVIOCGPROP(sizeof bits), bits) >= 0)
        for (int p = 0; p <= INPUT_PROP_MAX; p++) if (has_bit(bits, p)) ioctl(u, UI_SET_PROPBIT, p);

    input_id id;
    memset(&id, 0, sizeof id);
    ioctl(src, EVIOCGID, &id);
    char name[80] = "touch";
    ioctl(src, EVIOCGNAME(sizeof name - 1), name);

    uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id = id;
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    if (ioctl(u, UI_DEV_SETUP, &us) == 0) {
        for (int a = 0; a <= ABS_MAX; a++)
            if (have[a]) {
                uinput_abs_setup as;
                memset(&as, 0, sizeof as);
                as.code = a;
                as.absinfo = g_ai[a];
                ioctl(u, UI_ABS_SETUP, &as);
            }
    } else {  // legacy kernels
        uinput_user_dev ud;
        memset(&ud, 0, sizeof ud);
        snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "%s", name);
        ud.id = id;
        for (int a = 0; a <= ABS_MAX; a++)
            if (have[a]) {
                ud.absmin[a] = g_ai[a].minimum;
                ud.absmax[a] = g_ai[a].maximum;
                ud.absfuzz[a] = g_ai[a].fuzz;
                ud.absflat[a] = g_ai[a].flat;
            }
        if (write(u, &ud, sizeof ud) < 0) { fprintf(stderr, "ERR uinput setup errno=%d\n", errno); close(u); return -1; }
    }
    if (ioctl(u, UI_DEV_CREATE) < 0) { fprintf(stderr, "ERR UI_DEV_CREATE errno=%d\n", errno); close(u); return -1; }
    return u;
}

// ---------- scaling
// Per finger and axis. Movement is scaled incrementally (with fractional carry), so the
// pointer reacts immediately after hitting a screen edge. The factor is fixed at touch-down,
// so changing the slider never makes a finger that is already down jump.
struct Axis { int last, out, rem, pm; bool have; };
struct Slot { Axis x, y; };
static Slot slots[MAXSLOT + 1];  // last entry absorbs out-of-range slot numbers
static int cur = 0;

static int track(int v, Axis &a, int lo, int hi) {
    if (a.pm <= 1000) return v;
    if (!a.have) { a.have = true; a.last = a.out = v; a.rem = 0; return v; }
    int t = (v - a.last) * a.pm + a.rem;
    a.last = v;
    int s = t / 1000;
    a.rem = t - s * 1000;
    a.out += s;
    if (a.out < lo) a.out = lo; else if (a.out > hi) a.out = hi;
    return a.out;
}

static void start_touch(Slot &s, const Config *c) {
    s = Slot();
    if (!c || !c->enabled) return;
    bool all = c->axis == 0;
    bool sx = all || (g_vert_is_x ? c->axis == 1 : c->axis == 2);
    bool sy = all || (g_vert_is_x ? c->axis == 2 : c->axis == 1);
    if (sx) s.x.pm = c->pm;
    if (sy) s.y.pm = c->pm;
}

static void handle(input_event &e, const Config &c) {
    if (e.type != EV_ABS) return;
    Slot &s = slots[cur];
    switch (e.code) {
        case ABS_MT_SLOT: cur = (e.value >= 0 && e.value < MAXSLOT) ? e.value : MAXSLOT; break;
        case ABS_MT_TRACKING_ID: start_touch(s, (e.value >= 0 && cur < MAXSLOT) ? &c : nullptr); break;
        case ABS_MT_POSITION_X:
            e.value = track(e.value, s.x, g_ai[ABS_MT_POSITION_X].minimum, g_ai[ABS_MT_POSITION_X].maximum); break;
        case ABS_MT_POSITION_Y:
            e.value = track(e.value, s.y, g_ai[ABS_MT_POSITION_Y].minimum, g_ai[ABS_MT_POSITION_Y].maximum); break;
    }
}

// ---------- helpers
static bool write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w; len -= (size_t)w;
    }
    return true;
}

static bool already_running(const char *pidpath) {
    FILE *f = fopen(pidpath, "r");
    if (!f) return false;
    int p = 0;
    bool ok = fscanf(f, "%d", &p) == 1;
    fclose(f);
    if (!ok || p <= 1 || kill(p, 0) != 0) return false;
    char cp[64], comm[32] = {0};
    snprintf(cp, sizeof cp, "/proc/%d/comm", p);
    FILE *c = fopen(cp, "r");
    if (!c) return false;
    if (!fgets(comm, sizeof comm, c)) comm[0] = 0;
    fclose(c);
    return strncmp(comm, "sensit", 6) == 0;
}

static void write_pid(const char *pidpath) {
    if (FILE *pf = fopen(pidpath, "w")) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
}

int main(int argc, char **argv) {
    const char *cfgpath = "config", *pidpath = "sensit.pid", *dev = nullptr;
    bool daemon = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--daemon")) daemon = true;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgpath = argv[++i];
        else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) pidpath = argv[++i];
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
    }
    if (already_running(pidpath)) { fprintf(stderr, "ERR already running\n"); return 1; }

    char path[64];
    int src;
    if (dev) { snprintf(path, sizeof path, "%s", dev); src = open(path, O_RDONLY | O_CLOEXEC); }
    else src = find_touch(path, sizeof path);
    if (src < 0) { fprintf(stderr, "ERR touchscreen not found errno=%d\n", errno); return 2; }

    int u = create_clone(src);
    if (u < 0) return 3;
    const input_absinfo &ax = g_ai[ABS_MT_POSITION_X], &ay = g_ai[ABS_MT_POSITION_Y];
    g_vert_is_x = (ax.maximum - ax.minimum) < (ay.maximum - ay.minimum);

    usleep(300000);  // let Android pick up the virtual device before grabbing the real one
    if (ioctl(src, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "ERR EVIOCGRAB errno=%d\n", errno);
        ioctl(u, UI_DEV_DESTROY);
        return 4;
    }

    if (daemon) {
        int pp[2];
        if (pipe2(pp, O_CLOEXEC) < 0) { fprintf(stderr, "ERR pipe errno=%d\n", errno); return 5; }
        pid_t p = fork();
        if (p > 0) {  // launcher: report only after the daemon is really up
            close(pp[1]);
            char b = 0;
            ssize_t n;
            do { n = read(pp[0], &b, 1); } while (n < 0 && errno == EINTR);
            if (n == 1) { printf("OK dev=%s vert_is_x=%d\n", path, (int)g_vert_is_x); fflush(stdout); _exit(0); }
            fprintf(stderr, "ERR daemon failed to start\n");
            _exit(5);
        }
        setsid();
        if (fork() > 0) _exit(0);
        int n = open("/dev/null", O_RDWR);
        if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); }
        write_pid(pidpath);
        (void)!write(pp[1], "1", 1);
        close(pp[1]);
    } else {
        write_pid(pidpath);
        printf("OK dev=%s vert_is_x=%d\n", path, (int)g_vert_is_x);
        fflush(stdout);
    }

    setpriority(PRIO_PROCESS, 0, -8);  // best effort, ignored if not permitted
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    Config cfg;
    load_config(cfgpath, cfg);

    // Reload config via inotify (config is replaced atomically by rename). Falls back to polling mtime.
    char dir[256];
    snprintf(dir, sizeof dir, "%s", cfgpath);
    char *slash = strrchr(dir, '/');
    const char *base = cfgpath;
    if (slash) { *slash = 0; base = slash + 1; if (!dir[0]) snprintf(dir, sizeof dir, "/"); }
    else snprintf(dir, sizeof dir, ".");
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd >= 0 && inotify_add_watch(ifd, dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) { close(ifd); ifd = -1; }
    struct stat st;
    long m_s = 0, m_ns = 0;
    if (ifd < 0 && !stat(cfgpath, &st)) { m_s = st.st_mtim.tv_sec; m_ns = st.st_mtim.tv_nsec; }

    input_event in[64];
    pollfd pfd[2] = {{src, POLLIN, 0}, {ifd, POLLIN, 0}};
    while (!g_stop) {
        int r = poll(pfd, 2, ifd >= 0 ? 1000 : 250);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) {
            if (ifd < 0 && !stat(cfgpath, &st) && (st.st_mtim.tv_sec != m_s || st.st_mtim.tv_nsec != m_ns)) {
                m_s = st.st_mtim.tv_sec; m_ns = st.st_mtim.tv_nsec;
                load_config(cfgpath, cfg);
            }
            continue;
        }
        if (pfd[1].revents & POLLIN) {
            char ib[512] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t n = read(ifd, ib, sizeof ib);
            bool reload = false;
            for (char *q = ib; n > 0 && q < ib + n;) {
                inotify_event *ev = (inotify_event *)q;
                if (ev->len && !strcmp(ev->name, base)) reload = true;
                q += sizeof(inotify_event) + ev->len;
            }
            if (reload) load_config(cfgpath, cfg);
        }
        if (pfd[0].revents & POLLIN) {
            ssize_t n = read(src, in, sizeof in);
            if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
            int cnt = (int)(n / (ssize_t)sizeof(input_event));
            for (int i = 0; i < cnt; i++) handle(in[i], cfg);
            if (!write_all(u, in, (size_t)cnt * sizeof(input_event))) break;
        }
        if (pfd[0].revents & ~POLLIN) break;  // device gone: exit, grab is released
    }
    ioctl(src, EVIOCGRAB, 0);
    ioctl(u, UI_DEV_DESTROY);
    unlink(pidpath);
    return 0;
}
