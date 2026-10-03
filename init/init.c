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
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <linux/vt.h>

static void ensure_dir(const char *p) { mkdir(p, 0755); }

/* Poll for a device node to appear. devtmpfs is populated asynchronously as
   drivers probe, so PID 1 can start before the node we need exists. */
static int wait_for_node(const char *path, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        if (access(path, F_OK) == 0) return 0;
        usleep(20000);
    }
    return -1;
}

static void try_mount(const char *src, const char *tgt, const char *fs, unsigned long flags) {
    ensure_dir(tgt);
    if (mount(src, tgt, fs, flags, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "init: mount %s -> %s: %s\n", src, tgt, strerror(errno));
}

/* Mount the optical drive, waiting for the device node the kernel creates
   once it has probed the ATAPI/CD-ROM device. */
static void mount_cdrom(void) {
    ensure_dir("/media/cdrom");
    if (wait_for_node("/dev/sr0", 5000) != 0) return;
    try_mount("/dev/sr0", "/media/cdrom", "iso9660", MS_RDONLY);
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
    mount_cdrom();
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

/* devtmpfs normally provides the framebuffer node, but if the kernel did not
   populate it (or CONFIG_DEVTMPFS_MOUNT raced us) create it by hand so the
   GUI still comes up. fb0 is char major 29. */
static void ensure_fb_node(void) {
    if (wait_for_node("/dev/fb0", 2000) == 0) return;
    if (mknod("/dev/fb0", S_IFCHR | 0600, makedev(29, 0)) < 0)
        fprintf(stderr, "init: mknod /dev/fb0: %s\n", strerror(errno));
}

/* The kernel's virtual terminal keeps an active text console on the very same
   framebuffer the GUI draws to. If it stays in text mode its cursor and any
   key echo show through (and above) the desktop -- the blinking underscore in
   the top-left corner. Putting the console into KD_GRAPHICS mode and blanking
   it stops the kernel from touching the screen at all; the GUI is then the
   only writer. Keyboard input still arrives via /dev/input. */
static void silence_console(void) {
    /* tty0 is the active virtual terminal; that is the one drawing the cursor.
       Fall back to tty1 if it cannot be opened. */
    const char *paths[] = { "/dev/tty0", "/dev/tty1", "/dev/console" };
    for (int i = 0; i < 3; i++) {
        int fd = open(paths[i], O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, KDSETMODE, KD_GRAPHICS);
        close(fd);
    }
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

/* Run the first-run wizard in its own process. It talks to the compositor,
   so it must come up after slop-shell has created its socket; run_shell()
   waits for that below. */
static void start_setup_wizard(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        setenv("HOME", "/home/user", 1);
        setenv("PATH", "/usr/bin:/bin", 1);
        execl("/usr/bin/slop-setup", "slop-setup", (char *)NULL);
        execl("/bin/slop-setup", "slop-setup", (char *)NULL);
        _exit(127);
    }
}

/* Wait for the compositor socket so the wizard can connect immediately
   instead of falling back to owning the framebuffer. */
static void wait_for_compositor(void) {
    for (int i = 0; i < 200; i++) {
        if (access("/run/slop/compositor.sock", F_OK) == 0) return;
        usleep(25000);
    }
}

static int needs_setup(void) {
    return access("/home/user/.config/slop/.configured", F_OK) != 0;
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

    int have_fb;
    mount_all();
    ensure_fb_node();
    set_hostname();
    tune_sysctl();
    write_os_release();

    /* Open the framebuffer only after /dev is mounted: on some boots the
       kernel's devtmpfs is not populated yet when init starts, and the fb
       node would not be there to open. */
    have_fb = (slop_init() == 0);
    if (have_fb) { silence_console(); splash(0, total, steps[0].msg); }

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

    /* First boot: start the shell, then hand off to the setup wizard so it can
       own the first window the user sees. Later boots go straight to the
       desktop. */
    pid_t sh = run_shell();
    wait_for_compositor();
    if (needs_setup()) start_setup_wizard();

    for (;;) {
        int st = 0;
        waitpid(-1, &st, 0);
        if (sh > 0 && waitpid(sh, &st, WNOHANG) == sh) {
            fprintf(stderr, "init: shell exited (status %d), restarting\n", st);
            sleep(1);
            sh = run_shell();
            wait_for_compositor();
        }
    }
    return 0;
}
