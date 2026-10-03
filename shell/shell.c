/*
 * slop-shell -- the SlopOS desktop session.
 *
 * Draws the panel and dock, launches native and third-party apps, and comes
 * back cleanly when they exit. Each app is a fullscreen process; the shell is
 * simply the thing that owns the screen between them.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>

enum { ICON_FILES, ICON_TERM, ICON_IMAGE, ICON_VIEW, ICON_ZEN, ICON_OBS,
       ICON_RESOLVE, ICON_ABOUT, ICON_COUNT };

typedef struct {
    const char *name;
    const char *desc;
    const char *exec;   /* argv[0] */
    int         icon;
    int         third_party;
} app_entry;

static const app_entry APPS[] = {
    { "Files",          "Browse and manage your files",        "slop-files",    ICON_FILES,   0 },
    { "Terminal",       "Run commands and programs",           "slop-term",     ICON_TERM,    0 },
    { "Image Viewer",   "Look at photos and graphics",         "slop-image",    ICON_IMAGE,   0 },
    { "File Viewer",    "Read text, code and documents",       "slop-view",     ICON_VIEW,    0 },
    { "Zen Browser",    "Fast, private web browsing",          "slop-launch",   ICON_ZEN,     1 },
    { "OBS Studio",     "Record and stream your screen",       "slop-launch",   ICON_OBS,     1 },
    { "DaVinci Resolve","Professional video editing",          "slop-launch",   ICON_RESOLVE, 1 },
    { "System Info",    "About this SlopOS machine",           "slop-about",    ICON_ABOUT,   0 },
};
static const int NAPPS = (int)(sizeof(APPS) / sizeof(APPS[0]));

/* ================================================================ icons */
static void icon_draw(int icon, int cx, int cy, int s, int hover) {
    int r = s / 2;
    /* tile */
    slop_color tile = hover ? slop_theme_dark.surface_hi : slop_theme_dark.surface;
    slop_fill_round(cx - r, cy - r, s, s, s / 5, tile);
    slop_round_outline(cx - r, cy - r, s, s, s / 5, 1, slop_theme_dark.border);
    switch (icon) {
    case ICON_FILES: {
        int w = s * 5 / 9, h = s * 4 / 9;
        int x = cx - w / 2, y = cy - h / 2 + 2;
        slop_fill_round(x, y - 4, w / 2, 6, 2, SLOP_RGB(0xf0, 0xb4, 0x3c));
        slop_fill_round(x, y, w, h, 3, SLOP_RGB(0xf0, 0xb4, 0x3c));
        slop_fill_round(x, y, w, h / 2, 3, SLOP_RGB(0xf7, 0xcd, 0x6a));
        break;
    }
    case ICON_TERM: {
        int w = s * 3 / 5, h = s * 3 / 5;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 4, SLOP_RGB(0x0a, 0x0c, 0x12));
        slop_round_outline(x, y, w, h, 4, 1, slop_theme_dark.border_hi);
        slop_text(&slop_font_monob, x + 5, y + h / 2 + 2, ">_", slop_theme_dark.good);
        break;
    }
    case ICON_IMAGE: {
        int w = s * 3 / 5, h = s * 3 / 5;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 4, SLOP_RGB(0x1d, 0x24, 0x36));
        slop_fill_circle(x + w / 4, y + h / 4, 3, SLOP_RGB(0xf0, 0xb4, 0x3c));
        for (int i = 0; i < w - 8; i++) {
            int hh = (w - 8 - i) / 2;
            slop_vline(x + 4 + i, y + h - 5 - hh, hh, SLOP_RGB(0x4a, 0xd9, 0x9a));
        }
        slop_round_outline(x, y, w, h, 4, 1, slop_theme_dark.border_hi);
        break;
    }
    case ICON_VIEW: {
        int w = s * 4 / 9, h = s * 5 / 9;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 3, SLOP_RGB(0xe6, 0xe9, 0xf0));
        for (int i = 0; i < 4; i++)
            slop_fill_rect(x + 4, y + 8 + i * 6, w - 8, 2, SLOP_RGB(0x8a, 0x93, 0xa8));
        break;
    }
    case ICON_ZEN: {
        slop_fill_circle(cx, cy, s / 4, SLOP_RGB(0x2b, 0x6c, 0xff));
        slop_circle(cx, cy, s / 4, SLOP_RGB(0x8a, 0xa4, 0xff));
        slop_line(cx - s / 4, cy, cx + s / 4, cy, SLOP_RGB(0xff, 0xff, 0xff));
        slop_line(cx, cy - s / 4, cx, cy + s / 4, SLOP_RGB(0xff, 0xff, 0xff));
        break;
    }
    case ICON_OBS: {
        slop_fill_circle(cx, cy, s / 4, SLOP_RGB(0x1b, 0x1e, 0x26));
        slop_circle(cx, cy, s / 4, SLOP_RGB(0x9a, 0xa2, 0xb8));
        slop_fill_circle(cx, cy, s / 8, SLOP_RGB(0xf0, 0x6a, 0x6a));
        break;
    }
    case ICON_RESOLVE: {
        int w = s * 3 / 5, h = s * 3 / 5;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 4, SLOP_RGB(0x12, 0x14, 0x1d));
        slop_fill_round(x + 3, y + 3, w / 2 - 4, 5, 2, SLOP_RGB(0x6c, 0x8c, 0xff));
        slop_fill_round(x + 3, y + 10, w / 2 - 4, 5, 2, SLOP_RGB(0x4a, 0xd9, 0x9a));
        slop_fill_round(x + 3, y + 17, w / 2 - 4, 5, 2, SLOP_RGB(0xf0, 0xb4, 0x3c));
        break;
    }
    case ICON_ABOUT: {
        slop_fill_circle(cx, cy, s / 4, slop_theme_dark.accent);
        int tw = slop_text_w(&slop_font_bold, "i");
        slop_text(&slop_font_bold, cx - tw / 2, cy + 6, "i", SLOP_RGB(255, 255, 255));
        break;
    }
    }
}

