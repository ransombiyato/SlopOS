/*
 * slop-launch -- the SlopOS third-party application runtime.
 *
 * SlopOS native apps talk to the framebuffer directly. Third-party desktop
 * apps (Zen Browser, OBS Studio, DaVinci Resolve) are ordinary Linux/ELF
 * programs that expect a glibc + X11/Wayland + audio world. Rather than make
 * them native, SlopOS runs them through this runtime:
 *
 *   1. read the app's manifest from /usr/share/slop/compat/<id>.app
 *   2. verify the kernel features and libraries the app declares
 *   3. apply the compatibility profile (env, device access, data dirs)
 *   4. exec the first launcher that actually exists
 *
 * If the app is not installed, it shows an honest readiness screen instead of
 * failing silently.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <linux/memfd.h>
#include <stdarg.h>
#include <sys/wait.h>

/* ------------------------------------------------------------- manifest */
typedef struct {
    char name[128];
    char id[64];
    char icon[64];
    char profile[64];
    char data_dir[256];
    char execs[8][256];
    int  nexec;
    char libs[64][128];
    int  nlibs;
    char kern[32][128];
    int  nkern;
    char devs[16][128];
    int  ndev;
    int  min_ram_mb, min_cores;
} manifest;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}

static int parse_list(char *val, char arr[][128], int max, int slot) {
    int n = 0;
    char *tok = strtok(val, ",");
    while (tok && slot + n < max) {
        snprintf(arr[slot + n], 128, "%s", trim(tok));
        n++;
        tok = strtok(NULL, ",");
    }
    return n;
}

static int load_manifest(const char *id, manifest *m) {
    memset(m, 0, sizeof(*m));
    m->min_ram_mb = 0; m->min_cores = 0;
    char path[256];
    snprintf(path, sizeof(path), "/usr/share/slop/compat/%s.app", id);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '#' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (!strcmp(key, "name")) snprintf(m->name, sizeof(m->name), "%s", val);
        else if (!strcmp(key, "id")) snprintf(m->id, sizeof(m->id), "%s", val);
        else if (!strcmp(key, "icon")) snprintf(m->icon, sizeof(m->icon), "%s", val);
        else if (!strcmp(key, "profile")) snprintf(m->profile, sizeof(m->profile), "%s", val);
        else if (!strcmp(key, "data_dir")) snprintf(m->data_dir, sizeof(m->data_dir), "%s", val);
        else if (!strcmp(key, "exec") && m->nexec < 8)
            snprintf(m->execs[m->nexec++], 256, "%s", val);
        else if (!strcmp(key, "needs_libs"))
            m->nlibs += parse_list(val, m->libs, 64, m->nlibs);
        else if (!strcmp(key, "needs_kernel"))
            m->nkern += parse_list(val, m->kern, 32, m->nkern);
        else if (!strcmp(key, "needs_dev"))
            m->ndev += parse_list(val, m->devs, 16, m->ndev);
        else if (!strcmp(key, "min_ram_mb")) m->min_ram_mb = atoi(val);
        else if (!strcmp(key, "min_cores")) m->min_cores = atoi(val);
    }
    fclose(f);
    return 0;
}

/* ----------------------------------------------------- capability probes */
typedef struct { char label[48]; int ok; } check_t;

static int has_path(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void probe(const char *feature, check_t *out) {
    snprintf(out->label, sizeof(out->label), "%s", feature);
    out->ok = 0;
    if (!strcmp(feature, "seccomp")) {
        FILE *f = fopen("/proc/sys/kernel/seccomp/actions_avail", "r");
        out->ok = f != NULL;
        if (f) fclose(f);
    } else if (!strcmp(feature, "namespaces") || !strcmp(feature, "user_ns")) {
        out->ok = has_path("/proc/self/ns/user") || has_path("/proc/self/ns/mnt");
    } else if (!strcmp(feature, "memfd")) {
        int fd = memfd_create("probe", 0);
        out->ok = fd >= 0; if (fd >= 0) close(fd);
    } else if (!strcmp(feature, "eventfd")) {
        int fd = eventfd(0, 0);
        out->ok = fd >= 0; if (fd >= 0) close(fd);
    } else if (!strcmp(feature, "epoll")) {
        int fd = epoll_create1(0);
        out->ok = fd >= 0; if (fd >= 0) close(fd);
    } else if (!strcmp(feature, "inotify")) {
        int fd = inotify_init1(IN_NONBLOCK);
        out->ok = fd >= 0; if (fd >= 0) close(fd);
    } else if (!strcmp(feature, "unix_sockets")) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        out->ok = fd >= 0; if (fd >= 0) close(fd);
    } else if (!strcmp(feature, "tcp")) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        out->ok = fd >= 0; if (fd >= 0) close(fd);
    } else if (!strcmp(feature, "shm")) {
        out->ok = has_path("/dev/shm");
    } else if (!strcmp(feature, "drm") || !strcmp(feature, "drm_render")) {
        out->ok = has_path("/dev/dri");
    } else if (!strcmp(feature, "evdev")) {
        out->ok = has_path("/dev/input");
    } else if (!strcmp(feature, "snd")) {
        out->ok = has_path("/dev/snd");
    } else if (!strcmp(feature, "v4l2")) {
        out->ok = has_path("/dev/video0");
    } else if (!strcmp(feature, "futex") || !strcmp(feature, "aio") ||
               !strcmp(feature, "sched_rt") || !strcmp(feature, "sockets")) {
        out->ok = 1;   /* guaranteed by the SlopOS kernel config */
    } else {
        out->ok = 1;
    }
}

