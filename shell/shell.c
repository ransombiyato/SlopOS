/*
 * slop-shell -- the SlopOS desktop session and compositor.
 *
 * The shell is the only process that owns the screen. Native apps connect to
 * it as compositor clients and hand over shared-memory surfaces, which the
 * shell draws into decorated, draggable, resizable windows. Third-party apps
 * (Zen, OBS, Resolve) still run fullscreen through the compatibility runtime,
 * because they draw their own X11/Wayland output rather than using libslop.
 *
 * The result is a real little desktop: overlapping windows with focus, a
 * dock, a launcher, quick settings, notifications and a calendar.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <stdarg.h>

/* ================================================================== apps */
typedef struct {
    const char *name;
    const char *desc;
    const char *exec;
    int         icon;
    int         third_party;
} app_entry;

static const app_entry APPS[] = {
    { "Files",          "Browse and manage your files",  "slop-files",    SLOP_ICON_FILES,    0 },
    { "Terminal",       "Run commands and programs",     "slop-term",     SLOP_ICON_TERM,     0 },
    { "Image Viewer",   "Look at photos and graphics",   "slop-image",    SLOP_ICON_IMAGE,    0 },
    { "File Viewer",    "Read text, code and documents", "slop-view",     SLOP_ICON_VIEW,     0 },
    { "Zen Browser",    "Fast, private web browsing",    "slop-launch",   SLOP_ICON_ZEN,      1 },
    { "OBS Studio",     "Record and stream your screen", "slop-launch",   SLOP_ICON_OBS,      1 },
    { "DaVinci Resolve","Professional video editing",    "slop-launch",   SLOP_ICON_RESOLVE,  1 },
    { "Calculator",     "Do quick maths",                "slop-calc",     SLOP_ICON_CALC,     0 },
    { "System Monitor", "Watch CPU, memory and load",    "slop-monitor",  SLOP_ICON_MONITOR,  0 },
    { "Settings",       "Configure your SlopOS desktop", "slop-settings", SLOP_ICON_SETTINGS, 0 },
    { "Notes",          "Jot things down",               "slop-notes",    SLOP_ICON_NOTES,    0 },
    { "System Info",    "About this SlopOS machine",     "slop-about",    SLOP_ICON_ABOUT,    0 },
};
static const int NAPPS = (int)(sizeof(APPS) / sizeof(APPS[0]));

/* ============================================================== windows */
#define MAX_WIN 32
#define TITLEBAR_H 32
#define BORDER 1

typedef struct {
    int   used;
    int   id;
    int   fd;
    uint32_t *pixels;       /* client surface (shared memory) */
    size_t pixels_size;
    int   buf_w, buf_h;
    int   x, y, w, h;
    int   saved_w, saved_h;
    int   saved_x, saved_y;
    int   maximized;
    int   minimized;
    int   icon;
    int   dirty;
    char  title[96];
    int   drag_mode;
    int   drag_edge;
    int   drag_dx, drag_dy;
} window_t;

static window_t wins[MAX_WIN];        /* index 0 = bottom, MAX_WIN-1 = top */
static int next_win_id = 1;
static int active = -1;
static int listen_fd = -1;
static int alt_tab_active = 0, alt_tab_sel = 0;

typedef struct { char title[64], body[128]; slop_color accent; int ttl, born; } toast_t;
static toast_t toasts[6];
static int ntoasts = 0;

static int menu_open = 0, qs_open = 0, cal_open = 0, notif_open = 0;
static int dock_hover = -1;
static volatile pid_t foreign_pid = 0;
static int lock_screen = 0;
static int have_fb = 0;
/* First-run help overlay: shown until the user opens something or dismisses
   it, so a new user is never stuck wondering how to reach the apps. It waits
   for the setup wizard to finish before appearing, so it does not cover the
   wizard itself. */
static int show_help = 0;
static int help_pending = 0;
static int help_seen_wizard = 0;
/* Set whenever something in the frame changed; when nothing is dirty the
   shell skips the expensive desktop recomposite. Software (non-KVM) VMs
   otherwise redraw a full-screen window blit every frame. */
static int shell_dirty = 1;

static const slop_color WALL_TOP = 0x10131f;
static const slop_color WALL_BOT = 0x08090f;

/* ============================================================== helpers */
static int win_at(int mx, int my) {
    for (int i = MAX_WIN - 1; i >= 0; i--) {
        window_t *w = &wins[i];
        if (!w->used || w->minimized) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return i;
    }
    return -1;
}
static int win_index(int id) {
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].id == id) return i;
    return -1;
}
static window_t *win_by_id(int id) {
    int i = win_index(id);
    return i >= 0 ? &wins[i] : NULL;
}
/* Move a window to the top of the stack and make it active. */
static void raise_id(int id) {
    int idx = win_index(id);
    if (idx < 0 || idx == MAX_WIN - 1) { if (idx >= 0) active = idx; shell_dirty = 1; return; }
    window_t tmp = wins[idx];
    for (int j = idx; j < MAX_WIN - 1; j++) wins[j] = wins[j + 1];
    wins[MAX_WIN - 1] = tmp;
    active = MAX_WIN - 1;
    shell_dirty = 1;
}
static void send_to_win(window_t *w, slop_msg *m) {
    if (w && w->fd >= 0) slop_send_msg(w->fd, m);
}
static void send_configure(window_t *w, int nw, int nh) {
    slop_msg m = {0};
    m.type = SLOP_MSG_CONFIGURE; m.magic = SLOP_PROTO_MAGIC;
    m.w = nw; m.h = nh;
    send_to_win(w, &m);
}
static void send_close(window_t *w) {
    slop_msg m = {0};
    m.type = SLOP_MSG_CLOSE; m.magic = SLOP_PROTO_MAGIC;
    send_to_win(w, &m);
}