/* ============================================================= launching */
static volatile int child_pid = 0;

static void toast(const char *title, const char *body, slop_color accent);

static int launch(int idx) {
    const app_entry *a = &APPS[idx];
    char id[32];
    snprintf(id, sizeof(id), "%d", idx);

    /* Show a launch splash so third-party apps (which take a moment to start)
       never look like a frozen screen. */
    slop_clear(slop_theme_dark.bg);
    slop_vgradient(0, 0, slop.w, slop.h, SLOP_RGB(0x10, 0x13, 0x1f), slop_theme_dark.bg);
    int cx = slop.w / 2, cy = slop.h / 2;
    icon_draw(a->icon, cx, cy - 30, 96, 0);
    int nw = slop_text_w(&slop_font_title, a->name);
    slop_text(&slop_font_title, cx - nw / 2, cy + 70, a->name, slop_theme_dark.text);
    int dw = slop_text_w(&slop_font_small, a->desc);
    slop_text(&slop_font_small, cx - dw / 2, cy + 94, a->desc, slop_theme_dark.text_mute);
    if (a->third_party) {
        const char *c = "Starting through the SlopOS compatibility runtime...";
        int cw = slop_text_w(&slop_font_small, c);
        slop_text(&slop_font_small, cx - cw / 2, cy + 122, c, slop_theme_dark.accent);
    }
    slop_present();
    usleep(350000);
    slop_flush_events();

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (a->third_party) {
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
    child_pid = pid;
    int st = 0;
    waitpid(pid, &st, 0);
    child_pid = 0;
    slop_flush_events();
    if (WEXITSTATUS(st) == 127) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s could not be started", a->name);
        toast("Launch failed", msg, slop_theme_dark.bad);
    }
    return 0;
}