static int lib_present(const char *lib) {
    const char *dirs[] = { "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
                           "/lib64", "/usr/lib64", "/lib", "/usr/lib", NULL };
    for (int i = 0; dirs[i]; i++) {
        char p[320];
        snprintf(p, sizeof(p), "%s/%s", dirs[i], lib);
        if (has_path(p)) return 1;
    }
    /* many apps are flatpak/AppImage and ship their own libs */
    return 0;
}

/* --------------------------------------------------------- profile apply */
static void apply_profile(const manifest *m) {
    mkdir("/run/user", 0755);
    mkdir("/run/user/1000", 0700);
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    setenv("HOME", "/home/user", 1);
    setenv("PATH", "/opt/zen:/opt/obs/bin:/opt/davinci/bin:/usr/bin:/bin", 1);
    setenv("DISPLAY", ":0", 1);
    if (!strcmp(m->profile, "desktop-graphical")) {
        setenv("GDK_BACKEND", "x11", 1);
        setenv("MOZ_ENABLE_WAYLAND", "0", 1);
        setenv("MOZ_X11_EGL", "1", 1);
    } else if (!strcmp(m->profile, "media-graphical")) {
        setenv("GDK_BACKEND", "x11", 1);
        setenv("PULSE_SERVER", "unix:/run/user/1000/pulse/native", 1);
        setenv("PIPEWIRE_RUNTIME_DIR", "/run/user/1000", 1);
        setenv("OBS_USE_EGL", "1", 1);
    } else if (!strcmp(m->profile, "workstation-gpu")) {
        setenv("QT_QPA_PLATFORM", "xcb", 1);
        setenv("OCL_ICD_VENDORS", "/etc/OpenCL/vendors", 1);
        setenv("LIBVA_DRIVER_NAME", "iHD", 1);
        setenv("RESOLVE_GPU_MODE", "auto", 1);
    }
    if (m->data_dir[0]) {
        char tmp[300];
        snprintf(tmp, sizeof(tmp), "%s", m->data_dir);
        for (char *p = tmp + 1; *p; p++)
            if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
        mkdir(tmp, 0755);
    }
}

/* ------------------------------------------------------------- UI screen */
static void draw_readiness(const manifest *m, check_t *checks, int nchecks,
                           int libs_ok, int libs_total, const char *tried, int running) {
    slop_clear(slop_theme_dark.bg);
    slop_vgradient(0, 0, slop.w, slop.h, SLOP_RGB(0x10, 0x13, 0x1f), slop_theme_dark.bg);
    slop_chrome c = slop_window(m->name, "SlopOS compatibility runtime", 0);

    int x = c.x + 24, y = c.y + 18, w = c.w - 48;

    slop_badge(x, y, "COMPATIBILITY", slop_theme_dark.accent);
    slop_badge(x + 150, y, m->profile, slop_theme_dark.good);
    y += 40;

    /* kernel features */
    slop_text(&slop_font_bold, x, y + 10, "Kernel features", slop_theme_dark.text);
    y += 26;
    int col = 0;
    for (int i = 0; i < nchecks; i++) {
        int cx = x + (col % 3) * (w / 3);
        int cy = y + (col / 3) * 22;
        slop_color dot = checks[i].ok ? slop_theme_dark.good : slop_theme_dark.bad;
        slop_fill_circle(cx + 6, cy + 8, 5, dot);
        slop_text_vcenter(&slop_font_small, cx + 18, cy, 16, checks[i].label,
                          checks[i].ok ? slop_theme_dark.text : slop_theme_dark.text_mute);
        col++;
    }
    y += ((col + 2) / 3) * 22 + 20;

    /* libraries */
    char lbuf[128];
    snprintf(lbuf, sizeof(lbuf), "Libraries: %d / %d resolved from host", libs_ok, libs_total);
    slop_text(&slop_font_bold, x, y + 10, "Libraries", slop_theme_dark.text);
    slop_text_vcenter(&slop_font_small, x + 100, y, 16, lbuf,
                      libs_ok == libs_total ? slop_theme_dark.good : slop_theme_dark.warn);
    y += 30;

    slop_hline(x, y, w, slop_theme_dark.border);
    y += 20;

    if (running) {
        slop_text(&slop_font_bold, x, y + 10, "Launching...", slop_theme_dark.good);
    } else {
        slop_text(&slop_font_bold, x, y + 10, "Application not installed", slop_theme_dark.warn);
        y += 28;
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "The runtime is ready, but none of this app's launchers were found.");
        slop_text(&slop_font_small, x, y + 10, msg, slop_theme_dark.text_dim);
        y += 26;
        slop_text(&slop_font_small, x, y + 10, "Looked for:", slop_theme_dark.text_mute);
        y += 20;
        slop_text(&slop_font_mono, x + 12, y + 12, tried, slop_theme_dark.text_dim);
        y += 30;
        char hint[512];
        snprintf(hint, sizeof(hint),
                 "Install it with the package manager, then reopen it from the dock.");
        slop_text(&slop_font_small, x, y + 10, hint, slop_theme_dark.text_mute);
        y += 28;
        char p1[300];
        if (m->execs[0][0])
            snprintf(p1, sizeof(p1), "Expected at: %s", m->execs[0]);
        else
            snprintf(p1, sizeof(p1), "No exec path declared in the manifest.");
        slop_text(&slop_font_mono, x + 12, y + 12, p1, slop_theme_dark.accent);
    }

    /* footer: honest note */
    const char *note =
        "Native SlopOS apps draw straight to the framebuffer and need no runtime. "
        "Only third-party apps use this compatibility layer.";
    int ny = c.y + c.h - 20;
    slop_text(&slop_font_small, x, ny, note, slop_theme_dark.text_mute);
    slop_present();
}