/* ========================================================== notifications */
static void toast(const char *title, const char *body, slop_color accent) {
    if (ntoasts >= 6) {
        memmove(&toasts[0], &toasts[1], sizeof(toast_t) * 5);
        ntoasts = 5;
    }
    toast_t *t = &toasts[ntoasts++];
    snprintf(t->title, sizeof(t->title), "%s", title);
    snprintf(t->body, sizeof(t->body), "%s", body);
    t->accent = accent;
    t->ttl = 9000;
    t->born = slop_ticks_ms();
}
static int tick_toasts(int dt) {
    int changed = 0;
    for (int i = 0; i < ntoasts; i++) { toasts[i].ttl -= dt; if (toasts[i].ttl <= 0) changed = 1; }
    if (changed) {
        int j = 0;
        for (int i = 0; i < ntoasts; i++) if (toasts[i].ttl > 0) toasts[j++] = toasts[i];
        ntoasts = j;
    }
    return changed;
}

/* ============================================================== launching */
static int launch(int idx) {
    const app_entry *a = &APPS[idx];
    show_help = 0;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (a->third_party) {
            char id[16];
            snprintf(id, sizeof(id), "%d", idx);
            execl("/usr/bin/slop-launch", "slop-launch", a->name, id, (char *)NULL);
            execl("/bin/slop-launch", "slop-launch", a->name, id, (char *)NULL);
        } else {
            char p1[64], p2[64];
            snprintf(p1, sizeof(p1), "/usr/bin/%s", a->exec);
            snprintf(p2, sizeof(p2), "/bin/%s", a->exec);
            execl(p1, a->exec, (char *)NULL);
            execl(p2, a->exec, (char *)NULL);
        }
        _exit(127);
    }
    if (pid < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s could not be started", a->name);
        toast("Launch failed", msg, slop_theme_dark.bad);
    } else if (a->third_party) {
        foreign_pid = pid;
    }
    return 0;
}

/* ========================================================== compositor I/O */
static window_t *alloc_window(void) {
    for (int i = 0; i < MAX_WIN; i++) if (!wins[i].used) return &wins[i];
    return NULL;
}
static void drop_window(window_t *w) {
    if (w->pixels) { munmap(w->pixels, w->pixels_size); w->pixels = NULL; }
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    w->used = 0;
    if (active >= 0 && &wins[active] == w) active = -1;
    shell_dirty = 1;
}
static void accept_client(void) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    slop_msg hello;
    int n = (int)slop_recv_msg(fd, &hello, NULL);
    if (n != (int)sizeof(hello) || hello.type != SLOP_MSG_HELLO ||
        hello.magic != SLOP_PROTO_MAGIC) { close(fd); return; }

    window_t *w = alloc_window();
    if (!w) { close(fd); return; }
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->id = next_win_id++;
    w->fd = fd;
    w->icon = hello.icon;
    snprintf(w->title, sizeof(w->title), "%s", hello.title[0] ? hello.title : "Window");
    w->w = 760; w->h = 500;
    if (w->w > slop.w - 40) w->w = slop.w - 40;
    if (w->h > slop.h - 160) w->h = slop.h - 160;
    w->x = 90 + (w->id % 5) * 36;
    w->y = 70 + (w->id % 5) * 30;

    slop_msg welcome = {0};
    welcome.type = SLOP_MSG_WELCOME; welcome.magic = SLOP_PROTO_MAGIC;
    welcome.version = SLOP_PROTO_VERSION;
    welcome.id = w->id;
    welcome.w = w->w - 2;
    welcome.h = w->h - TITLEBAR_H - 1;
    welcome.focused = 1;
    slop_send_msg(fd, &welcome);
    raise_id(w->id);
    shell_dirty = 1;
}

static void service_client(int i) {
    window_t *w = &wins[i];
    if (!w->used || w->fd < 0) return;
    for (;;) {
        slop_msg m; int fd = -1;
        int n = (int)slop_recv_msg(w->fd, &m, &fd);
        if (n <= 0) {
            if (fd >= 0) close(fd);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            drop_window(w);
            return;
        }
        if (m.magic != SLOP_PROTO_MAGIC) { if (fd >= 0) close(fd); continue; }
        switch (m.type) {
        case SLOP_MSG_SURFACE:
            if (fd >= 0) {
                if (w->pixels) munmap(w->pixels, w->pixels_size);
                int sw = m.w > 0 ? m.w : w->w;
                int sh = m.h > 0 ? m.h : w->h;
                w->pixels_size = (size_t)sw * sh * 4;
                void *p = mmap(NULL, w->pixels_size, PROT_READ, MAP_SHARED, fd, 0);
                if (p != MAP_FAILED) { w->pixels = p; w->buf_w = sw; w->buf_h = sh; }
                close(fd);
                w->dirty = 1;
                shell_dirty = 1;
            }
            break;
        case SLOP_MSG_FRAME: w->dirty = 1; shell_dirty = 1; break;
        case SLOP_MSG_SET_TITLE: snprintf(w->title, sizeof(w->title), "%s", m.title); shell_dirty = 1; break;
        case SLOP_MSG_SET_ICON: w->icon = m.icon; shell_dirty = 1; break;
        case SLOP_MSG_BYE: drop_window(w); return;
        default: break;
        }
        if (fd >= 0) close(fd);
    }
}

/* ======================================================= input routing */
/* App coordinates are relative to the client surface, which the shell blits
   at (w->x + BORDER, w->y + TITLEBAR_H). Screen coordinates must be shifted by
   the same origin, not just w->x/w->y, or every click lands a titlebar high. */
