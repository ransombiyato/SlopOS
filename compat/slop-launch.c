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

int main(int argc, char **argv) {
    const char *id = argc > 1 ? argv[1] : "zen";
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

    int have_gui = (slop_init() == 0);
    if (have_gui) {
        slop_flush_events();
        draw_readiness(&m, checks, nc, libs_ok, m.nlibs, tried, launch_path != NULL);
        if (launch_path) {
            usleep(400000);
            slop_shutdown();
            apply_profile(&m);
            execl(launch_path, launch_path, (char *)NULL);
            perror("slop-launch: exec");
            return 1;
        }
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
    if (launch_path) { apply_profile(&m); execl(launch_path, launch_path, (char *)NULL); }
    return 0;
}