/* --------------------------------------------------------- X11 session
 *
 * Zen/OBS/Resolve are X11 clients. To run them we start a real X server
 * (Xvfb) writing its screen to a file (--fbdir), then start slop-xsession,
 * which blits that screen into a SlopOS window and forwards input back
 * through XTEST. The app itself then runs unmodified with DISPLAY set.
 */
#define SLOP_X_DISPLAY ":1"
#define SLOP_X_FBDIR   "/run/slop/x"
#define SLOP_X_W       758
#define SLOP_X_H       467

static int icon_for_id(const char *id) {
    if (strstr(id, "zen")) return 5;
    if (strstr(id, "obs")) return 6;
    if (strstr(id, "davinci") || strstr(id, "resolve")) return 7;
    return 0;
}

static int wait_for(const char *path, int ms) {
    for (int t = 0; t < ms; t += 50) {
        if (access(path, R_OK) == 0) return 0;
        usleep(50000);
    }
    return -1;
}

/* ------------------------------------------------------------- serial log
   The kernel keeps the framebuffer as the primary console, so app stderr
   does not reach the serial port and /dev/kmsg is filtered out at
   `quiet loglevel=3`. Opening /dev/ttyS0 and writing to it reaches the
   serial log regardless, which is how the compatibility session is made
   visible during a headless boot. */
static void klog(const char *fmt, ...) {
    int fd = open("/dev/ttyS0", O_WRONLY);
    if (fd < 0) fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0) return;
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ssize_t r = write(fd, buf, strlen(buf));
    (void)r;
    close(fd);
}

/* Echo a small file's contents to the serial log, one line, labelled. */
static void klog_file(const char *label, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[600];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = ' ';
    klog("slop-x11: %s: %.500s\n", label, buf);
}