static void to_client(window_t *w, int mx, int my, int *cx, int *cy) {
    *cx = mx - w->x - BORDER;
    *cy = my - w->y - TITLEBAR_H - BORDER;
}
static void route_motion(window_t *w, int mx, int my) {
    slop_msg m = {0};
    m.type = SLOP_MSG_EVENT; m.magic = SLOP_PROTO_MAGIC; m.kind = SLOP_PEV_MOTION;
    to_client(w, mx, my, &m.x, &m.y);
    send_to_win(w, &m);
}
static void route_button(window_t *w, int down, int btn, int mx, int my) {
    slop_msg m = {0};
    m.type = SLOP_MSG_EVENT; m.magic = SLOP_PROTO_MAGIC;
    m.kind = down ? SLOP_PEV_BUTTON_DOWN : SLOP_PEV_BUTTON_UP;
    m.button = btn;
    to_client(w, mx, my, &m.x, &m.y);
    send_to_win(w, &m);
}
static void route_wheel(window_t *w, int wheel, int mx, int my) {
    slop_msg m = {0};
    m.type = SLOP_MSG_EVENT; m.magic = SLOP_PROTO_MAGIC;
    m.kind = SLOP_PEV_WHEEL; m.wheel = wheel;
    to_client(w, mx, my, &m.x, &m.y);
    send_to_win(w, &m);
}
static void route_key(window_t *w, const slop_event *e) {
    slop_msg m = {0};
    m.type = SLOP_MSG_EVENT; m.magic = SLOP_PROTO_MAGIC;
    m.kind = SLOP_PEV_KEY; m.code = e->code; m.key = e->key; m.mods = e->mods;
    to_client(w, slop.mouse_x, slop.mouse_y, &m.x, &m.y);
    send_to_win(w, &m);
    if (e->ch) {
        slop_msg t = {0};
        t.type = SLOP_MSG_EVENT; t.magic = SLOP_PROTO_MAGIC;
        t.kind = SLOP_PEV_TEXT; t.ch = e->ch; t.mods = e->mods;
        to_client(w, slop.mouse_x, slop.mouse_y, &t.x, &t.y);
        send_to_win(w, &t);
    }
}
void route_event(const slop_event *e) {
    if (alt_tab_active) return;
    if (active < 0 || !wins[active].used) return;
    window_t *w = &wins[active];
    switch (e->type) {
    case SLOP_EV_MOUSE_MOVE: route_motion(w, e->x, e->y); break;
    case SLOP_EV_MOUSE_DOWN: route_button(w, 1, e->button, e->x, e->y); break;
    case SLOP_EV_MOUSE_UP:   route_button(w, 0, e->button, e->x, e->y); break;
    case SLOP_EV_WHEEL:      route_wheel(w, e->wheel, e->x, e->y); break;
    case SLOP_EV_KEY:        route_key(w, e); break;
    default: break;
    }
}

/* ============================================================== chrome */
static void draw_window(window_t *w, int focused) {
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    slop_shadow(x, y, ww, hh, 16, focused ? 120 : 70);
    slop_fill_round(x, y, ww, hh, 12, slop_theme_dark.surface);

    int cbx = x + BORDER, cby = y + TITLEBAR_H;
    int cw = ww - 2 * BORDER, ch = hh - TITLEBAR_H - BORDER;
    slop_fill_rect(cbx, cby, cw, ch, slop_theme_dark.bg);
    if (w->pixels) {
        int sw = w->buf_w < cw ? w->buf_w : cw;
        int sh = w->buf_h < ch ? w->buf_h : ch;
        for (int j = 0; j < sh; j++) {
            int dy = cby + j;
            const uint32_t *src = &w->pixels[(size_t)j * w->buf_w];
            uint32_t *dst = &slop.back[(size_t)dy * slop.w];
            for (int i2 = 0; i2 < sw; i2++) dst[cbx + i2] = src[i2];
        }
        if (w->buf_w < cw) slop_fill_rect(cbx + w->buf_w, cby, cw - w->buf_w, ch, slop_theme_dark.bg);
        if (w->buf_h < ch) slop_fill_rect(cbx, cby + w->buf_h, cw, ch - w->buf_h, slop_theme_dark.bg);
    }

    slop_color tb = focused ? slop_theme_dark.titlebar_focus : slop_theme_dark.titlebar;
    slop_fill_round(x, y, ww, TITLEBAR_H, 12, tb);
    slop_fill_rect(x, y + TITLEBAR_H - 10, ww, 10, tb);
    slop_hline(x, y + TITLEBAR_H - 1, ww, slop_theme_dark.border);

    int bx = x + 12, by = y + TITLEBAR_H / 2;
    int hc = (ui.mx >= bx - 7 && ui.mx < bx + 7 && ui.my >= by - 7 && ui.my < by + 7);
    slop_fill_circle(bx, by, 6, hc ? SLOP_RGB(0xff, 0x6b, 0x63) : SLOP_RGB(0xff, 0x5f, 0x57));
    slop_fill_circle(bx + 18, by, 6, SLOP_RGB(0xfe, 0xbc, 0x2e));
    slop_fill_circle(bx + 36, by, 6, SLOP_RGB(0x28, 0xc8, 0x40));
    if (hc) {
        slop_line(bx - 3, by - 3, bx + 3, by + 3, SLOP_RGB(0x5a, 0x14, 0x10));
        slop_line(bx + 3, by - 3, bx - 3, by + 3, SLOP_RGB(0x5a, 0x14, 0x10));
    }
    slop_icon_draw(w->icon, x + 72, by, 20, 0);
    const char *t = w->title;
    int maxw = ww - 160;
    char cut[96];
    if (slop_text_w(&slop_font_bold, t) > maxw) {
        int n2 = 0;
        while (n2 < (int)sizeof(cut) - 4 && t[n2] && slop_text_wn(&slop_font_bold, t, n2 + 1) < maxw) n2++;
        memcpy(cut, t, n2); cut[n2] = 0; strcat(cut, "...");
        t = cut;
    }
    slop_text_vcenter(&slop_font_bold, x + 90, y, TITLEBAR_H, t,
                      focused ? slop_theme_dark.text : slop_theme_dark.text_dim);
    slop_round_outline(x, y, ww, hh, 12, 1,
                       focused ? slop_theme_dark.border_hi : slop_theme_dark.border);
    if (!w->maximized) {
        int gx = x + ww - 16, gy = y + hh - 16;
        for (int i = 0; i < 3; i++)
            slop_line(gx + i * 5, gy + 12, gx + 12, gy + i * 5, slop_theme_dark.border_hi);
    }
}

