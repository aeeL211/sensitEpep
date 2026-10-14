// sensit: no-root touch sensitivity booster that only acts while a target game
// (Free Fire / Free Fire MAX) is in the foreground.
//
// IDLE   : nothing grabbed, no touch events read. Wakes once per 3 s to check the game.
// ACTIVE : real touchscreen grabbed (EVIOCGRAB), movement scaled, events re-emitted via uinput.
// If this process dies the kernel releases the grab and normal touch returns.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXSLOT 32
#define MAXPID 8192
#define TICK_MS 3000
#define BITS(n) ((n) / 8 + 1)

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static char g_dir[256] = ".";  // directory of the config file (the plugin folder)

// ---------------------------------------------------------------- config
// sens_h / sens_v : 1.0..5.0, screen axes while the game is landscape (h = left-right, v = up-down)
// zone            : 0 whole screen, 1 left half, 2 right half     orient: 1 or 3 (which landscape)
// smooth          : 1 = One Euro Filter, strength / resp 1..100 (%)
// filter          : 1 = only while the game is in the foreground
struct Config {
    float kh = 1.0f, kv = 1.0f;
    int zone = 0, orient = 1;
    int smooth = 0, strength = 50, resp = 50;
    int filter = 1;
    float fcmin = 3.0f, beta = 5.0f;  // derived One Euro parameters
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// strength   -> min cutoff: 20 Hz (light) .. 0.5 Hz (heavy), how much slow movement is calmed
// resp       -> beta: 0.5 .. 60 Hz per (panel width / second), how fast the filter opens up on fast swipes
static void derive(Config &c) {
    float s = (float)(c.strength - 1) / 99.0f, r = (float)(c.resp - 1) / 99.0f;
    c.fcmin = 20.0f * powf(0.025f, s);
    c.beta = 0.5f * powf(120.0f, r);
}

static void load_config(const char *path, Config &c) {
    if (FILE *f = fopen(path, "r")) {
        char line[64];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = line, *v = eq + 1;
            if (*v < '0' || *v > '9') continue;  // ignore malformed values, keep previous
            if (!strcmp(k, "sens_h")) c.kh = clampf((float)atof(v), 1.0f, 5.0f);
            else if (!strcmp(k, "sens_v")) c.kv = clampf((float)atof(v), 1.0f, 5.0f);
            else if (!strcmp(k, "zone")) c.zone = clampi(atoi(v), 0, 2);
            else if (!strcmp(k, "orient")) c.orient = atoi(v) == 3 ? 3 : 1;
            else if (!strcmp(k, "smooth")) c.smooth = atoi(v) != 0;
            else if (!strcmp(k, "strength")) c.strength = clampi(atoi(v), 1, 100);
            else if (!strcmp(k, "resp")) c.resp = clampi(atoi(v), 1, 100);
            else if (!strcmp(k, "filter")) c.filter = atoi(v) != 0;
        }
        fclose(f);
    }
    derive(c);
}

// ---------------------------------------------------------------- foreground detection
// No dumpsys. /proc is fully visible to the shell user (group readproc). The game's main
// process is named after its package, and the top app has oom_score_adj <= 0.
//  - not running : every 3 s list /proc and look only at pids that are new since the last tick
//  - running     : one pread() of an already-open /proc/<pid>/oom_score_adj per tick
static const char *const kPkgs[] = {"com.dts.freefireth", "com.dts.freefiremax"};

static int g_adj_fd = -1;
static int g_prev[MAXPID], g_snap[MAXPID], g_cur[MAXPID];
static int g_prev_n = 0;

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int read_pids(int *out, int max) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    int n = 0;
    while (dirent *e = readdir(d)) {
        char c = e->d_name[0];
        if (c < '1' || c > '9') continue;
        if (n < max) out[n++] = atoi(e->d_name);
    }
    closedir(d);
    for (int i = 1; i < n; i++)  // procfs lists pids ascending; sort only if that ever changes
        if (out[i - 1] > out[i]) { qsort(out, (size_t)n, sizeof(int), cmp_int); break; }
    return n;
}

