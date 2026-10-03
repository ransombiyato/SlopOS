/*
 * slop-xsession -- run a real X11 application inside a SlopOS window.
 *
 * SlopOS native apps draw straight to the framebuffer. Third-party desktop
 * apps (Zen Browser, OBS Studio, DaVinci Resolve) speak X11, so SlopOS runs a
 * real X server (Xvfb, --fbdir) behind the scenes and this process bridges it
 * to the shell:
 *
 *   Xvfb's in-memory framebuffer   ->  the window surface   (blit)
 *   shell pointer + keys           ->  Xvfb                 (XTEST)
 *
 * The X server is launched by slop-launch before this client starts; here we
 * only connect, map a window, and pump in both directions. Everything the
 * third-party app needs then comes from a genuine X11 + glibc stack, not a
 * reimplementation.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdarg.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

static Display *dpy;
static Window   win;
static int      screen;

static const uint8_t *xfb;      /* Xvfb's screen, read-only */
static size_t          xfb_size;
static int             xfb_w, xfb_h, xfb_bpl;
static const char     *xfb_path;

static int keysym_lower[256];   /* keycode -> keysym level 0 */
static int keysym_upper[256];   /* keycode -> keysym level 1 */

/* ------------------------------------------------------------ input maps */
static void build_keymap(void) {
    int min_kc, max_kc, per;
    XDisplayKeycodes(dpy, &min_kc, &max_kc);
    KeySym *ks = XGetKeyboardMapping(dpy, min_kc, max_kc - min_kc + 1, &per);
    if (!ks) return;
    for (int kc = min_kc; kc <= max_kc; kc++) {
        int base = (kc - min_kc) * per;
        keysym_lower[kc] = (per > 0) ? (int)ks[base] : 0;
        keysym_upper[kc] = (per > 1) ? (int)ks[base + 1] : 0;
    }
    XFree(ks);
}

static char keysym_char(int ks) {
    if (ks >= 0x20 && ks < 0x7f) return (char)ks;
    if (ks >= 0x1000000) {           /* Unicode keysym */
        int u = ks & 0xffffff;
        if (u < 0x80) return (char)u;
    }
    if (ks == XK_space) return ' ';
    return 0;
}

/* Find the keycode + shift state that produce character c. */
static int keycode_for_char(char c, int *shift) {
    *shift = 0;
    for (int kc = 8; kc < 256; kc++) {
        if (keysym_char(keysym_lower[kc]) == c) { *shift = 0; return kc; }
        if (keysym_char(keysym_upper[kc]) == c) { *shift = 1; return kc; }
    }
    return 0;
}

static void fake_key(int kc, int press) {
    if (kc <= 0) return;
    XTestFakeKeyEvent(dpy, kc, press, CurrentTime);
}

static KeySym keysym_for_logic(int key) {
    switch (key) {
    case SLOP_KEY_ENTER:    return XK_Return;
    case SLOP_KEY_ESC:      return XK_Escape;
    case SLOP_KEY_BACKSPACE:return XK_BackSpace;
    case SLOP_KEY_TAB:      return XK_Tab;
    case SLOP_KEY_UP:       return XK_Up;
    case SLOP_KEY_DOWN:     return XK_Down;
    case SLOP_KEY_LEFT:     return XK_Left;
    case SLOP_KEY_RIGHT:    return XK_Right;
    case SLOP_KEY_HOME:     return XK_Home;
    case SLOP_KEY_END:      return XK_End;
    case SLOP_KEY_PGUP:     return XK_Prior;
    case SLOP_KEY_PGDN:     return XK_Next;
    case SLOP_KEY_DELETE:   return XK_Delete;
    case SLOP_KEY_SUPER:    return XK_Super_L;
    default: break;
    }
    if (key >= SLOP_KEY_F1 && key <= SLOP_KEY_F12)
        return XK_F1 + (key - SLOP_KEY_F1);
    return 0;
}

static int keycode_for_keysym(KeySym target) {
    if (!target) return 0;
    for (int kc = 8; kc < 256; kc++)
        if (keysym_lower[kc] == (int)target || keysym_upper[kc] == (int)target) return kc;
    return 0;
}

/* Replay a shell key/char event into the X server through XTEST. */
static void x_inject_key(const slop_event *e) {
    int ctrl = (e->mods & SLOP_MOD_CTRL) != 0;
    int shift = (e->mods & SLOP_MOD_SHIFT) != 0;
    if (e->ch) {
        int sh = 0;
        int kc = keycode_for_char(e->ch, &sh);
        if (!kc) return;
        if (ctrl) fake_key(keycode_for_keysym(XK_Control_L), 1);
        if (sh || shift) fake_key(keycode_for_keysym(XK_Shift_L), 1);
        fake_key(kc, 1); fake_key(kc, 0);
        if (sh || shift) fake_key(keycode_for_keysym(XK_Shift_L), 0);
        if (ctrl) fake_key(keycode_for_keysym(XK_Control_L), 0);
    } else if (e->key) {
        int kc = keycode_for_keysym(keysym_for_logic(e->key));
        if (!kc) return;
        if (ctrl) fake_key(keycode_for_keysym(XK_Control_L), 1);
        fake_key(kc, 1); fake_key(kc, 0);
        if (ctrl) fake_key(keycode_for_keysym(XK_Control_L), 0);
    }
}

static void on_event(const slop_event *e) {
    if (e->type == SLOP_EV_KEY) x_inject_key(e);
}

/* ------------------------------------------------------------------ main */
static void die(const char *what) {
    fprintf(stderr, "slop-xsession: %s: %s\n", what, strerror(errno));
    _exit(1);
}