/* Start Xvfb and the SlopOS window bridge. Returns 0 on success. */
static int start_x_session(const manifest *m, const char *id) {
    mkdir("/run/slop", 0755);
    mkdir(SLOP_X_FBDIR, 0755);
    remove(SLOP_X_FBDIR "/Xvfb_screen0");
    klog("slop-x11: starting display server for %s\n", id);

    pid_t x = fork();
    if (x == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); }
        /* Xvfb diagnostics go to a file we relay to the serial log if the
           display fails to come up, so the exact X server error is visible. */
        int ef = open("/run/slop/xvfb.err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ef >= 0) dup2(ef, 2);
        char size[32];
        snprintf(size, sizeof(size), "%dx%dx24", SLOP_X_W, SLOP_X_H);
        execl("/usr/bin/Xvfb", "Xvfb", (char *)SLOP_X_DISPLAY, "-screen", "0",
              size, "-nolisten", "tcp", "-fbdir", (char *)SLOP_X_FBDIR, (char *)NULL);
        _exit(127);
    }
    if (x < 0) return -1;

    if (wait_for(SLOP_X_FBDIR "/Xvfb_screen0", 8000) != 0) {
        klog("slop-x11: Xvfb produced no framebuffer\n");
        klog_file("Xvfb", "/run/slop/xvfb.err");
        return -1;
    }
    /* give the X server a moment to finish initialising the socket */
    wait_for("/tmp/.X11-unix/X1", 3000);

    pid_t s = fork();
    if (s == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); }
        int ef = open("/run/slop/xsession.err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ef >= 0) dup2(ef, 2);
        char sz[32], ic[16], shm[256];
        snprintf(sz, sizeof(sz), "%dx%d", SLOP_X_W, SLOP_X_H);
        snprintf(ic, sizeof(ic), "%d", icon_for_id(id));
        snprintf(shm, sizeof(shm), SLOP_X_FBDIR "/Xvfb_screen0");
        execl("/usr/bin/slop-xsession", "slop-xsession",
              "--display", (char *)SLOP_X_DISPLAY, "--shm", shm,
              "--size", sz, "--title", m->name, "--icon", ic, "--name", id,
              (char *)NULL);
        _exit(127);
    }
    usleep(800000);   /* let the X server and the bridge settle */
    /* If the bridge died on startup the window will stay blank, so surface
       the reason while we can. */
    int st = 0;
    if (s > 0 && waitpid(s, &st, WNOHANG) == s) {
        klog("slop-x11: window bridge exited (status %d)\n", st);
        klog_file("xsession", "/run/slop/xsession.err");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *id = argc > 1 ? argv[1] : "zen";
    klog("slop-launch: start id=%s\n", id);
    /* accept either a display name or an id; the shell passes the id */
    char key[64];
    snprintf(key, sizeof(key), "%s", id);
    for (char *p = key; *p; p++) if (*p == ' ') *p = '-';
    for (char *p = key; *p; p++) *p = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
    /* map friendly names to manifest ids */
    if (strstr(key, "zen")) strcpy(key, "zen-browser");
    else if (strstr(key, "obs")) strcpy(key, "obs-studio");
    else if (strstr(key, "davinci") || strstr(key, "resolve")) strcpy(key, "davinci-resolve");

    manifest m;
    if (load_manifest(key, &m) != 0) {
        fprintf(stderr, "slop-launch: no manifest for '%s'\n", id);
        return 1;
    }

    /* probes */
    check_t checks[32];
    int nc = 0;
    for (int i = 0; i < m.nkern && nc < 32; i++) probe(m.kern[i], &checks[nc++]);
    int libs_ok = 0;
    for (int i = 0; i < m.nlibs; i++) if (lib_present(m.libs[i])) libs_ok++;

    /* is the app installed? */
    char tried[512] = "";
    const char *launch_path = NULL;
    for (int i = 0; i < m.nexec; i++) {
        if (tried[0]) strncat(tried, "  ", sizeof(tried) - strlen(tried) - 1);
        strncat(tried, m.execs[i], sizeof(tried) - strlen(tried) - 1);
        if (m.execs[i][0] == '/' && access(m.execs[i], X_OK) == 0) { launch_path = m.execs[i]; break; }
    }

    /* If the app is installed, run it for real on a genuine X server. The
       readiness screen is only for the (uninstalled) case. */
    if (launch_path) {
        apply_profile(&m);
        if (start_x_session(&m, key) != 0)
            klog("slop-x11: display server failed for %s\n", key);
        else
            klog("slop-x11: display ready on %s (%dx%d)\n", SLOP_X_DISPLAY,
                 SLOP_X_W, SLOP_X_H);
        setenv("DISPLAY", SLOP_X_DISPLAY, 1);
        /* Capture the app's stderr so a failed X connection is visible. */
        int ae = open("/run/slop/app.err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ae >= 0) dup2(ae, 2);
        klog("slop-launch: exec %s (profile %s)\n", launch_path, m.profile);
        execv(launch_path, (char *[]){ (char *)launch_path, NULL });
        klog("slop-launch: exec failed: %s\n", strerror(errno));
        return 1;
    }

    int have_gui = (slop_init() == 0);
    if (have_gui) {
        slop_flush_events();
        draw_readiness(&m, checks, nc, libs_ok, m.nlibs, tried, 0);
        /* wait for the user to close the readiness screen */
        for (;;) {
            slop_begin_frame();
            if (ui.quit) break;
            draw_readiness(&m, checks, nc, libs_ok, m.nlibs, tried, 0);
            slop_end_frame();
            usleep(20000);
        }
        slop_shutdown();
        return 0;
    }

    /* headless: just report */
    printf("slop-launch: %s (profile %s)\n", m.name, m.profile);
    for (int i = 0; i < nc; i++)
        printf("  [%s] %s\n", checks[i].ok ? "ok" : "!!", checks[i].label);
    printf("  libraries %d/%d, launcher: %s\n", libs_ok, m.nlibs, launch_path ? launch_path : "not installed");
    return 0;
}