static int frame_hit(window_t *w, int mx, int my, int *edge) {
    int g = 6;
    if (mx < w->x - g || mx > w->x + w->w + g || my < w->y - g || my > w->y + w->h + g)
        return 0;
    int m = 0;
    if (mx >= w->x - g && mx < w->x + g) m |= 1;
    if (mx >= w->x + w->w - g && mx < w->x + w->w + g) m |= 2;
    if (my >= w->y - g && my < w->y + g) m |= 4;
    if (my >= w->y + w->h - g && my < w->y + w->h + g) m |= 8;
    *edge = m;
    if (m) return 2;
    if (my >= w->y && my < w->y + TITLEBAR_H) return 1;
    return 0;
}

/* =========================================================== desktop art */
/* The wallpaper never changes, so it is painted once into a cached buffer and
   blitted each frame. Redrawing the full-screen gradient plus the star dither
   every frame dominated the frame time on software (non-KVM) machines. */
static slop_color *wall_cache = NULL;

static void build_wallpaper(void) {
    size_t n = (size_t)slop.w * slop.h;
    wall_cache = malloc(n * sizeof(slop_color));
    if (!wall_cache) return;
    slop_color *save = slop.back;
    slop.back = wall_cache;
    slop_vgradient(0, 0, slop.w, slop.h, WALL_TOP, WALL_BOT);
    float p = slop_pulse(9000);
    int cx = slop.w / 2;
    for (int i = 0; i < 3; i++) {
        int r = 240 + i * 130 + (int)(p * 18);
        slop_color col = i == 0 ? SLOP_RGB(0x2b, 0x3c, 0x8f)
                      : i == 1 ? SLOP_RGB(0x1e, 0x5a, 0x6e)
                               : SLOP_RGB(0x4a, 0x2a, 0x6e);
        for (int a = 0; a < 360; a += 3) {
            float rad = a * 3.14159265f / 180.0f;
            int x = cx + (int)(__builtin_cosf(rad) * r);
            int y = slop.h / 2 - 30 + (int)(__builtin_sinf(rad) * r / 2);
            if (x < 0 || x >= slop.w || y < 0 || y >= slop.h) continue;
            slop_blend(x, y, col, 24);
            slop_blend(x + 1, y, col, 24);
        }
    }
    for (int y = 0; y < slop.h; y += 3)
        for (int x = 0; x < slop.w; x += 3)
            slop_blend(x, y, SLOP_RGB(0xff, 0xff, 0xff), 5);
    slop.back = save;
}

static void draw_desktop(void) {
    if (!wall_cache) { build_wallpaper(); if (!wall_cache) return; }
    memcpy(slop.back, wall_cache, (size_t)slop.w * slop.h * sizeof(slop_color));
}

/* ============================================================== the panel */
static void draw_panel(void) {
    int h = 34;
    slop_fill_rect(0, 0, slop.w, h, SLOP_RGB(0x0d, 0x0f, 0x16));
    slop_hline(0, h - 1, slop.w, slop_theme_dark.border);

    int mx = 10;
    int mhot = ui.mx >= mx && ui.my >= 5 && ui.mx < mx + 118 && ui.my < h - 5;
    if (menu_open || mhot)
        slop_fill_round(mx, 5, 118, h - 10, 7,
                        menu_open ? slop_theme_dark.accent : slop_theme_dark.surface_hi);
    slop_fill_round(mx + 7, 8, 18, 18, 5, menu_open ? SLOP_RGB(255,255,255) : slop_theme_dark.accent);
    int sw = slop_text_w(&slop_font_bold, "S");
    slop_text(&slop_font_bold, mx + 7 + 9 - sw / 2, 22, "S",
              menu_open ? slop_theme_dark.accent : SLOP_RGB(255, 255, 255));
    slop_text(&slop_font_bold, mx + 32, 22, "SlopOS",
              menu_open ? SLOP_RGB(255,255,255) : slop_theme_dark.text);
    if (mhot && ui.released) { menu_open = !menu_open; qs_open = cal_open = notif_open = 0; }

    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    char clk[16], date[32];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    strftime(date, sizeof(date), "%a %d %b", &tm);
    int cw = slop_text_w(&slop_font_bold, clk);
    int dw = slop_text_w(&slop_font_small, date);
    int chot = ui.mx >= slop.w / 2 - cw / 2 - 10 && ui.mx < slop.w / 2 + cw / 2 + dw + 20 &&
               ui.my >= 4 && ui.my < h - 4;
    if (chot || cal_open)
        slop_fill_round(slop.w / 2 - cw / 2 - 10, 5, cw + dw + 30, h - 10, 7,
                        cal_open ? slop_theme_dark.accent : slop_theme_dark.surface_hi);
    slop_text(&slop_font_bold, slop.w / 2 - cw / 2, 22, clk,
              (chot || cal_open) ? SLOP_RGB(255,255,255) : slop_theme_dark.text);
    slop_text(&slop_font_small, slop.w / 2 + cw / 2 + 12, 21, date,
              (chot || cal_open) ? SLOP_RGB(220,226,255) : slop_theme_dark.text_mute);
    if (chot && ui.released) { cal_open = !cal_open; qs_open = menu_open = notif_open = 0; }

    int rx = slop.w - 14;
    const char *bat = "100%";
    int bw = slop_text_w(&slop_font_small, bat);
    slop_text(&slop_font_small, rx - bw, 21, bat, slop_theme_dark.text_dim);
    rx -= bw + 14;
    slop_round_outline(rx - 20, 12, 20, 11, 3, 1, slop_theme_dark.border_hi);
    slop_fill_round(rx - 18, 14, 16, 7, 1, SLOP_RGB(0x4a, 0xd9, 0x9a));
    slop_fill_rect(rx, 14, 2, 7, slop_theme_dark.border_hi);
    rx -= 34;

    int nbx = rx - 8;
    int nhot = ui.mx >= nbx - 8 && ui.mx < nbx + 16 && ui.my >= 5 && ui.my < h - 5;
    if (nhot || notif_open)
        slop_fill_round(nbx - 8, 5, 24, h - 10, 6,
                        notif_open ? slop_theme_dark.accent : slop_theme_dark.surface_hi);
    slop_fill_round(nbx - 3, 10, 12, 11, 3,
                    (nhot || notif_open) ? SLOP_RGB(255,255,255) : slop_theme_dark.text_dim);
    slop_fill_circle(nbx + 3, 22, 2, slop_theme_dark.text_dim);
    if (ntoasts > 0) slop_fill_circle(nbx + 8, 10, 3, slop_theme_dark.bad);
    if (nhot && ui.released) { notif_open = !notif_open; qs_open = menu_open = cal_open = 0; }
    rx -= 30;

    int qbx = rx - 8;
    int qhot = ui.mx >= qbx - 8 && ui.mx < qbx + 16 && ui.my >= 5 && ui.my < h - 5;
    if (qhot || qs_open)
        slop_fill_round(qbx - 8, 5, 24, h - 10, 6,
                        qs_open ? slop_theme_dark.accent : slop_theme_dark.surface_hi);
    for (int i = 0; i < 3; i++)
        slop_circle(qbx + 1, 22, 3 + i * 4,
                    (qhot || qs_open) ? SLOP_RGB(255,255,255) : slop_theme_dark.text_dim);
    slop_fill_rect(qbx - 9, 20, 20, 4, SLOP_RGB(0x0d, 0x0f, 0x16));
    if (qhot && ui.released) { qs_open = !qs_open; menu_open = cal_open = notif_open = 0; }
}

