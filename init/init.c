/*
 * SlopOS init -- PID 1.
 *
 * Mounts the core filesystems, paints a boot splash, then hands control to
 * the SlopOS shell. It also reaps orphans and respawns the shell if it ever
 * exits, so the machine never lands in a dead console.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <linux/vt.h>

static void ensure_dir(const char *p) { mkdir(p, 0755); }

static void try_mount(const char *src, const char *tgt, const char *fs, unsigned long flags) {
    ensure_dir(tgt);
    if (mount(src, tgt, fs, flags, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "init: mount %s -> %s: %s\n", src, tgt, strerror(errno));
}

static void mount_all(void) {
    try_mount("proc", "/proc", "proc", 0);
    try_mount("sysfs", "/sys", "sysfs", 0);
    try_mount("devtmpfs", "/dev", "devtmpfs", 0);
    try_mount("devpts", "/dev/pts", "devpts", 0);   /* needed for the terminal pty */
    try_mount("tmpfs", "/tmp", "tmpfs", 0);
    try_mount("tmpfs", "/run", "tmpfs", 0);
    try_mount("tmpfs", "/dev/shm", "tmpfs", 0);
    try_mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0);
    ensure_dir("/run/slop");
    ensure_dir("/tmp");
    ensure_dir("/home");
    ensure_dir("/home/user");
    ensure_dir("/home/user/Desktop");
    ensure_dir("/home/user/Documents");
    ensure_dir("/home/user/Pictures");
    ensure_dir("/home/user/Downloads");
    ensure_dir("/home/user/Music");
    ensure_dir("/home/user/Videos");
    ensure_dir("/opt");
    ensure_dir("/opt/zen");
    ensure_dir("/opt/obs");
    ensure_dir("/opt/davinci");
    ensure_dir("/usr");
    ensure_dir("/usr/bin");
    ensure_dir("/usr/share");
}

static void write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { ssize_t r = write(fd, data, strlen(data)); (void)r; close(fd); }
}

static void set_hostname(void) {
    write_file("/proc/sys/kernel/hostname", "slopos");
}

/* Kernel knobs that make a small desktop feel nicer. */
static void tune_sysctl(void) {
    write_file("/proc/sys/vm/swappiness", "10");
    write_file("/proc/sys/kernel/printk", "3 3 3 3");
    write_file("/proc/sys/net/ipv4/ip_forward", "0");
}

/* Publish os-release so scripts and the shell can identify the system. */
static void write_os_release(void) {
    FILE *f = fopen("/etc/os-release", "w");
    if (!f) return;
    fputs("NAME=\"SlopOS\"\n"
          "VERSION=\"0.2 (Aurora)\"\n"
          "ID=slopos\n"
          "PRETTY_NAME=\"SlopOS 0.2 (Aurora)\"\n"
          "HOME_URL=\"https://slopos.local\"\n", f);
    fclose(f);
}

/* ------------------------------------------------------------- boot splash */
static void splash(int step, int total, const char *msg) {
    slop_clear(slop_theme_dark.bg);
    slop_vgradient(0, 0, slop.w, slop.h, SLOP_RGB(0x10, 0x13, 0x1f), SLOP_RGB(0x08, 0x09, 0x0f));

    int cx = slop.w / 2;
    int cy = slop.h / 2 - 40;

    /* logo: a rounded gradient tile with an "S" */
    int s = 96;
    slop_fill_round(cx - s / 2, cy - s / 2, s, s, 24, slop_theme_dark.accent_lo);
    slop_fill_round(cx - s / 2 + 4, cy - s / 2 + 4, s - 8, s - 8, 20, slop_theme_dark.accent);
    slop_vgradient(cx - s / 2 + 4, cy - s / 2 + 4, s - 8, (s - 8) / 2,
                   slop_theme_dark.accent_hi, slop_theme_dark.accent);
    int tw = slop_text_w(&slop_font_title, "S");
    slop_text(&slop_font_title, cx - tw / 2, cy + 9, "S", SLOP_RGB(0xff, 0xff, 0xff));

    const char *name = "SlopOS";
    int nw = slop_text_w(&slop_font_title, name);
    slop_text(&slop_font_title, cx - nw / 2, cy + s / 2 + 44, name, slop_theme_dark.text);
    const char *tag = "a smooth little x86_64 desktop";
    int gw = slop_text_w(&slop_font_small, tag);
    slop_text(&slop_font_small, cx - gw / 2, cy + s / 2 + 68, tag, slop_theme_dark.text_mute);

    int bw = 360, bh = 6;
    int bx = cx - bw / 2, by = cy + s / 2 + 100;
    slop_progress(bx, by, bw, bh, step * 100 / total, slop_theme_dark.accent);
    int mw = slop_text_w(&slop_font_small, msg);
    slop_text(&slop_font_small, cx - mw / 2, by + 30, msg, slop_theme_dark.text_dim);
    slop_present();
}

/* ------------------------------------------------------------------ shell */
static void reap(int sig) { (void)sig; while (waitpid(-1, NULL, WNOHANG) > 0) {} }

static pid_t run_shell(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int tty = open("/dev/tty1", O_RDWR);
        if (tty >= 0) { dup2(tty, 0); dup2(tty, 1); dup2(tty, 2); if (tty > 2) close(tty); }
        setenv("HOME", "/home/user", 1);
        setenv("PATH", "/usr/bin:/bin:/sbin", 1);
        setenv("TERM", "slop", 1);
        setenv("USER", "user", 1);
        execl("/usr/bin/slop-shell", "slop-shell", (char *)NULL);
        execl("/bin/slop-shell", "slop-shell", (char *)NULL);
        perror("init: exec slop-shell");
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Detach from the inherited console; we own the machine now. */
    setsid();

    static const struct { const char *msg; } steps[] = {
        { "Starting SlopOS kernel services" },
        { "Mounting /proc, /sys and /dev" },
        { "Preparing the desktop session" },
        { "Starting the Aurora window shell" },
    };
    const int total = (int)(sizeof(steps) / sizeof(steps[0]));

    int have_fb = (slop_init() == 0);
    if (have_fb) splash(0, total, steps[0].msg);

    mount_all();
    set_hostname();
    tune_sysctl();
    write_os_release();

    struct sigaction sa = {0};
    sa.sa_handler = reap;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < total; i++) {
        if (have_fb) splash(i, total, steps[i].msg);
        usleep(180000);
    }

    if (have_fb) {
        splash(total, total, "Ready");
        usleep(250000);
        slop_shutdown();
    }

    printf("\nSlopOS %s -- init ready (PID 1)\n\n", "0.2");
    fflush(stdout);

    for (;;) {
        pid_t sh = run_shell();
        int st = 0;
        waitpid(sh, &st, 0);
        fprintf(stderr, "init: shell exited (status %d), restarting\n", st);
        sleep(1);
    }
    return 0;
}