static void usage(void) {
    fprintf(stderr,
        "usage: slop-xsession --display :N --shm PATH --size WxH --title T\n"
        "                     [--icon N] [--name APP]\n");
    _exit(2);
}

int main(int argc, char **argv) {
    const char *title = "Application";
    const char *display = ":1";
    const char *name = "app";
    int icon = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--display") && i + 1 < argc) display = argv[++i];
        else if (!strcmp(argv[i], "--shm") && i + 1 < argc) xfb_path = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--icon") && i + 1 < argc) icon = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) { (void)argv[++i]; }
        else usage();
    }
    if (!xfb_path) usage();

    /* Map Xvfb's screen before anything else so the first frame is real. */
    int fd = open(xfb_path, O_RDONLY);
    if (fd < 0) die("open X framebuffer");
    struct stat st;
    if (fstat(fd, &st) < 0) die("stat X framebuffer");
    xfb_size = st.st_size;
    xfb = mmap(NULL, xfb_size, PROT_READ, MAP_SHARED, fd, 0);
    if (xfb == MAP_FAILED) die("mmap X framebuffer");
    close(fd);

    /* XWD file header (big-endian, as Xvfb writes it):
       [0] header_size [1] version [2] format [3] depth [4] width [5] height
       ... [11] bits_per_pixel [12] bytes_per_line ... The pixel data starts
       at header_size. */
    uint32_t hdr[25];
    uint32_t hdr_size = 100;
    if (xfb_size >= sizeof(hdr)) {
        memcpy(hdr, xfb, sizeof(hdr));
        #define BE(i) __builtin_bswap32(hdr[i])
        hdr_size = BE(0);
        xfb_w   = (int)BE(4);
        xfb_h   = (int)BE(5);
        xfb_bpl = (int)BE(12);
        #undef BE
    }
    if (xfb_w <= 0 || xfb_h <= 0 || xfb_bpl <= 0 ||
        (size_t)hdr_size >= xfb_size) die("bad X framebuffer header");
    const uint8_t *xfb_pix = xfb + hdr_size;

    dpy = XOpenDisplay(display);
    if (!dpy) { fprintf(stderr, "slop-xsession: cannot open display %s\n", display); return 1; }
    screen = DefaultScreen(dpy);
    build_keymap();

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              xfb_w, xfb_h, 0, BlackPixel(dpy, screen),
                              BlackPixel(dpy, screen));
    XStoreName(dpy, win, title);
    XSelectInput(dpy, win, StructureNotifyMask | ExposureMask);
    XMapWindow(dpy, win);
    XFlush(dpy);

    if (slop_app_start(title, icon) != 0) {
        fprintf(stderr, "slop-xsession: no display for %s\n", name);
        return 1;
    }
    slop_set_title(title);
    slop_event_hook = on_event;
    fprintf(stderr, "slop-xsession: X %dx%d (bpl %d) -> surface %dx%d\n",
            xfb_w, xfb_h, xfb_bpl, slop.w, slop.h);

    /* The window surface may not be the X screen size; letterbox if needed. */
    int vw = slop.w, vh = slop.h;
    (void)vw; (void)vh;

    unsigned long last = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    last = (unsigned long)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    for (;;) {
        slop_begin_frame();

        /* ---- X events (configure/expose) ---- */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }

        /* ---- pointer: shell -> X ---- */
        int mx = ui.mx, my = ui.my;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (ui.pressed || ui.released || ui.rpressed || ui.wheel)
            XTestFakeMotionEvent(dpy, screen, mx, my, CurrentTime);
        if (ui.pressed) XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        if (ui.released) {
            XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
            XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
        }
        if (ui.rpressed) XTestFakeButtonEvent(dpy, 3, True, CurrentTime);
        if (ui.wheel) {
            int b = ui.wheel > 0 ? 5 : 4;
            int n = ui.wheel > 0 ? ui.wheel : -ui.wheel;
            for (int w = 0; w < n; w++) {
                XTestFakeButtonEvent(dpy, b, True, CurrentTime);
                XTestFakeButtonEvent(dpy, b, False, CurrentTime);
            }
        }

        /* ---- X framebuffer -> our surface ---- */
        if (slop.back) {
            uint32_t *dst = slop.back;
            int dw = slop.w, dh = slop.h;
            int src_off_y = xfb_h > dh ? (xfb_h - dh) / 2 : 0;
            int src_off_x = xfb_w > dw ? (xfb_w - dw) / 2 : 0;
            int cw = dw < xfb_w ? dw : xfb_w;
            int ch = dh < xfb_h ? dh : xfb_h;
            uint32_t letterbox = slop_theme_dark.bg & 0xffffffu;
            for (int y = 0; y < dh; y++) {
                uint32_t *row = dst + (size_t)y * dw;
                if (y >= ch) {
                    for (int x = 0; x < dw; x++) row[x] = letterbox;
                    continue;
                }
                const uint32_t *src = (const uint32_t *)(xfb_pix + (size_t)(y + src_off_y) * xfb_bpl);
                for (int x = 0; x < cw; x++)
                    row[x] = src[x + src_off_x] & 0x00ffffffu;
                for (int x = cw; x < dw; x++) row[x] = letterbox;
            }
        }
        XFlush(dpy);

        slop_end_frame();

        struct timeval t2;
        gettimeofday(&t2, NULL);
        unsigned long now = (unsigned long)t2.tv_sec * 1000 + t2.tv_usec / 1000;
        if (now - last < 16) usleep(16000 - (now - last));
        last = now;
    }
    return 0;
}