/* =============================================================== launcher */
static void draw_launcher(void) {
    int gw = 4, cell = 156, pad = 44;
    int rows = (NAPPS + gw - 1) / gw;
    int w = gw * cell + pad * 2;
    int h = rows * (cell + 8) + pad * 2 + 30;
    int x = (slop.w - w) / 2, y = (slop.h - h) / 2 - 10;
    slop_fill_rect_a(0, 0, slop.w, slop.h, 0, 140);
    slop_shadow(x, y, w, h, 26, 130);
    slop_fill_round(x, y, w, h, 22, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 22, 1, slop_theme_dark.border_hi);
    slop_text(&slop_font_title, x + pad, y + 52, "Applications", slop_theme_dark.text);
    for (int i = 0; i < NAPPS; i++) {
        int gx = x + pad + (i % gw) * cell;
        int gy = y + pad + 30 + (i / gw) * (cell + 8);
        int hot = ui.mx >= gx && ui.mx < gx + cell - 16 && ui.my >= gy && ui.my < gy + cell - 16;
        if (hot) slop_fill_round(gx, gy, cell - 16, cell - 16, 16, slop_theme_dark.surface_hi);
        slop_icon_draw(APPS[i].icon, gx + (cell - 16) / 2, gy + 46, 62, hot);
        int nw = slop_text_w(&slop_font_bold, APPS[i].name);
        slop_text(&slop_font_bold, gx + (cell - 16) / 2 - nw / 2, gy + 98, APPS[i].name,
                  slop_theme_dark.text);
        if (APPS[i].third_party) {
            const char *c = "compatibility";
            int cw2 = slop_text_w(&slop_font_small, c);
            slop_text(&slop_font_small, gx + (cell - 16) / 2 - cw2 / 2, gy + 116, c,
                      slop_theme_dark.text_mute);
        }
        if (hot && ui.released) { menu_open = 0; launch(i); }
    }
    if (ui.released && !(ui.mx >= x && ui.mx < x + w && ui.my >= y && ui.my < y + h))
        menu_open = 0;
}

/* ================================================================ toasts */
static void draw_toasts(void) {
    int y = 46;
    for (int i = 0; i < ntoasts; i++) {
        toast_t *t = &toasts[i];
        int age = slop_ticks_ms() - t->born;
        float slide = slop_ease_out_cubic(age / 260.0f > 1 ? 1 : age / 260.0f);
        int w = 320, hh = 64;
        int x = slop.w - (int)((w + 20) * slide);
        slop_shadow(x, y, w, hh, 16, 110);
        slop_fill_round(x, y, w, hh, 13, slop_theme_dark.surface);
        slop_round_outline(x, y, w, hh, 13, 1, slop_theme_dark.border_hi);
        slop_fill_round(x, y, 5, hh, 2, t->accent);
        slop_text(&slop_font_bold, x + 18, y + 26, t->title, slop_theme_dark.text);
        slop_text(&slop_font_small, x + 18, y + 45, t->body, slop_theme_dark.text_dim);
        y += hh + 10;
    }
}

/* ========================================================== quick settings */
static void draw_quick_settings(void) {
    int w = 320, h = 300, x = slop.w - w - 16, y = 44;
    slop_shadow(x, y, w, h, 18, 120);
    slop_fill_round(x, y, w, h, 16, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 16, 1, slop_theme_dark.border_hi);
    slop_text(&slop_font_bold, x + 20, y + 34, "Quick Settings", slop_theme_dark.text);
    static int wifi = 1, bt = 0, dnd = 0, night = 0;
    const char *labels[4] = { "Wi-Fi", "Bluetooth", "Do Not Disturb", "Night Light" };
    int *vals[4] = { &wifi, &bt, &dnd, &night };
    for (int i = 0; i < 4; i++) {
        int ty = y + 54 + i * 46;
        slop_text_vcenter(&slop_font_body, x + 20, ty, 34, labels[i], slop_theme_dark.text_dim);
        slop_toggle(x + w - 64, ty + 5, vals[i]);
    }
    slop_hline(x + 20, y + h - 88, w - 40, slop_theme_dark.border);
    slop_text(&slop_font_small, x + 20, y + h - 62, "Brightness", slop_theme_dark.text_mute);
    static int bright = 80;
    slop_slider(x + 20, y + h - 46, w - 40, &bright, 20, 100, NULL);
    if (ui.released && !(ui.mx >= x && ui.mx < x + w && ui.my >= y && ui.my < y + h)) qs_open = 0;
}

