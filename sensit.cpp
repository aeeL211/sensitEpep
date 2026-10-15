// sensit: no-root touch sensitivity booster for free fire / free fire max.
// runs as the adb shell user (uid 2000) inside an axmanager plugin, no root needed.
//
// states
//   idle   : nothing grabbed, no touch events read, wakes once per 3 s to look for the game
//   active : game in the foreground -> the real touchscreen is grabbed (eviocgrab), finger
//            movement is scaled (and optionally smoothed) and re-emitted through a uinput clone
//
// safety rules (keep them)
//   - if this process dies the kernel drops the grab and normal touch returns
//   - never grab while a finger is down, and never grab if that cannot be checked
//   - release the grab only when no virtual finger is down (after a syn_dropped the fingers are
//     lifted explicitly first)
//   - no dumpsys, ps or other child process in the 3 s polling path
//
// cli
//   sensit --daemon --config <file> --pidfile <file> [--dev /dev/input/event<n>]
//   prints an ok line once the daemon is up, or an err line plus exit code:
//   1 already running, 2 touchscreen not found, 3 uinput failure, 5 daemonize failure, 6 /proc hidden
//
// files (next to the config)
//   config        key=value, see below, reloaded on change (inotify, mtime fallback)
//   sensit.pid    pid of the daemon
//   sensit.state  idle | active | error:<reason>
//   notify.sh     optional hook, called as: notify.sh on|off <sens_h> <sens_v>
//
// build: arm64 and armv7a (32-bit), static libstdc++, see .github/workflows/build.yml
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
#define RETRY_MS 250
#define BITS(n) ((n) / 8 + 1)
#define PKBUF 1024  // config value buffer for the package list
#define MAXPK 16    // packages kept from the list, also max tracked processes
#define PKLEN 128   // longest package name + 1
#define DEFPK "com.dts.freefireth,com.dts.freefiremax"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static char g_dir[256] = ".";  // plugin folder (where the config lives)

// ---------------------------------------------------------------- config
// keys (all optional, malformed values are ignored)
//   sens_h, sens_v : 1.0..5.0, screen axes while the game is landscape (h = left-right, v = up-down)
//   zone           : 0 whole screen, 1 left half, 2 right half
//   orient         : 1 or 3, which landscape rotation (1 = screen-left is the low raw value)
//   smooth         : 1 = one euro filter on, strength / resp 1..100 (%)
//   filter         : 1 = only while one of the packages is in the foreground, 0 = always
//   pkgs           : comma separated package names for filter=1 (letters, digits, _ and . only,
//                    16 max), empty or invalid falls back to the two free fire packages
// the scaled position is never clamped to the panel, so a swipe cannot stall at the edge.
// k, smooth and zone are fixed when a finger touches down, so a finger already on the
// screen never jumps. strength / resp apply live, the filter is continuous so nothing jumps.
// defaults (strength 40, resp 85) come from simulating free fire style aiming: about half of
// the resting jitter is removed while a fast flick is delayed by less than one touch frame.
struct Cfg {
  float kh = 1.0f, kv = 1.0f;
  int zone = 0, orient = 1;
  int smooth = 0, strength = 40, resp = 85;
  int filter = 1;
  char pk[PKBUF] = DEFPK;
  float fcmin = 3.0f, beta = 5.0f;  // derived one euro parameters
};

static void set_pk(const char *s);  // applies the package list, see foreground detection

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// strength -> min cutoff, 20 hz (light) .. 0.5 hz (heavy)
// resp     -> beta, 0.5 .. 60 hz per (panel width / s), how fast the filter opens on fast swipes
static void derive(Cfg &c) {
  float s = (float)(c.strength - 1) / 99.0f, r = (float)(c.resp - 1) / 99.0f;
  c.fcmin = 20.0f * powf(0.025f, s);
  c.beta = 0.5f * powf(120.0f, r);
}

