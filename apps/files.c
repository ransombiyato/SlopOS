/*
 * slop-files -- the SlopOS file manager.
 *
 * A real directory browser: sidebar places, breadcrumb, sortable list,
 * keyboard navigation, and double-click-to-open that hands files to the right
 * native viewer. Directories are entered in place.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef struct {
    char name[256];
    int  is_dir;
    long size;
    long mtime;
    const char *ext;
} entry_t;

#define MAX_ENTRIES 4096
static entry_t ents[MAX_ENTRIES];
static int nents = 0;
static char cwd[1024];
static int sel = 0, scroll = 0;
static int scroll_drag = 0;
static char status[256];

static const struct { const char *label; const char *path; } PLACES[] = {
    { "Home",      "/home/user" },
    { "Desktop",   "/home/user/Desktop" },
    { "Documents", "/home/user/Documents" },
    { "Pictures",  "/home/user/Pictures" },
    { "Downloads", "/home/user/Downloads" },
    { "Videos",    "/home/user/Videos" },
    { "Root",      "/" },
    { "Tmp",       "/tmp" },
};
static const int NPLACES = (int)(sizeof(PLACES) / sizeof(PLACES[0]));

static int cmp_entry(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void load_dir(const char *path) {
    char real[1024];
    if (!realpath(path, real)) {
        snprintf(status, sizeof(status), "Cannot open %s: %s", path, strerror(errno));
        return;
    }
    strncpy(cwd, real, sizeof(cwd) - 1);
    nents = 0;
    DIR *d = opendir(cwd);
    if (!d) { snprintf(status, sizeof(status), "Cannot read %s", cwd); return; }
    struct dirent *de;
    while ((de = readdir(d)) && nents < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".")) continue;
        entry_t *e = &ents[nents];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", cwd, de->d_name);
        struct stat st;
        if (lstat(full, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size = (long)st.st_size;
            e->mtime = (long)st.st_mtime;
        } else { e->is_dir = 0; e->size = 0; e->mtime = 0; }
        e->ext = slop_ext(e->name);
        nents++;
    }
    closedir(d);
    qsort(ents, nents, sizeof(entry_t), cmp_entry);
    sel = 0; scroll = 0;
    long total = 0; int dirs = 0;
    for (int i = 0; i < nents; i++) { if (ents[i].is_dir) dirs++; else total += ents[i].size; }
    char hs[32]; slop_human_size(total, hs, sizeof(hs));
    snprintf(status, sizeof(status), "%d items  -  %d folders  -  %s", nents, dirs, hs);
}

static void go_parent(void) {
    if (!strcmp(cwd, "/")) return;
    char up[1024];
    strncpy(up, cwd, sizeof(up) - 1);
    char *s = strrchr(up, '/');
    if (s && s != up) *s = 0; else strcpy(up, "/");
    load_dir(up);
}

/* ---------------------------------------------------------------- icons */
static void draw_file_icon(int x, int y, int s, const entry_t *e) {
    if (e->is_dir) {
        slop_fill_round(x, y + 4, s, s - 6, 3, SLOP_RGB(0xf0, 0xb4, 0x3c));
        slop_fill_round(x, y, s / 2, 7, 2, SLOP_RGB(0xf7, 0xcd, 0x6a));
        return;
    }
    slop_color c = SLOP_RGB(0x8a, 0x93, 0xa8);
    const char *x4 = e->ext;
    if (!strcasecmp(x4, "png") || !strcasecmp(x4, "jpg") || !strcasecmp(x4, "jpeg") ||
        !strcasecmp(x4, "gif") || !strcasecmp(x4, "bmp")) c = SLOP_RGB(0x4a, 0xd9, 0x9a);
    else if (!strcasecmp(x4, "c") || !strcasecmp(x4, "h") || !strcasecmp(x4, "py") ||
             !strcasecmp(x4, "sh")) c = SLOP_RGB(0x6c, 0x8c, 0xff);
    else if (!strcasecmp(x4, "txt") || !strcasecmp(x4, "md")) c = SLOP_RGB(0xd0, 0xd4, 0xde);
    else if (!strcasecmp(x4, "mp4") || !strcasecmp(x4, "mkv")) c = SLOP_RGB(0xc0, 0x7a, 0xf0);
    int fold = 5;
    slop_fill_round(x, y, s - 4, s - 2, 3, c);
    slop_fill_round(x + s - 4 - 9, y, 9, 9, 3, slop_theme_dark.surface);
    (void)fold;
}