/* =============================================================== calendar */
static void draw_calendar(void) {
    int w = 320, h = 300, x = (slop.w - w) / 2, y = 44;
    slop_shadow(x, y, w, h, 18, 120);
    slop_fill_round(x, y, w, h, 16, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 16, 1, slop_theme_dark.border_hi);
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    char title[64];
    strftime(title, sizeof(title), "%B %Y", &tm);
    slop_text(&slop_font_bold, x + 20, y + 34, title, slop_theme_dark.text);
    static const char *wd[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    int cw = (w - 40) / 7;
    for (int i = 0; i < 7; i++) {
        int tw = slop_text_w(&slop_font_small, wd[i]);
        slop_text(&slop_font_small, x + 20 + i * cw + cw / 2 - tw / 2, y + 62, wd[i],
                  slop_theme_dark.text_mute);
    }
    struct tm first = tm; first.tm_mday = 1; mktime(&first);
    int lead = (first.tm_wday + 6) % 7;
    int mm = tm.tm_mon, yy = tm.tm_year + 1900;
    int days = (mm == 1) ? ((yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 29 : 28)
             : (mm == 3 || mm == 5 || mm == 8 || mm == 10) ? 30 : 31;
    int row = 0, col = lead;
    for (int d = 1; d <= days; d++) {
        int cx2 = x + 20 + col * cw, cy2 = y + 78 + row * 32;
        int today = (d == tm.tm_mday);
        if (today) slop_fill_circle(cx2 + cw / 2, cy2 + 14, 14, slop_theme_dark.accent);
        char ds[8]; snprintf(ds, sizeof(ds), "%d", d);
        int tw = slop_text_w(&slop_font_body, ds);
        slop_text(&slop_font_body, cx2 + cw / 2 - tw / 2, cy2 + 19, ds,
                  today ? SLOP_RGB(255,255,255) : slop_theme_dark.text_dim);
        if (++col == 7) { col = 0; row++; }
    }
    if (ui.released && !(ui.mx >= x && ui.mx < x + w && ui.my >= y && ui.my < y + h)) cal_open = 0;
}

/* ==================================================== notification centre */
static void draw_notif_center(void) {
    int w = 340;
    int rows = ntoasts > 0 ? ntoasts : 1;
    int h = 60 + rows * 78;
    if (h > slop.h - 120) h = slop.h - 120;
    int x = slop.w - w - 16, y = 44;
    slop_shadow(x, y, w, h, 18, 120);
    slop_fill_round(x, y, w, h, 16, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 16, 1, slop_theme_dark.border_hi);
    slop_text(&slop_font_bold, x + 20, y + 34, "Notifications", slop_theme_dark.text);
    if (ntoasts == 0) {
        slop_text(&slop_font_small, x + 20, y + 78, "Nothing new. Enjoy the quiet.",
                  slop_theme_dark.text_mute);
    } else {
        for (int i = 0; i < ntoasts && i < 6; i++) {
            int ry = y + 48 + i * 78;
            slop_fill_round(x + 14, ry, w - 28, 68, 10, slop_theme_dark.surface_hi);
            slop_fill_round(x + 14, ry, 4, 68, 2, toasts[i].accent);
            slop_text(&slop_font_bold, x + 28, ry + 26, toasts[i].title, slop_theme_dark.text);
            slop_text(&slop_font_small, x + 28, ry + 46, toasts[i].body, slop_theme_dark.text_dim);
        }
    }
    if (ui.released && !(ui.mx >= x && ui.mx < x + w && ui.my >= y && ui.my < y + h)) notif_open = 0;
}

/* ============================================================ help overlay */
/* A short, always-visible-until-dismissed cheat sheet. The old build relied on
   a 4-second toast, which users missed and then had no way to rediscover. */
static void draw_help(void) {
    int w = 460, h = 250;
    int x = (slop.w - w) / 2, y = (slop.h - h) / 2 - 40;
    slop_shadow(x, y, w, h, 20, 140);
    slop_fill_round(x, y, w, h, 18, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 18, 1, slop_theme_dark.border_hi);

    slop_text(&slop_font_title, x + 28, y + 46, "Welcome to SlopOS", slop_theme_dark.text);
    slop_text(&slop_font_small, x + 28, y + 72,
              "Here is how to get around. This card stays until you dismiss it.",
              slop_theme_dark.text_dim);

    const char *rows[][2] = {
        { "Open an app",   "Click an icon in the dock below, or press 1-9" },
        { "All apps",      "Press M for the launcher" },
        { "Close window",  "Ctrl+Q, or the title-bar button" },
        { "Show desktop",  "Press Esc to dismiss menus and this card" },
    };
    for (int i = 0; i < 4; i++) {
        int ry = y + 100 + i * 30;
        slop_fill_round(x + 28, ry - 12, 8, 8, 4, slop_theme_dark.accent);
        slop_text(&slop_font_body, x + 46, ry, rows[i][0], slop_theme_dark.text);
        slop_text(&slop_font_small, x + 200, ry, rows[i][1], slop_theme_dark.text_dim);
    }

    int bw = 170, bh = 38;
    slop_button b = { x + w - 28 - bw, y + h - 28 - bh, bw, bh,
                      "Got it", 1, 1, 0, 0 };
    if (slop_button_draw(&b) || ui.key == SLOP_KEY_ESC || ui.released) {
        show_help = 0;
        int fd = open("/home/user/.config/slop/seen-help", O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) close(fd);
    }
}

/* ================================================================ dock */
static void draw_dock(void) {
    int n = NAPPS, isz = 50, gap = 12;
    int total = n * isz + (n - 1) * gap;
    int x0 = (slop.w - total) / 2;
    int y = slop.h - isz - 20;
    dock_hover = -1;
    slop_shadow(x0 - 14, y - 10, total + 28, isz + 20, 20, 100);
    slop_fill_round(x0 - 14, y - 10, total + 28, isz + 20, 20, SLOP_RGB(0x14, 0x17, 0x22));
    slop_round_outline(x0 - 14, y - 10, total + 28, isz + 20, 20, 1, slop_theme_dark.border);
    for (int i = 0; i < n; i++) {
        int cx = x0 + i * (isz + gap) + isz / 2;
        int cy = y + isz / 2;
        int box = isz / 2 + 4;
        int hot = ui.mx >= cx - box && ui.mx < cx + box && ui.my >= cy - box && ui.my < cy + box;
        if (hot) dock_hover = i;
        slop_icon_draw(APPS[i].icon, cx, cy, isz, hot);
        if (hot) {
            int tw = slop_text_w(&slop_font_small, APPS[i].name);
            int tx = cx - tw / 2, ty = y - 30;
            slop_fill_round(tx - 8, ty - 16, tw + 16, 24, 6, SLOP_RGB(0x24, 0x29, 0x38));
            slop_text(&slop_font_small, tx, ty, APPS[i].name, slop_theme_dark.text);
        }
        int running = 0;
        for (int k = 0; k < MAX_WIN; k++)
            if (wins[k].used && wins[k].icon == APPS[i].icon) running = 1;
        if (running) slop_fill_circle(cx, y + isz + 8, 3, slop_theme_dark.accent);
    }
    if (dock_hover >= 0 && ui.released) launch(dock_hover);
}

/* ============================================================ task switcher */
static void draw_alt_tab(void) {
    int count = 0;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && !wins[i].minimized) count++;
    if (count == 0) return;
    int cw = 190, ch = 150, gap = 16;
    int w = count * cw + (count - 1) * gap + 40;
    int h = ch + 40;
    int x = (slop.w - w) / 2, y = (slop.h - h) / 2;
    slop_fill_rect_a(0, 0, slop.w, slop.h, 0, 130);
    slop_fill_round(x, y, w, h, 16, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 16, 1, slop_theme_dark.border_hi);
    int k = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used || wins[i].minimized) continue;
        int px = x + 20 + k * (cw + gap), py = y + 20;
        if (k == (alt_tab_sel % count))
            slop_fill_round(px - 6, py - 6, cw + 12, ch + 12, 12, slop_theme_dark.accent_lo);
        slop_fill_round(px, py, cw, ch, 10, slop_theme_dark.titlebar_focus);
        slop_round_outline(px, py, cw, ch, 10, 1, slop_theme_dark.border_hi);
        if (wins[i].pixels) {
            int tw = cw - 16, th = ch - 40;
            for (int j = 0; j < th; j++) {
                int sy = j * wins[i].buf_h / th;
                for (int i2 = 0; i2 < tw; i2++) {
                    int sx = i2 * wins[i].buf_w / tw;
                    slop_blend(px + 8 + i2, py + 8 + j,
                               wins[i].pixels[(size_t)sy * wins[i].buf_w + sx], 255);
                }
            }
        }
        slop_icon_draw(wins[i].icon, px + 18, py + ch - 18, 20, 0);
        slop_text(&slop_font_small, px + 34, py + ch - 12, wins[i].title, slop_theme_dark.text);
        k++;
    }
}