/* ================================================================ widgets */
static void panel_draw(int *menu_open, int *clock_clicked) {
    int h = 34;
    slop_fill_rect(0, 0, slop.w, h, slop_theme_dark.titlebar);
    slop_hline(0, h - 1, slop.w, slop_theme_dark.border);

    /* left: menu */
    int mx = 10;
    int mhot = ui.mx >= mx && ui.my >= 6 && ui.mx < mx + 120 && ui.my < h - 6;
    if (mhot) slop_fill_round(mx, 6, 120, h - 12, 6, slop_theme_dark.surface_hi);
    slop_fill_round(mx + 6, 9, 16, 16, 5, slop_theme_dark.accent);
    int sw = slop_text_w(&slop_font_bold, "S");
    slop_text(&slop_font_bold, mx + 6 + 8 - sw / 2, 22, "S", SLOP_RGB(255, 255, 255));
    slop_text(&slop_font_bold, mx + 30, 22, "SlopOS", slop_theme_dark.text);
    if (mhot && ui.released) *menu_open = !*menu_open;

    /* center: clock + date */
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char clk[16], date[32];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    strftime(date, sizeof(date), "%a %d %b", &tm);
    int cw = slop_text_w(&slop_font_bold, clk);
    slop_text(&slop_font_bold, slop.w / 2 - cw / 2, 22, clk, slop_theme_dark.text);
    slop_text(&slop_font_small, slop.w / 2 + cw / 2 + 12, 21, date, slop_theme_dark.text_mute);

    /* right: status */
    int rx = slop.w - 20;
    const char *bat = "100%";
    int bw = slop_text_w(&slop_font_small, bat);
    slop_text(&slop_font_small, rx - bw, 21, bat, slop_theme_dark.text_dim);
    rx -= bw + 16;
    slop_fill_round(rx - 18, 12, 18, 10, 2, slop_theme_dark.good);
    rx -= 30;
    /* wifi arcs */
    for (int i = 0; i < 3; i++)
        slop_circle(rx - 10, 24, 3 + i * 4, slop_theme_dark.text_dim);
    slop_fill_rect(rx - 20, 22, 20, 3, slop_theme_dark.titlebar);
    (void)clock_clicked;
}

static void dock_draw(int *hover_idx) {
    int n = NAPPS;
    int isz = 52, gap = 14;
    int total = n * isz + (n - 1) * gap;
    int x0 = (slop.w - total) / 2;
    int y = slop.h - isz - 22;
    *hover_idx = -1;
    /* dock background */
    slop_fill_round(x0 - 14, y - 12, total + 28, isz + 24, 20, slop_theme_dark.titlebar_focus);
    slop_round_outline(x0 - 14, y - 12, total + 28, isz + 24, 20, 1, slop_theme_dark.border);
    for (int i = 0; i < n; i++) {
        int cx = x0 + i * (isz + gap) + isz / 2;
        int cy = y + isz / 2;
        int hot = ui.mx >= cx - isz / 2 && ui.mx < cx + isz / 2 &&
                  ui.my >= cy - isz / 2 && ui.my < cy + isz / 2;
        if (hot) *hover_idx = i;
        icon_draw(APPS[i].icon, cx, cy, isz, hot);
    }
}

static void launcher_draw(int *open) {
    int gw = 4, cell = 150, pad = 40;
    int rows = (NAPPS + gw - 1) / gw;
    (void)rows;
    int w = gw * cell + pad * 2;
    int h = ((NAPPS + gw - 1) / gw) * (cell + 10) + pad * 2;
    int x = (slop.w - w) / 2, y = (slop.h - h) / 2 - 20;
    slop_fill_rect_a(0, 0, slop.w, slop.h, 0, 120);
    slop_fill_round(x, y, w, h, 20, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 20, 1, slop_theme_dark.border_hi);
    slop_text(&slop_font_title, x + pad, y + 46, "Applications", slop_theme_dark.text);
    for (int i = 0; i < NAPPS; i++) {
        int gx = x + pad + (i % gw) * cell;
        int gy = y + pad + 24 + (i / gw) * (cell + 10);
        int hot = ui.mx >= gx && ui.mx < gx + cell - 20 && ui.my >= gy && ui.my < gy + cell - 20;
        if (hot) slop_fill_round(gx, gy, cell - 20, cell - 20, 14, slop_theme_dark.surface_hi);
        icon_draw(APPS[i].icon, gx + (cell - 20) / 2, gy + 44, 64, 0);
        int nw = slop_text_w(&slop_font_bold, APPS[i].name);
        slop_text(&slop_font_bold, gx + (cell - 20) / 2 - nw / 2, gy + 96, APPS[i].name,
                  slop_theme_dark.text);
        if (APPS[i].third_party) {
            int cw2 = slop_text_w(&slop_font_small, "compatibility");
            slop_text(&slop_font_small, gx + (cell - 20) / 2 - cw2 / 2, gy + 114,
                      "compatibility", slop_theme_dark.text_mute);
        }
        if (hot && ui.released) { *open = 0; launch(i); }
    }
}