/* -------------------------------------------------------------- opening */
static void launch_viewer(const char *path) {
    const char *ext = slop_ext(path);
    const char *prog = "slop-view";
    if (!strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg") ||
        !strcasecmp(ext, "gif") || !strcasecmp(ext, "bmp"))
        prog = "slop-image";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/usr/bin/slop-open", "slop-open", prog, path, (char *)NULL);
        execl("/bin/slop-open", "slop-open", prog, path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

static void activate(int i) {
    if (i < 0 || i >= nents) return;
    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", cwd, ents[i].name);
    if (ents[i].is_dir) load_dir(full);
    else launch_viewer(full);
}

/* ----------------------------------------------------------------- main */
static void draw_sidebar(int x, int y, int w, int h, const char *places_sel) {
    slop_fill_rect(x, y, w, h, slop_theme_dark.surface_lo);
    slop_vline(x + w - 1, y, h, slop_theme_dark.border);
    slop_text(&slop_font_small, x + 16, y + 26, "PLACES", slop_theme_dark.text_mute);
    int py = y + 42;
    for (int i = 0; i < NPLACES; i++) {
        int hot = ui.mx >= x && ui.mx < x + w - 1 && ui.my >= py && ui.my < py + 30;
        int active = places_sel && !strcmp(places_sel, PLACES[i].path);
        if (hot || active) slop_fill_round(x + 8, py, w - 20, 30, 7,
                                           active ? slop_theme_dark.accent_lo : slop_theme_dark.surface_hi);
        slop_fill_round(x + 16, py + 9, 12, 12, 3, SLOP_RGB(0x6c, 0x8c, 0xff));
        slop_text_vcenter(&slop_font_body, x + 38, py, 30, PLACES[i].label,
                          (hot || active) ? slop_theme_dark.text : slop_theme_dark.text_dim);
        if (hot && ui.released) load_dir(PLACES[i].path);
        py += 32;
    }
}

static void draw_crumbs(int x, int y, int w, int h) {
    slop_fill_round(x, y, w, h, 8, slop_theme_dark.surface_lo);
    slop_round_outline(x, y, w, h, 8, 1, slop_theme_dark.border);
    int px = x + 12;
    slop_text_vcenter(&slop_font_body, px, y, h, "/", slop_theme_dark.text_mute);
    px += slop_text_w(&slop_font_body, "/") + 2;
    char tmp[1024];
    strncpy(tmp, cwd, sizeof(tmp) - 1);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        int tw = slop_text_w(&slop_font_body, tok);
        int hot = ui.mx >= px && ui.mx < px + tw && ui.my >= y && ui.my < y + h;
        slop_text_vcenter(&slop_font_body, px, y, h, tok, hot ? slop_theme_dark.accent_hi : slop_theme_dark.text);
        px += tw + 4;
        slop_text_vcenter(&slop_font_body, px, y, h, "/", slop_theme_dark.text_mute);
        px += slop_text_w(&slop_font_body, "/") + 4;
    }
}

int main(void) {
    if (slop_init() != 0) { fprintf(stderr, "slop-files: no framebuffer\n"); return 1; }
    load_dir("/home/user");
    slop_flush_events();
    int last_click = 0, last_click_i = -1;

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;

        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("Files", status, 0);

        int sbw = 180;
        int row_h = 34;
        int top = c.y + 52;
        int list_h = c.h - 52 - 40;

        draw_sidebar(c.x, c.y, sbw, c.h, cwd);
        draw_crumbs(c.x + sbw + 12, c.y + 10, c.w - sbw - 24, 32);

        /* list */
        int lx = c.x + sbw + 12;
        int lw = c.w - sbw - 24;
        int visible = list_h / row_h;
        for (int i = 0; i < visible && i + scroll < nents; i++) {
            int idx = i + scroll;
            int ry = top + i * row_h;
            int hot = ui.mx >= lx && ui.mx < lx + lw && ui.my >= ry && ui.my < ry + row_h;
            entry_t *e = &ents[idx];
            if (idx == sel) slop_fill_round(lx, ry, lw, row_h, 7, slop_theme_dark.accent_lo);
            else if (hot) slop_fill_round(lx, ry, lw, row_h, 7, slop_theme_dark.surface_hi);
            draw_file_icon(lx + 10, ry + 7, 22, e);
            slop_color tc = idx == sel ? SLOP_RGB(255,255,255) : slop_theme_dark.text;
            slop_text_vcenter(&slop_font_body, lx + 44, ry, row_h, e->name, tc);
            if (!e->is_dir) {
                char hs[32]; slop_human_size(e->size, hs, sizeof(hs));
                int sw = slop_text_w(&slop_font_small, hs);
                slop_text_vcenter(&slop_font_small, lx + lw - sw - 16, ry, row_h, hs,
                                  idx == sel ? SLOP_RGB(220,226,255) : slop_theme_dark.text_mute);
            } else {
                const char *d = "folder";
                int sw = slop_text_w(&slop_font_small, d);
                slop_text_vcenter(&slop_font_small, lx + lw - sw - 16, ry, row_h, d,
                                  idx == sel ? SLOP_RGB(220,226,255) : slop_theme_dark.text_mute);
            }
            if (hot && ui.pressed) {
                sel = idx;
                int now = slop_ticks_ms();
                if (last_click_i == idx && now - last_click < 450) activate(idx);
                last_click = now; last_click_i = idx;
            }
        }
        slop_scrollbar(lx + lw - 2, top, list_h, nents, visible, &scroll, &scroll_drag);
        if (ui.wheel) { scroll += ui.wheel * 2; if (scroll < 0) scroll = 0; }
        if (scroll > nents - visible && nents > visible) scroll = nents - visible;
        if (scroll < 0) scroll = 0;

        /* status bar */
        int sy = c.y + c.h - 30;
        slop_hline(c.x, sy - 6, c.w, slop_theme_dark.border);
        slop_text_vcenter(&slop_font_small, c.x + 12, sy - 6, 30, cwd, slop_theme_dark.text_dim);
        int nw = slop_text_w(&slop_font_small, status);
        slop_text_vcenter(&slop_font_small, c.x + c.w - nw - 12, sy - 6, 30, status, slop_theme_dark.text_mute);

        /* keyboard */
        if (ui.key == SLOP_KEY_DOWN && sel + 1 < nents) sel++;
        if (ui.key == SLOP_KEY_UP && sel > 0) sel--;
        if (ui.key == SLOP_KEY_HOME) sel = 0;
        if (ui.key == SLOP_KEY_END) sel = nents - 1;
        if (ui.key == SLOP_KEY_BACKSPACE || ui.key == SLOP_KEY_LEFT) go_parent();
        if (ui.key == SLOP_KEY_ENTER) activate(sel);
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + visible) scroll = sel - visible + 1;

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(5000);
    }
    slop_shutdown();
    return 0;
}