/* ============================================================== main */
/* A tiny autostart hook used by `make verify-x11`: when /run/slop/autostart
   contains a single index, the shell launches that dock app shortly after
   starting, which lets a headless QEMU run exercise the real X11 bridge. */
static void maybe_autostart(void) {
    int fd = open("/run/slop/autostart", O_RDONLY);
    if (fd < 0) fd = open("/usr/share/slop/autostart", O_RDONLY);
    if (fd < 0) return;
    char buf[64] = {0};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return;
    unlink("/run/slop/autostart");
    show_help = 0;
    /* trim */
    for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\r' || *p == ' ') { *p = 0; break; }
    if (!buf[0]) return;

    int numeric = 1;
    for (char *p = buf; *p; p++) if (*p < '0' || *p > '9') numeric = 0;
    if (numeric) {
        int idx = atoi(buf);
        if (idx >= 0 && idx < NAPPS) launch(idx);
        return;
    }
    /* a raw compatibility app id: drive slop-launch directly */
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/usr/bin/slop-launch", "slop-launch", buf, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) foreign_pid = pid;
}

static void install_socket(void) {
    unlink(SLOP_COMPOSITOR_SOCK);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) { perror("shell: socket"); return; }
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", SLOP_COMPOSITOR_SOCK);
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) perror("shell: bind");
    listen(listen_fd, 16);
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);
}