// 1 = target game, 0 = not it (final), -1 = process not renamed yet, look again next tick
static int check_pid(int pid) {
    char p[32], buf[64];
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;  // kernel thread / zombie
    buf[n] = 0;
    for (const char *pkg : kPkgs)
        if (!strcmp(buf, pkg)) return 1;
    // freshly forked from zygote (or the USAP pool): name is not set yet
    if (!strncmp(buf, "zygote", 6) || buf[0] == '<') return -1;
    return 0;
}

static bool find_game() {
    int n = read_pids(g_cur, MAXPID);
    if (n < 0) return false;
    int sn = 0, j = 0;
    for (int i = 0; i < n; i++) {
        int pid = g_cur[i];
        while (j < g_prev_n && g_prev[j] < pid) j++;
        if (j < g_prev_n && g_prev[j] == pid) { g_snap[sn++] = pid; continue; }  // seen before
        int r = check_pid(pid);
        if (r == 1 && g_adj_fd < 0) {
            char p[40];
            snprintf(p, sizeof p, "/proc/%d/oom_score_adj", pid);
            g_adj_fd = open(p, O_RDONLY | O_CLOEXEC);
            if (g_adj_fd < 0) continue;  // could not open yet: leave it out so it is retried
        }
        if (r >= 0) g_snap[sn++] = pid;
    }
    memcpy(g_prev, g_snap, (size_t)sn * sizeof(int));
    g_prev_n = sn;
    return g_adj_fd >= 0;
}

static bool game_foreground() {
    if (g_adj_fd < 0 && !find_game()) return false;
    char b[16];
    ssize_t n = pread(g_adj_fd, b, sizeof b - 1, 0);
    if (n <= 0) {  // process died
        close(g_adj_fd);
        g_adj_fd = -1;
        return false;
    }
    b[n] = 0;
    return atoi(b) <= 0;
}

// ---------------------------------------------------------------- device setup
static bool has_bit(const unsigned char *b, int n) { return b[n / 8] & (1 << (n % 8)); }

static input_absinfo g_ai[ABS_CNT];
static bool g_vert_is_x = true;  // landscape up/down maps to raw X on portrait-native panels
static char g_srcpath[64];
static int g_uin = -1;  // virtual touchscreen, created once, lives as long as the daemon
static int g_src = -1;  // real touchscreen, only open while ACTIVE

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

// ---------------------------------------------------------------- scaling + stabilizing
// Per finger and axis: optional One Euro Filter on the raw position (Casiez et al., CHI 2012),
// then incremental scaling of the filtered movement. The pointer reacts immediately after hitting
// a screen edge. Per-touch settings are fixed at touch-down, so a finger that is already down
// never jumps when a slider changes.
struct Axis { float k; bool filt, have; float raw, xhat, dxhat, fprev, out; double t; };
struct Slot { Axis x, y; bool zk; };
static Slot slots[MAXSLOT + 1];  // last entry absorbs out-of-range slot numbers
static int cur = 0;
static uint64_t g_down = 0;          // bitmask of fingers currently down
static float g_inv_ref = 1.0f / 720;  // 1 / short panel side: makes filter speed resolution independent