/* ================================================================ toasts */
typedef struct { char title[64], body[96]; slop_color accent; int ttl; } toast_t;
static toast_t toasts[4];
static int ntoasts = 0;

static void toast(const char *title, const char *body, slop_color accent) {
    if (ntoasts >= 4) {
        memmove(&toasts[0], &toasts[1], sizeof(toast_t) * 3);
        ntoasts = 3;
    }
    toast_t *t = &toasts[ntoasts++];
    snprintf(t->title, sizeof(t->title), "%s", title);
    snprintf(t->body, sizeof(t->body), "%s", body);
    t->accent = accent;
    t->ttl = 3200;
}

static void toasts_draw(void) {
    int y = 48;
    for (int i = 0; i < ntoasts; i++) {
        toast_t *t = &toasts[i];
        int w = 300, h = 60;
        int x = slop.w - w - 20;
        slop_fill_round(x, y, w, h, 12, slop_theme_dark.surface);
        slop_round_outline(x, y, w, h, 12, 1, slop_theme_dark.border_hi);
        slop_fill_round(x, y, 5, h, 2, t->accent);
        slop_text(&slop_font_bold, x + 16, y + 26, t->title, slop_theme_dark.text);
        slop_text(&slop_font_small, x + 16, y + 44, t->body, slop_theme_dark.text_dim);
        y += h + 10;
    }
}

/* ================================================================ desktop */
static void draw_desktop(void) {
    slop_vgradient(0, 0, slop.w, slop.h, SLOP_RGB(0x0f, 0x12, 0x1d), SLOP_RGB(0x08, 0x09, 0x10));
    /* subtle dot grid for depth */
    for (int y = 60; y < slop.h - 120; y += 40)
        for (int x = 40; x < slop.w - 40; x += 40)
            slop_blend(x, y, SLOP_RGB(0x30, 0x36, 0x4a), 40);

    /* wallpaper wordmark */
    const char *w = "SlopOS";
    int ww = slop_text_w(&slop_font_title, w);
    slop_text(&slop_font_title, slop.w / 2 - ww / 2, 90, w, SLOP_RGB(0x1c, 0x22, 0x33));
    const char *s = "native desktop, zero bloat";
    int sw = slop_text_w(&slop_font_small, s);
    slop_text(&slop_font_small, slop.w / 2 - sw / 2, 112, s, SLOP_RGB(0x18, 0x1d, 0x2b));
}

int main(void) {
    if (slop_init() != 0) {
        fprintf(stderr, "slop-shell: cannot open framebuffer\n");
        return 1;
    }
    int menu_open = 0, hover = -1, last_tick = 0;
    int launched_once = 0;
    slop_flush_events();

    toast("Welcome to SlopOS", "Press 1-8 to launch apps, M for the launcher",
          slop_theme_dark.accent);

    for (;;) {
        slop_begin_frame();
        ui.quit = 0;   /* the session never exits on Esc */

        int now = slop_ticks_ms();
        if (last_tick && now - last_tick >= 100) {
            for (int i = 0; i < ntoasts; i++) toasts[i].ttl -= (now - last_tick);
            last_tick = now;
            while (ntoasts > 0 && toasts[0].ttl <= 0) {
                memmove(&toasts[0], &toasts[1], sizeof(toast_t) * (ntoasts - 1));
                ntoasts--;
            }
        } else if (!last_tick) last_tick = now;

        draw_desktop();
        if (menu_open) launcher_draw(&menu_open);
        dock_draw(&hover);
        panel_draw(&menu_open, NULL);
        toasts_draw();

        if (hover >= 0 && ui.released) launch(hover);

        /* keyboard: 1-8 launch dock apps, M toggles the launcher, Esc closes */
        for (int i = 0; i < ui.ntext; i++) {
            char ch = ui.text[i];
            if (ch >= '1' && ch <= '8') {
                int idx = ch - '1';
                if (idx < NAPPS) launch(idx);
            } else if (ch == 'm' || ch == 'M') {
                menu_open = !menu_open;
            }
        }
        if (ui.key == SLOP_KEY_ESC && menu_open) menu_open = 0;

        slop_end_frame();
        if (!launched_once) launched_once = 1;
        usleep(4000);
    }
    slop_shutdown();
    return 0;
}