int main(void) {
    have_fb = (slop_init() == 0);
    if (!have_fb) { fprintf(stderr, "slop-shell: cannot open framebuffer\n"); return 1; }

    install_socket();
    slop_event_hook = route_event;
    slop_flush_events();
    show_help = 0;
    help_pending = (access("/home/user/.config/slop/seen-help", F_OK) != 0);
    maybe_autostart();

    toast("Welcome to SlopOS 0.2", "Open apps from the dock, or press 1-9 / M",
          slop_theme_dark.accent);

    int last_tick = slop_ticks_ms();

    for (;;) {
        /* Third-party apps run inside their own compositor window (the X11
           bridge), so they no longer take over the screen. Just reap the
           launcher when it exits and tell the user. */
        if (foreign_pid) {
            int st = 0;
            if (waitpid(foreign_pid, &st, WNOHANG) == foreign_pid) {
                foreign_pid = 0;
                toast("Application closed", "The compatibility session has ended",
                      slop_theme_dark.good);
            }
        }

        slop_begin_frame();
        if (ui.moved || ui.down || ui.pressed || ui.released || ui.rdown ||
            ui.rpressed || ui.ntext || ui.key || ui.wheel)
            shell_dirty = 1;

        /* The setup wizard is the only window on first boot. Once it has
           appeared and then closed, offer the help card. */
        if (help_pending) {
            int wizard_open = 0;
            for (int i = 0; i < MAX_WIN; i++)
                if (wins[i].used && strstr(wins[i].title, "Setup")) wizard_open = 1;
            if (wizard_open) {
                help_seen_wizard = 1;
            } else if (help_seen_wizard) {
                show_help = 1;
                help_pending = 0;
                shell_dirty = 1;
            }
        }

        int now = slop_ticks_ms();
        int dt = now - last_tick;
        last_tick = now;
        if (tick_toasts(dt)) shell_dirty = 1;
        if (ntoasts > 0) shell_dirty = 1;   /* toasts slide and expire every frame */

        /* alt-tab */
        if (ui.key == SLOP_KEY_TAB && (ui.alt || ui.super)) alt_tab_active = 1;
        if (alt_tab_active && ui.key == SLOP_KEY_TAB && ui.released) alt_tab_sel++;
        if (alt_tab_active && !ui.alt) {
            int count = 0;
            for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && !wins[i].minimized) count++;
            if (count > 0) {
                int target = alt_tab_sel % count;
                int k = 0;
                for (int i = 0; i < MAX_WIN; i++) {
                    if (!wins[i].used || wins[i].minimized) continue;
                    if (k == target) { raise_id(wins[i].id); break; }
                    k++;
                }
            }
            alt_tab_active = 0;
            alt_tab_sel = 0;
        }

        /* compositor clients */
        if (listen_fd >= 0) {
            struct pollfd pfd = { listen_fd, POLLIN, 0 };
            if (poll(&pfd, 1, 0) > 0) accept_client();
        }
        for (int i = 0; i < MAX_WIN; i++) if (wins[i].used) service_client(i);

        /* window interaction */
        int mx = ui.mx, my = ui.my;
        int hit = -1;
        if (my > 34 && my < slop.h - 72) hit = win_at(mx, my);

        if (ui.released && !ui.down) {
            for (int i = 0; i < MAX_WIN; i++) wins[i].drag_mode = 0;
        }

        if (ui.pressed && hit >= 0) {
            int id = wins[hit].id;
            int edge = 0;
            int mode = frame_hit(&wins[hit], mx, my, &edge);
            raise_id(id);
            window_t *w = win_by_id(id);
            if (w) {
                if (mode == 1) { w->drag_mode = 1; w->drag_dx = mx - w->x; w->drag_dy = my - w->y; }
                else if (mode == 2) { w->drag_mode = 2; w->drag_edge = edge;
                                      w->drag_dx = mx - w->x; w->drag_dy = my - w->y; }
                /* traffic lights */
                if (my < w->y + TITLEBAR_H) {
                    if (mx >= w->x + 5 && mx < w->x + 19) {
                        send_close(w);
                    } else if (mx >= w->x + 23 && mx < w->x + 37) {
                        if (!w->maximized) {
                            w->saved_x = w->x; w->saved_y = w->y;
                            w->saved_w = w->w; w->saved_h = w->h;
                            w->x = 0; w->y = 34; w->w = slop.w; w->h = slop.h - 34 - 78;
                            w->maximized = 1;
                        } else {
                            w->x = w->saved_x; w->y = w->saved_y;
                            w->w = w->saved_w; w->h = w->saved_h;
                            w->maximized = 0;
                        }
                        send_configure(w, w->w - 2, w->h - TITLEBAR_H - 1);
                    } else if (mx >= w->x + 41 && mx < w->x + 55) {
                        w->minimized = 1;
                    }
                }
            }
        }

        for (int i = 0; i < MAX_WIN; i++) {
            window_t *w = &wins[i];
            if (!w->used || !w->drag_mode || !ui.down) continue;
            if (w->drag_mode == 1) {
                w->x = mx - w->drag_dx;
                w->y = my - w->drag_dy;
                if (w->y < 34) w->y = 34;
            } else {
                int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
                if (w->drag_edge & 1) { nx = mx - w->drag_dx; nw = w->x + w->w - nx; }
                if (w->drag_edge & 2) { nw = mx - w->x; }
                if (w->drag_edge & 4) { ny = my - w->drag_dy; nh = w->y + w->h - ny; }
                if (w->drag_edge & 8) { nh = my - w->y; }
                if (nw < 320) nw = 320;
                if (nh < 220) nh = 220;
                if (ny < 34) { nh += 34 - ny; ny = 34; }
                w->x = nx; w->y = ny; w->w = nw; w->h = nh;
            }
            send_configure(w, w->w - 2, w->h - TITLEBAR_H - 1);
        }

        if (ui.key == SLOP_KEY_ESC) { menu_open = qs_open = cal_open = notif_open = 0; }
        if (ui.ctrl && ui.ctrl_ch == 'q' && active >= 0 && wins[active].used) send_close(&wins[active]);

        /* keyboard shortcuts: 1-9 launch, M launcher, L lock */
        for (int i = 0; i < ui.ntext; i++) {
            char c = ui.text[i];
            if (c >= '1' && c <= '9') { int idx = c - '1'; if (idx < NAPPS) launch(idx); }
            else if (c == 'm' || c == 'M') { menu_open = !menu_open; qs_open = cal_open = notif_open = 0; }
            else if (c == 'l' || c == 'L') lock_screen = !lock_screen;
        }

        /* draw. When nothing changed, the composed frame from last time is
           still valid, so we only redraw the software cursor and present. This
           keeps an idle desktop nearly free on slow, non-accelerated VMs. */
        int need_present = 0;
        if (shell_dirty) {
            draw_desktop();
            for (int i = 0; i < MAX_WIN; i++) {
                if (!wins[i].used || wins[i].minimized) continue;
                draw_window(&wins[i], i == active);
            }
            draw_panel();
            draw_dock();
            if (menu_open) draw_launcher();
            if (qs_open) draw_quick_settings();
            if (cal_open) draw_calendar();
            if (notif_open) draw_notif_center();
            if (alt_tab_active) draw_alt_tab();
            if (show_help && !lock_screen) draw_help();
            draw_toasts();
            if (lock_screen) {
                slop_fill_rect_a(0, 0, slop.w, slop.h, 0, 240);
                const char *ttl = "SlopOS";
                slop_text(&slop_font_title, slop.w / 2 - slop_text_w(&slop_font_title, ttl) / 2,
                          slop.h / 2, ttl, slop_theme_dark.text);
                const char *m = "Screen locked. Press any key to unlock.";
                slop_text(&slop_font_small, slop.w / 2 - slop_text_w(&slop_font_small, m) / 2,
                          slop.h / 2 + 30, m, slop_theme_dark.text_mute);
                if (ui.ntext || ui.key) lock_screen = 0;
            }
            shell_dirty = 0;
            need_present = 1;
        } else if (ui.moved) {
            /* only the pointer moved: restore the old cursor and redraw it */
            need_present = 1;
        }

        if (need_present) slop_end_frame();
        usleep(2000);
    }
    slop_shutdown();
    return 0;
}