static float lp_alpha(float cutoff, float dt) {
    float tau = 1.0f / (6.2831853f * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

static int track(int v, double t, Axis &a, int lo, int hi, const Config &c) {
    if (!a.filt && a.k <= 1.0f) return v;  // nothing to do on this axis
    float x = (float)v;
    if (!a.have) {
        a.have = true;
        a.raw = a.xhat = a.fprev = a.out = x;
        a.dxhat = 0.0f;
        a.t = t;
        return v;
    }
    float dt = (float)(t - a.t);
    dt = dt < 0.001f ? 0.001f : (dt > 0.1f ? 0.1f : dt);
    float fx = x;
    if (a.filt) {
        float dx = (x - a.raw) / dt;
        a.dxhat += lp_alpha(1.0f, dt) * (dx - a.dxhat);                       // derivative cutoff 1 Hz
        float cutoff = c.fcmin + c.beta * fabsf(a.dxhat) * g_inv_ref;         // adaptive cutoff
        a.xhat += lp_alpha(cutoff, dt) * (x - a.xhat);
        fx = a.xhat;
    }
    a.raw = x;
    a.t = t;
    a.out += (fx - a.fprev) * a.k;
    a.fprev = fx;
    if (a.out < (float)lo) a.out = (float)lo; else if (a.out > (float)hi) a.out = (float)hi;
    return (int)(a.out + 0.5f);
}

static void start_touch(Slot &s, const Config *c) {
    s = Slot();
    if (!c) return;
    s.x.k = g_vert_is_x ? c->kv : c->kh;  // portrait-native panel: raw X is screen up-down in landscape
    s.y.k = g_vert_is_x ? c->kh : c->kv;
    s.x.filt = s.y.filt = c->smooth != 0;
    s.zk = c->zone == 0;  // left/right is decided on the first horizontal sample of the touch
}

static void decide_zone(Slot &s, int v, const input_absinfo &r, const Config &c) {
    s.zk = true;
    int span = r.maximum - r.minimum;
    if (c.zone == 0 || span <= 0) return;
    bool low_half = (v - r.minimum) * 2 < span;
    bool left_is_low = c.orient == 1;  // landscape 90: screen-left = low raw value on the long axis
    bool in = (c.zone == 1) == (low_half == left_is_low);
    if (!in) { s.x.k = s.y.k = 1.0f; s.x.filt = s.y.filt = false; }  // outside the zone: untouched input
}

static void handle(input_event &e, const Config &c) {
    if (e.type != EV_ABS) return;
    Slot &s = slots[cur];
    double t = (double)e.time.tv_sec + (double)e.time.tv_usec * 1e-6;
    switch (e.code) {
        case ABS_MT_SLOT: cur = (e.value >= 0 && e.value < MAXSLOT) ? e.value : MAXSLOT; break;
        case ABS_MT_TRACKING_ID:
            if (cur < MAXSLOT) {
                if (e.value >= 0) g_down |= 1ull << cur; else g_down &= ~(1ull << cur);
            }
            start_touch(s, (e.value >= 0 && cur < MAXSLOT) ? &c : nullptr);
            break;
        case ABS_MT_POSITION_X:
            if (!s.zk && !g_vert_is_x) decide_zone(s, e.value, g_ai[ABS_MT_POSITION_X], c);
            e.value = track(e.value, t, s.x, g_ai[ABS_MT_POSITION_X].minimum, g_ai[ABS_MT_POSITION_X].maximum, c);
            break;
        case ABS_MT_POSITION_Y:
            if (!s.zk && g_vert_is_x) decide_zone(s, e.value, g_ai[ABS_MT_POSITION_Y], c);
            e.value = track(e.value, t, s.y, g_ai[ABS_MT_POSITION_Y].minimum, g_ai[ABS_MT_POSITION_Y].maximum, c);
            break;
    }
}

// ---------------------------------------------------------------- IDLE <-> ACTIVE
static bool finger_down_on_real(int fd) {
    int ns = g_ai[ABS_MT_SLOT].maximum + 1;
    if (ns < 1) ns = 1;
    if (ns > MAXSLOT) ns = MAXSLOT;
    struct { __u32 code; __s32 values[MAXSLOT]; } req;
    memset(&req, 0, sizeof req);
    req.code = ABS_MT_TRACKING_ID;
    if (ioctl(fd, EVIOCGMTSLOTS(sizeof(__u32) + (size_t)ns * sizeof(__s32)), &req) < 0) return false;
    for (int i = 0; i < ns; i++) if (req.values[i] >= 0) return true;
    return false;
}

static bool activate() {
    int fd = open(g_srcpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    // never grab in the middle of a touch, otherwise Android is left with a stuck finger
    if (finger_down_on_real(fd) || ioctl(fd, EVIOCGRAB, 1) < 0) { close(fd); return false; }
    input_event tmp[64];
    while (read(fd, tmp, sizeof tmp) > 0) {}  // drop whatever was queued before the grab
    memset(slots, 0, sizeof slots);
    cur = 0;
    g_down = 0;
    g_src = fd;
    return true;
}

static void deactivate() {
    ioctl(g_src, EVIOCGRAB, 0);
    close(g_src);
    g_src = -1;
}

// ---------------------------------------------------------------- state file + notification hook
static void write_state(const char *s) {
    char p[300];
    snprintf(p, sizeof p, "%s/sensit.state", g_dir);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

// Runs <plugin dir>/notify.sh on|off <sens_h> <sens_v>, detached. Only called on state changes.
static void notify(bool on, const Config &c) {
    char script[300];
    snprintf(script, sizeof script, "%s/notify.sh", g_dir);
    if (access(script, R_OK) != 0) return;
    char sh[16], sv[16];
    snprintf(sh, sizeof sh, "%.1f", (double)c.kh);
    snprintf(sv, sizeof sv, "%.1f", (double)c.kv);
    if (fork() == 0) {
        signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGHUP, SIG_DFL);
        execl("/system/bin/sh", "sh", script, on ? "on" : "off", sh, sv, (char *)nullptr);
        _exit(127);
    }
}

static void on_state(bool active, const Config &c) {
    write_state(active ? "active" : "idle");
    notify(active, c);
}

// ---------------------------------------------------------------- helpers
static int64_t mono_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

    // config dir (also where state file and notify.sh live)
    char base[128] = "config";
    snprintf(g_dir, sizeof g_dir, "%s", cfgpath);
    if (char *slash = strrchr(g_dir, '/')) {
        snprintf(base, sizeof base, "%s", slash + 1);
        *slash = 0;
        if (!g_dir[0]) snprintf(g_dir, sizeof g_dir, "/");
    } else {
        snprintf(g_dir, sizeof g_dir, ".");
    }
    Config cfg;
    load_config(cfgpath, cfg);

    int src;
    if (dev) { snprintf(g_srcpath, sizeof g_srcpath, "%s", dev); src = open(g_srcpath, O_RDONLY | O_CLOEXEC); }
    else src = find_touch(g_srcpath, sizeof g_srcpath);
    if (src < 0) { fprintf(stderr, "ERR touchscreen not found errno=%d\n", errno); return 2; }
    g_uin = create_clone(src);
    close(src);
    if (g_uin < 0) return 3;
    const input_absinfo &ax = g_ai[ABS_MT_POSITION_X], &ay = g_ai[ABS_MT_POSITION_Y];
    g_vert_is_x = (ax.maximum - ax.minimum) < (ay.maximum - ay.minimum);
    { int rx = ax.maximum - ax.minimum, ry = ay.maximum - ay.minimum, ref = rx < ry ? rx : ry;
      if (ref > 0) g_inv_ref = 1.0f / (float)ref; }

    int procs = read_pids(g_cur, MAXPID);
    if (cfg.filter && procs < 20) {  // /proc hidden (hidepid): game detection could never work
        fprintf(stderr, "ERR /proc not visible (pids=%d), set filter=0 to bypass\n", procs);
        ioctl(g_uin, UI_DEV_DESTROY);
        return 6;
    }
    usleep(300000);  // let Android pick up the virtual device before the first grab

    if (daemon) {
        int pp[2];
        if (pipe2(pp, O_CLOEXEC) < 0) { fprintf(stderr, "ERR pipe errno=%d\n", errno); return 5; }
        if (fork() > 0) {  // launcher: report only after the daemon is really up
            close(pp[1]);
            char b = 0;
            ssize_t n;
            do { n = read(pp[0], &b, 1); } while (n < 0 && errno == EINTR);
            if (n == 1) {
                printf("OK dev=%s vert_is_x=%d filter=%d procs=%d\n", g_srcpath, (int)g_vert_is_x, cfg.filter, procs);
                fflush(stdout);
                _exit(0);
            }
            fprintf(stderr, "ERR daemon failed to start\n");
            _exit(5);
        }
        setsid();
        if (fork() > 0) _exit(0);
        int n = open("/dev/null", O_RDWR);
        if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); }
        write_pid(pidpath);
        write_state("idle");
        (void)!write(pp[1], "1", 1);
        close(pp[1]);
    } else {
        write_pid(pidpath);
        write_state("idle");
        printf("OK dev=%s vert_is_x=%d filter=%d procs=%d\n", g_srcpath, (int)g_vert_is_x, cfg.filter, procs);
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
    signal(SIGCHLD, SIG_IGN);  // detached notify.sh children are reaped automatically

    // Config is replaced atomically by rename; inotify tells us. Fallback: re-check mtime on each tick.
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd >= 0 && inotify_add_watch(ifd, g_dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) { close(ifd); ifd = -1; }
    struct stat st;
    long m_s = 0, m_ns = 0;
    if (!stat(cfgpath, &st)) { m_s = st.st_mtim.tv_sec; m_ns = st.st_mtim.tv_nsec; }

    bool want = false;
    int64_t next_tick = 0;
    input_event in[64];
    pollfd pfd[2];
    while (!g_stop) {
        int64_t now = mono_ms();
        if (now >= next_tick) {
            next_tick = now + TICK_MS;
            if (ifd < 0 && !stat(cfgpath, &st) && (st.st_mtim.tv_sec != m_s || st.st_mtim.tv_nsec != m_ns)) {
                m_s = st.st_mtim.tv_sec; m_ns = st.st_mtim.tv_nsec;
                load_config(cfgpath, cfg);
            }
            want = !cfg.filter || game_foreground();
            if (want && g_src < 0) {
                if (activate()) on_state(true, cfg);
                else next_tick = now + 250;  // a finger is down right now: try again shortly
            }
        }
        if (g_src >= 0 && !want && g_down == 0) {  // leave only when no finger is down
            deactivate();
            on_state(false, cfg);
        }

        int64_t wait = next_tick - now;
        pfd[0].fd = g_src; pfd[0].events = POLLIN; pfd[0].revents = 0;  // fd -1 is ignored by poll
        pfd[1].fd = ifd;   pfd[1].events = POLLIN; pfd[1].revents = 0;
        int r = poll(pfd, 2, (int)(wait < 0 ? 0 : (wait > TICK_MS ? TICK_MS : wait)));
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;

        if (pfd[1].revents & POLLIN) {
            char ib[512] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t n = read(ifd, ib, sizeof ib);
            bool reload = false;
            for (char *q = ib; n > 0 && q < ib + n;) {
                inotify_event *ev = (inotify_event *)q;
                if (ev->len && !strcmp(ev->name, base)) reload = true;
                q += sizeof(inotify_event) + ev->len;
            }
            if (reload) { load_config(cfgpath, cfg); next_tick = 0; }  // re-evaluate right away
        }
        if (g_src >= 0) {
            if (pfd[0].revents & POLLIN) {
                ssize_t n = read(g_src, in, sizeof in);
                if (n < 0) {
                    if (errno != EINTR && errno != EAGAIN) break;
                } else {
                    int cnt = (int)(n / (ssize_t)sizeof(input_event));
                    for (int i = 0; i < cnt; i++) handle(in[i], cfg);
                    if (!write_all(g_uin, in, (size_t)cnt * sizeof(input_event))) break;
                }
            }
            if (pfd[0].revents & ~POLLIN) break;  // device gone: exit, grab is released
        }
    }
    if (g_src >= 0) { deactivate(); notify(false, cfg); }
    ioctl(g_uin, UI_DEV_DESTROY);
    unlink(pidpath);
    char sp[300];
    snprintf(sp, sizeof sp, "%s/sensit.state", g_dir);
    unlink(sp);
    return 0;
}