static void load(const char *path, Cfg &c) {
  if (FILE *f = fopen(path, "r")) {
    char ln[PKBUF + 64];
    bool cut = false;  // inside the tail of a line that did not fit the buffer
    while (fgets(ln, sizeof ln, f)) {
      size_t n = strlen(ln);
      bool full = n && ln[n - 1] == '\n';
      bool skip = cut || (!full && n == sizeof ln - 1);
      cut = !full && n == sizeof ln - 1;
      if (skip) continue;
      char *eq = strchr(ln, '=');
      if (!eq) continue;
      *eq = 0;
      const char *k = ln;
      char *v = eq + 1;
      v[strcspn(v, "\r\n")] = 0;
      if (!strcmp(k, "pkgs")) { snprintf(c.pk, sizeof c.pk, "%s", v); continue; }
      if (*v < '0' || *v > '9') continue;  // malformed value, keep the previous one
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
  set_pk(c.pk);
}

// ---------------------------------------------------------------- state file
// idle | active | error:<reason>, rewritten only when the text changes
static char g_st[64];
static bool g_on = false;  // boost on/off as last announced through notify.sh

static void set_state(const char *s) {
  if (!strcmp(s, g_st)) return;
  snprintf(g_st, sizeof g_st, "%s", s);
  char p[300];
  snprintf(p, sizeof p, "%s/sensit.state", g_dir);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

// ---------------------------------------------------------------- foreground detection
// no dumpsys. /proc is fully visible to the shell user (group readproc). an app's main process is
// named after its package and the top app has oom_score_adj <= 0.
//  - every tick : list /proc, read cmdline only for pids that are new since the last tick
//  - on a match : keep /proc/<pid>/oom_score_adj open, one pread() per tick (any match <= 0 = foreground)
static char g_pk[MAXPK][PKLEN];
static int g_pkn = 0;
static int g_adj[MAXPK];   // open oom_score_adj fds of the running listed apps
static int g_nadj = 0;
static int g_adj_err = 0;  // errno of the last failed open of one, 0 = fine
static int g_buf[3][MAXPID];
static int *g_prev = g_buf[0], *g_snap = g_buf[1], *g_cur = g_buf[2];
static int g_prev_n = 0;

static bool pk_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
         ch == '.';
}

// "a,b,c" -> names, blank or invalid tokens are skipped, returns how many were kept
static int parse_pk(const char *s, char out[][PKLEN]) {
  int n = 0;
  while (*s && n < MAXPK) {
    while (*s == ',' || *s == ' ' || *s == '\t') s++;
    const char *t = s;
    while (*s && *s != ',') s++;
    const char *e = s;
    while (e > t && (e[-1] == ' ' || e[-1] == '\t')) e--;
    int len = (int)(e - t);
    bool ok = len > 0 && len < PKLEN;
    for (int i = 0; ok && i < len; i++) ok = pk_char(t[i]);
    if (ok) { memcpy(out[n], t, (size_t)len); out[n][len] = 0; n++; }
  }
  return n;
}

// a changed list drops the tracked processes and forgets the seen pids, so /proc is rescanned
static void set_pk(const char *s) {
  char tmp[MAXPK][PKLEN] = {};
  int n = parse_pk(s, tmp);
  if (!n) n = parse_pk(DEFPK, tmp);
  bool same = n == g_pkn;
  for (int i = 0; same && i < n; i++) same = !strcmp(tmp[i], g_pk[i]);
  if (same) return;
  memcpy(g_pk, tmp, sizeof g_pk);
  g_pkn = n;
  for (int i = 0; i < g_nadj; i++) close(g_adj[i]);
  g_nadj = 0;
  g_adj_err = 0;
  g_prev_n = 0;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int read_pids(int *out, int max) {
  DIR *d = opendir("/proc");
  if (!d) return -1;
  int n = 0;
  while (dirent *e = readdir(d)) {
    char ch = e->d_name[0];
    if (ch < '1' || ch > '9') continue;
    if (n < max) out[n++] = atoi(e->d_name);
  }
  closedir(d);
  for (int i = 1; i < n; i++)  // procfs lists pids ascending, sort only if that ever changes
    if (out[i - 1] > out[i]) { qsort(out, (size_t)n, sizeof(int), cmp_int); break; }
  return n;
}

// 1 = listed app, 0 = not it (final), -1 = not renamed yet, look again next tick
static int check_pid(int pid) {
  char p[32], b[PKLEN + 32];
  snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0) return 0;  // kernel thread or zombie
  b[n] = 0;
  for (int i = 0; i < g_pkn; i++)
    if (!strcmp(b, g_pk[i])) return 1;
  // fresh from zygote or the usap pool: name not set yet
  return (!strncmp(b, "zygote", 6) || b[0] == '<') ? -1 : 0;
}

// looks at pids that appeared since the last tick and keeps oom_score_adj open for listed apps
static void scan() {
  int n = read_pids(g_cur, MAXPID);
  if (n < 0) return;
  g_adj_err = 0;
  int sn = 0, j = 0;
  for (int i = 0; i < n; i++) {
    int pid = g_cur[i];
    while (j < g_prev_n && g_prev[j] < pid) j++;
    if (j < g_prev_n && g_prev[j] == pid) { g_snap[sn++] = pid; continue; }  // seen before
    int r = check_pid(pid);
    if (r == 1 && g_nadj < MAXPK) {
      char p[40];
      snprintf(p, sizeof p, "/proc/%d/oom_score_adj", pid);
      int fd = open(p, O_RDONLY | O_CLOEXEC);
      if (fd < 0) { g_adj_err = errno; continue; }  // leave it out so it is retried
      g_adj[g_nadj++] = fd;
    }
    if (r >= 0) g_snap[sn++] = pid;
  }
  int *t = g_prev;  // swap instead of copy
  g_prev = g_snap;
  g_snap = t;
  g_prev_n = sn;
}

static bool in_game() {
  scan();
  bool fg = false;
  for (int i = 0; i < g_nadj;) {
    char b[16];
    ssize_t n = pread(g_adj[i], b, sizeof b - 1, 0);
    if (n <= 0) {  // process died
      close(g_adj[i]);
      g_adj[i] = g_adj[--g_nadj];
      continue;
    }
    b[n] = 0;
    if (atoi(b) <= 0) fg = true;
    i++;
  }
  return fg;
}

// ---------------------------------------------------------------- device setup
static bool has_bit(const unsigned char *b, int n) { return b[n / 8] & (1 << (n % 8)); }

static input_absinfo g_ai[ABS_CNT];
static bool g_vx = true;  // landscape up/down is raw x on portrait-native panels
static char g_path[64];
static int g_uin = -1;  // virtual touchscreen, created once, lives as long as the daemon
static int g_src = -1;  // real touchscreen, only open while active

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

static int fail(int u, const char *what) {
  fprintf(stderr, "ERR %s errno=%d\n", what, errno);
  if (u >= 0) close(u);
  return -1;
}

static int clone_dev(int src) {
  int u = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  if (u < 0) return fail(-1, "open /dev/uinput");
  unsigned char bits[BITS(KEY_MAX)];
  bool have[ABS_CNT] = {false};

  // the clone must be a complete copy: a half-configured panel would break all touch
  if (ioctl(u, UI_SET_EVBIT, EV_SYN) < 0 || ioctl(u, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(u, UI_SET_EVBIT, EV_ABS) < 0)
    return fail(u, "uinput ioctl blocked");

  memset(bits, 0, sizeof bits);
  ioctl(src, EVIOCGBIT(EV_KEY, sizeof bits), bits);
  for (int k = 0; k <= KEY_MAX; k++) if (has_bit(bits, k)) ioctl(u, UI_SET_KEYBIT, k);

  memset(bits, 0, sizeof bits);
  ioctl(src, EVIOCGBIT(EV_ABS, sizeof bits), bits);
  for (int a = 0; a <= ABS_MAX; a++)
    if (has_bit(bits, a)) {
      if (ioctl(u, UI_SET_ABSBIT, a) < 0 || ioctl(src, EVIOCGABS(a), &g_ai[a]) < 0)
        return fail(u, "abs axis copy");
      have[a] = true;
    }
  if (!have[ABS_MT_SLOT] || !have[ABS_MT_POSITION_X] || !have[ABS_MT_POSITION_Y])
    return fail(u, "not a multitouch panel");

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
        if (ioctl(u, UI_ABS_SETUP, &as) < 0) return fail(u, "UI_ABS_SETUP");
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
    if (write(u, &ud, sizeof ud) < 0) return fail(u, "uinput setup");
  }
  if (ioctl(u, UI_DEV_CREATE) < 0) return fail(u, "UI_DEV_CREATE");
  return u;
}

// ---------------------------------------------------------------- scaling + stabilizing
// per finger and axis: optional one euro filter on the raw position (casiez et al., chi 2012),
// then scaling of the filtered movement around the touch-down point. filtering comes first so
// jitter is not amplified. the scaled position is never clamped to the panel, so a swipe never
// stalls before the finger reaches the screen edge: games only look at the movement between events.
struct Ax { float k; bool filt, have; float raw, xh, dxh, fp, out; double t; };
struct Slot { Ax x, y; bool zk; };
static Slot g_sl[MAXSLOT + 1];  // last entry absorbs out-of-range slot numbers
static int g_cur_slot = 0;
static uint64_t g_down = 0;           // bitmask of fingers currently down
static float g_inv = 1.0f / 720;      // 1 / short panel side, makes filter speed resolution independent

static float alpha(float cutoff, float dt) { return 1.0f / (1.0f + 1.0f / (6.2831853f * cutoff) / dt); }

static int track(int v, double t, Ax &a, const Cfg &c) {
  if (!a.filt && a.k <= 1.0f) return v;  // nothing to do on this axis
  float x = (float)v;
  if (!a.have) {
    a.have = true;
    a.raw = a.xh = a.fp = a.out = x;
    a.dxh = 0.0f;
    a.t = t;
    return v;
  }
  float dt = (float)(t - a.t);
  dt = dt < 0.001f ? 0.001f : (dt > 0.1f ? 0.1f : dt);
  float fx = x;
  if (a.filt) {
    a.dxh += alpha(1.0f, dt) * ((x - a.raw) / dt - a.dxh);                 // derivative cutoff 1 hz
    float cut = c.fcmin + c.beta * fabsf(a.dxh) * g_inv;                   // adaptive cutoff
    a.xh += alpha(cut, dt) * (x - a.xh);
    fx = a.xh;
  }
  a.raw = x;
  a.t = t;
  a.out += (fx - a.fp) * a.k;
  a.fp = fx;
  return (int)floorf(a.out + 0.5f);
}

static void start_touch(Slot &s, const Cfg *c) {
  s = Slot();
  if (!c) return;
  s.x.k = g_vx ? c->kv : c->kh;  // portrait-native panel: raw x is screen up-down in landscape
  s.y.k = g_vx ? c->kh : c->kv;
  s.x.filt = s.y.filt = c->smooth != 0;
  s.zk = c->zone == 0;  // left/right is decided on the first horizontal sample of the touch
}

static void pick_zone(Slot &s, int v, const input_absinfo &r, const Cfg &c) {
  s.zk = true;
  int span = r.maximum - r.minimum;
  if (c.zone == 0 || span <= 0) return;
  bool low = (v - r.minimum) * 2 < span;
  bool in = (c.zone == 1) == (low == (c.orient == 1));  // orient 1: screen-left = low raw value
  if (!in) { s.x.k = s.y.k = 1.0f; s.x.filt = s.y.filt = false; }  // outside the zone: untouched
}

static double ts(const input_event &e) { return (double)e.time.tv_sec + (double)e.time.tv_usec * 1e-6; }

static void handle(input_event &e, const Cfg &c) {
  if (e.type != EV_ABS) return;
  Slot &s = g_sl[g_cur_slot];
  const input_absinfo &ax = g_ai[ABS_MT_POSITION_X], &ay = g_ai[ABS_MT_POSITION_Y];
  switch (e.code) {
    case ABS_MT_SLOT:
      g_cur_slot = (e.value >= 0 && e.value < MAXSLOT) ? e.value : MAXSLOT;
      break;
    case ABS_MT_TRACKING_ID:
      if (g_cur_slot < MAXSLOT) {
        if (e.value >= 0) g_down |= 1ull << g_cur_slot; else g_down &= ~(1ull << g_cur_slot);
      }
      start_touch(s, (e.value >= 0 && g_cur_slot < MAXSLOT) ? &c : nullptr);
      break;
    case ABS_MT_POSITION_X:
      if (!s.zk && !g_vx) pick_zone(s, e.value, ax, c);
      e.value = track(e.value, ts(e), s.x, c);
      break;
    case ABS_MT_POSITION_Y:
      if (!s.zk && g_vx) pick_zone(s, e.value, ay, c);
      e.value = track(e.value, ts(e), s.y, c);
      break;
  }
}

// ---------------------------------------------------------------- idle <-> active
// 1 = a finger is down, 0 = none, -1 = could not tell (never treated as "none")
static int touching(int fd) {
  int ns = clampi(g_ai[ABS_MT_SLOT].maximum + 1, 1, MAXSLOT);
  struct { __u32 code; __s32 v[MAXSLOT]; } q;
  memset(&q, 0, sizeof q);
  q.code = ABS_MT_TRACKING_ID;
  if (ioctl(fd, EVIOCGMTSLOTS(sizeof(__u32) + (size_t)ns * sizeof(__s32)), &q) < 0) return -1;
  for (int i = 0; i < ns; i++) if (q.v[i] >= 0) return 1;
  return 0;
}

// 0 = grabbed, 1 = finger down (retry soon), -1 = error (reason goes to the state file)
static int activate() {
  char e[64];
  int fd = open(g_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { snprintf(e, sizeof e, "error:open errno=%d", errno); set_state(e); return -1; }
  // never grab in the middle of a touch, android would be left with a stuck finger
  int t = touching(fd);
  if (t != 0) {
    if (t < 0) { snprintf(e, sizeof e, "error:touch errno=%d", errno); set_state(e); }
    close(fd);
    return t;
  }
  if (ioctl(fd, EVIOCGRAB, 1) < 0) {
    snprintf(e, sizeof e, "error:grab errno=%d", errno);
    set_state(e);
    close(fd);
    return -1;
  }
  input_event tmp[64];
  while (read(fd, tmp, sizeof tmp) > 0) {}  // drop whatever was queued before the grab
  memset(g_sl, 0, sizeof g_sl);
  g_cur_slot = 0;
  g_down = 0;
  g_src = fd;
  return 0;
}

static void deactivate() {
  ioctl(g_src, EVIOCGRAB, 0);
  close(g_src);
  g_src = -1;
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

// lifts every finger android still thinks is down (used when our view of the panel is lost)
static void lift_all() {
  input_event e[2 * MAXSLOT + 1];
  memset(e, 0, sizeof e);
  int n = 0;
  for (int s = 0; s < MAXSLOT; s++)
    if (g_down >> s & 1) {
      e[n].type = EV_ABS; e[n].code = ABS_MT_SLOT; e[n++].value = s;
      e[n].type = EV_ABS; e[n].code = ABS_MT_TRACKING_ID; e[n++].value = -1;
    }
  if (!n) return;
  e[n].type = EV_SYN; e[n++].code = SYN_REPORT;
  (void)write_all(g_uin, e, (size_t)n * sizeof(input_event));
  g_down = 0;
}

// reads one batch from the real panel and forwards it
// 1 = ok, 0 = syn_dropped (the kernel lost events, finger state is unreliable: fingers are lifted,
// the caller releases the grab and grabs again once the panel is idle), -1 = fatal
static int pump(const Cfg &c) {
  input_event in[64];
  ssize_t n = read(g_src, in, sizeof in);
  if (n < 0) return (errno == EINTR || errno == EAGAIN) ? 1 : -1;
  if (n == 0) return -1;
  int cnt = (int)(n / (ssize_t)sizeof(input_event)), ok = cnt;
  for (int i = 0; i < cnt; i++) {
    if (in[i].type == EV_SYN && in[i].code == SYN_DROPPED) { ok = i; break; }  // rest is stale
    handle(in[i], c);
  }
  if (ok && !write_all(g_uin, in, (size_t)ok * sizeof(input_event))) return -1;
  if (ok == cnt) return 1;
  lift_all();
  return 0;
}

// runs <plugin dir>/notify.sh on|off <sens_h> <sens_v>, detached, only on state changes
static void notify(bool on, const Cfg &c) {
  char sc[300];
  snprintf(sc, sizeof sc, "%s/notify.sh", g_dir);
  if (access(sc, R_OK) != 0) return;
  char h[16], v[16];
  snprintf(h, sizeof h, "%.1f", (double)c.kh);
  snprintf(v, sizeof v, "%.1f", (double)c.kv);
  if (fork() == 0) {
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGHUP, SIG_DFL);
    execl("/system/bin/sh", "sh", sc, on ? "on" : "off", h, v, (char *)nullptr);
    _exit(127);
  }
}

static void on_change(bool on, const Cfg &c) {
  set_state(on ? "active" : "idle");
  if (on == g_on) return;  // e.g. regrab after a syn_dropped: no second notification
  g_on = on;
  notify(on, c);
}

// ---------------------------------------------------------------- helpers
static int64_t now_ms() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool running(const char *pidpath) {
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
  return !strncmp(comm, "sensit", 6);
}

static void write_pid(const char *path) {
  if (FILE *f = fopen(path, "w")) { fprintf(f, "%d\n", getpid()); fclose(f); }
}

static void ok_line(int procs, const Cfg &c) {
  printf("OK dev=%s vert_is_x=%d filter=%d pkgs=%d procs=%d\n", g_path, (int)g_vx, c.filter, g_pkn, procs);
  fflush(stdout);
}

// config path -> folder (g_dir) and file name (base)
static void split(const char *cfg, char *base, size_t bn) {
  snprintf(g_dir, sizeof g_dir, "%s", cfg);
  snprintf(base, bn, "%s", cfg);
  if (char *sl = strrchr(g_dir, '/')) {
    snprintf(base, bn, "%s", sl + 1);
    *sl = 0;
    if (!g_dir[0]) snprintf(g_dir, sizeof g_dir, "/");
  } else {
    snprintf(g_dir, sizeof g_dir, ".");
  }
}

// drains inotify, true if the config file itself changed
static bool cfg_changed(int ifd, const char *base) {
  char ib[512] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t n = read(ifd, ib, sizeof ib);
  bool hit = false;
  for (char *q = ib; n > 0 && q < ib + n;) {
    inotify_event *ev = (inotify_event *)q;
    if (ev->len && !strcmp(ev->name, base)) hit = true;
    q += sizeof(inotify_event) + ev->len;
  }
  return hit;
}

int main(int argc, char **argv) {
  const char *cfgp = "config", *pidp = "sensit.pid", *dev = nullptr;
  bool dmn = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--daemon")) dmn = true;
    else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgp = argv[++i];
    else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) pidp = argv[++i];
    else if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
  }
  if (running(pidp)) { fprintf(stderr, "ERR already running\n"); return 1; }

  char base[128];
  split(cfgp, base, sizeof base);
  Cfg cfg;
  load(cfgp, cfg);

  int src;
  if (dev) { snprintf(g_path, sizeof g_path, "%s", dev); src = open(g_path, O_RDONLY | O_CLOEXEC); }
  else src = find_touch(g_path, sizeof g_path);
  if (src < 0) { fprintf(stderr, "ERR touchscreen not found errno=%d\n", errno); return 2; }
  g_uin = clone_dev(src);
  close(src);
  if (g_uin < 0) return 3;
  const input_absinfo &ax = g_ai[ABS_MT_POSITION_X], &ay = g_ai[ABS_MT_POSITION_Y];
  int rx = ax.maximum - ax.minimum, ry = ay.maximum - ay.minimum, ref = rx < ry ? rx : ry;
  g_vx = rx < ry;
  if (ref > 0) g_inv = 1.0f / (float)ref;

  int procs = read_pids(g_cur, MAXPID);
  if (cfg.filter && procs < 20) {  // /proc hidden (hidepid): game detection could never work
    fprintf(stderr, "ERR /proc not visible (pids=%d), set filter=0 to bypass\n", procs);
    ioctl(g_uin, UI_DEV_DESTROY);
    return 6;
  }
  usleep(300000);  // let android pick up the virtual device before the first grab

  if (dmn) {
    int pp[2];
    if (pipe2(pp, O_CLOEXEC) < 0) { fprintf(stderr, "ERR pipe errno=%d\n", errno); return 5; }
    if (fork() > 0) {  // launcher: report only after the daemon is really up
      close(pp[1]);
      char b = 0;
      ssize_t n;
      do { n = read(pp[0], &b, 1); } while (n < 0 && errno == EINTR);
      if (n == 1) { ok_line(procs, cfg); _exit(0); }
      fprintf(stderr, "ERR daemon failed to start\n");
      _exit(5);
    }
    setsid();
    if (fork() > 0) _exit(0);
    int z = open("/dev/null", O_RDWR);
    if (z >= 0) { dup2(z, 0); dup2(z, 1); dup2(z, 2); }
    write_pid(pidp);
    set_state("idle");
    (void)!write(pp[1], "1", 1);
    close(pp[1]);
  } else {
    write_pid(pidp);
    set_state("idle");
    ok_line(procs, cfg);
  }

  setpriority(PRIO_PROCESS, 0, -8);  // best effort, ignored if not permitted
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sig;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  signal(SIGHUP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);  // detached notify.sh children are reaped automatically

  // config is replaced by rename and inotify tells us, fallback: mtime check on each tick
  int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (ifd >= 0 && inotify_add_watch(ifd, g_dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) { close(ifd); ifd = -1; }
  struct stat st;
  long ms = 0, mn = 0;
  if (!stat(cfgp, &st)) { ms = st.st_mtim.tv_sec; mn = st.st_mtim.tv_nsec; }

  bool want = false;
  int64_t next = 0;
  pollfd pf[2];
  while (!g_stop) {
    int64_t now = now_ms();
    if (now >= next) {
      next = now + TICK_MS;
      if (ifd < 0 && !stat(cfgp, &st) && (st.st_mtim.tv_sec != ms || st.st_mtim.tv_nsec != mn)) {
        ms = st.st_mtim.tv_sec; mn = st.st_mtim.tv_nsec;
        load(cfgp, cfg);
      }
      want = !cfg.filter || in_game();
      if (g_src < 0) {
        if (!want) {
          if (g_on) on_change(false, cfg);  // left off by a regrab that never came
          char e[64];
          snprintf(e, sizeof e, "error:adj errno=%d", g_adj_err);
          set_state(g_adj_err ? e : "idle");  // surface an unreadable oom_score_adj
        } else {
          int r = activate();
          if (r == 0) on_change(true, cfg);
          else if (r > 0) next = now + RETRY_MS;  // a finger is down right now, try again shortly
        }
      }
    }
    if (g_src >= 0 && !want && g_down == 0) {  // leave only when no finger is down
      deactivate();
      on_change(false, cfg);
    }

    int64_t w = next - now;
    pf[0].fd = g_src; pf[0].events = POLLIN; pf[0].revents = 0;  // fd -1 is ignored by poll
    pf[1].fd = ifd;   pf[1].events = POLLIN; pf[1].revents = 0;
    int r = poll(pf, 2, (int)(w < 0 ? 0 : (w > TICK_MS ? TICK_MS : w)));
    if (r < 0) { if (errno == EINTR) continue; break; }
    if (r == 0) continue;

    if ((pf[1].revents & POLLIN) && cfg_changed(ifd, base)) { load(cfgp, cfg); next = 0; }
    if (g_src >= 0) {
      if (pf[0].revents & POLLIN) {
        int pr = pump(cfg);
        if (pr < 0) break;
        if (pr == 0) { deactivate(); next = now_ms() + RETRY_MS; continue; }  // regrab shortly
      }
      if (pf[0].revents & ~POLLIN) break;  // device gone: exit, grab is released
    }
  }
  if (g_src >= 0) deactivate();
  if (g_on) notify(false, cfg);
  ioctl(g_uin, UI_DEV_DESTROY);
  unlink(pidp);
  char sp[300];
  snprintf(sp, sizeof sp, "%s/sensit.state", g_dir);
  unlink(sp);
  return 0;
}
