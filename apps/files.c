/*
 * slop-files -- the SlopOS file manager.
 *
 * A real directory browser: sidebar places, breadcrumb navigation, grid and
 * list views, search, keyboard navigation, and a context menu that can create
 * folders, rename, delete and show properties. Double-clicking hands files to
 * the matching native viewer.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef struct {
    char name[256];
    int  is_dir;
    long size;
    long mtime;
    const char *ext;
} entry_t;

#define MAX_ENTRIES 8192
static entry_t ents[MAX_ENTRIES];
static int nents = 0;
static char cwd[1024];
static int sel = 0, scroll = 0, scroll_drag = 0;
static char status[256];
static int grid_view = 0;
static char filter[64];
static int filter_focus = 0, filter_cursor = 0;

enum { DLG_NONE, DLG_RENAME, DLG_NEWFOLDER, DLG_DELETE, DLG_PROPS };
static int dlg = DLG_NONE;
static char dlg_buf[256];
static int dlg_cursor = 0;
static int ctx_x = 0, ctx_y = 0, ctx_open = 0, ctx_target = -1;

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
    char real[2048];
    if (!realpath(path, real)) {
        snprintf(status, sizeof(status), "Cannot open %s: %s", path, strerror(errno));
        return;
    }
    if (strlen(real) >= sizeof(cwd)) { snprintf(status, sizeof(status), "Path too long"); return; }
    memcpy(cwd, real, strlen(real) + 1);
    nents = 0;
    DIR *d = opendir(cwd);
    if (!d) { snprintf(status, sizeof(status), "Cannot read %.240s", cwd); return; }
    struct dirent *de;
    while ((de = readdir(d)) && nents < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".")) continue;
        entry_t *e = &ents[nents];
        if (snprintf(e->name, sizeof(e->name), "%s", de->d_name) >= (int)sizeof(e->name))
            continue;
        char full[4096];
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

static int match_filter(const entry_t *e) {
    if (!filter[0]) return 1;
    char lower[256], f[64];
    snprintf(lower, sizeof(lower), "%s", e->name);
    snprintf(f, sizeof(f), "%s", filter);
    for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = f; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(lower, f) != NULL;
}

static void go_parent(void) {
    if (!strcmp(cwd, "/")) return;
    char up[1024];
    snprintf(up, sizeof(up), "%s", cwd);
    char *s = strrchr(up, '/');
    if (s && s != up) *s = 0; else strcpy(up, "/");
    load_dir(up);
}

static void full_path(int i, char *out, int cap) {
    snprintf(out, cap, "%s/%s", cwd, ents[i].name);
}

static void do_delete(int i) {
    if (i < 0 || i >= nents) return;
    char full[4096]; full_path(i, full, sizeof(full));
    if (ents[i].is_dir) rmdir(full); else unlink(full);
    load_dir(cwd);
}
static void do_rename(int i, const char *newname) {
    if (i < 0 || i >= nents || !newname[0]) return;
    char a[4096], b[4096];
    full_path(i, a, sizeof(a));
    snprintf(b, sizeof(b), "%s/%s", cwd, newname);
    rename(a, b);
    load_dir(cwd);
}
static void do_newfolder(const char *name) {
    if (!name[0]) return;
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", cwd, name);
    mkdir(full, 0755);
    load_dir(cwd);
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
    slop_fill_round(x, y, s - 4, s - 2, 3, c);
    slop_fill_round(x + s - 4 - 9, y, 9, 9, 3, slop_theme_dark.surface);
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
    char full[4096];
    full_path(i, full, sizeof(full));
    if (ents[i].is_dir) load_dir(full);
    else launch_viewer(full);
}

/* --------------------------------------------------------------- chrome */
static void draw_sidebar(int x, int y, int w, int h, const char *places_sel) {
    slop_fill_rect(x, y, w, h, slop_theme_dark.surface_lo);
    slop_vline(x + w - 1, y, h, slop_theme_dark.border);
    slop_text(&slop_font_small, x + 16, y + 26, "PLACES", slop_theme_dark.text_mute);
    int py = y + 42;
    for (int i = 0; i < NPLACES; i++) {
        int hot = ui.mx >= x && ui.mx < x + w - 1 && ui.my >= py && ui.my < py + 30;
        int active = places_sel && !strcmp(places_sel, PLACES[i].path);
        if (hot || active)
            slop_fill_round(x + 8, py, w - 20, 30, 7,
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
    snprintf(tmp, sizeof(tmp), "%s", cwd);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        int tw = slop_text_w(&slop_font_body, tok);
        int hot = ui.mx >= px && ui.mx < px + tw && ui.my >= y && ui.my < y + h;
        slop_text_vcenter(&slop_font_body, px, y, h, tok,
                          hot ? slop_theme_dark.accent_hi : slop_theme_dark.text);
        px += tw + 4;
        slop_text_vcenter(&slop_font_body, px, y, h, "/", slop_theme_dark.text_mute);
        px += slop_text_w(&slop_font_body, "/") + 4;
    }
}

static void draw_toolbar(int x, int y, int w, int h) {
    int bx = x;
    if (slop_icon_button(bx, y, h, "<", 0, "Back")) go_parent();
    bx += h + 6;
    if (slop_icon_button(bx, y, h, "Grid", grid_view, "Grid / list")) grid_view = !grid_view;
    bx += h + 6;
    if (slop_icon_button(bx, y, h, "+", 0, "New folder")) {
        dlg = DLG_NEWFOLDER; dlg_buf[0] = 0; dlg_cursor = 0;
    }
    int sw = 200;
    int sx = x + w - sw;
    slop_search_box(sx, y, sw, h, filter, sizeof(filter), &filter_cursor, &filter_focus);
}

static void draw_context_menu(void) {
    static const char *items[] = { "Open", "Rename", "Delete", "Properties" };
    int chosen = -1;
    slop_ctx_menu(ctx_x, ctx_y, items, 4, &chosen);
    if (chosen >= 0) {
        ctx_open = 0;
        int i = ctx_target;
        if (chosen == 0) activate(i);
        else if (chosen == 1) {
            dlg = DLG_RENAME;
            snprintf(dlg_buf, sizeof(dlg_buf), "%s", (i >= 0 && i < nents) ? ents[i].name : "");
            dlg_cursor = (int)strlen(dlg_buf);
        } else if (chosen == 2) dlg = DLG_DELETE;
        else if (chosen == 3) dlg = DLG_PROPS;
    } else if (ui.released) {
        ctx_open = 0;
    }
}

static void draw_dialog(void) {
    if (dlg == DLG_NONE) return;
    int w = 420, h = (dlg == DLG_PROPS) ? 320 : 190;
    int x = (slop.w - w) / 2, y = (slop.h - h) / 2;
    slop_fill_rect_a(0, 0, slop.w, slop.h, 0, 150);
    slop_shadow(x, y, w, h, 20, 140);
    slop_fill_round(x, y, w, h, 16, slop_theme_dark.surface);
    slop_round_outline(x, y, w, h, 16, 1, slop_theme_dark.border_hi);

    const char *title = dlg == DLG_RENAME ? "Rename" :
                        dlg == DLG_NEWFOLDER ? "New Folder" :
                        dlg == DLG_DELETE ? "Delete" : "Properties";
    slop_text(&slop_font_title, x + 24, y + 40, title, slop_theme_dark.text);

    if (dlg == DLG_PROPS) {
        int i = ctx_target;
        if (i >= 0 && i < nents) {
            char full[4096]; full_path(i, full, sizeof(full));
            struct stat st; stat(full, &st);
            char hs[32]; slop_human_size(ents[i].size, hs, sizeof(hs));
            char when[64];
            struct tm tm; localtime_r(&st.st_mtime, &tm);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
            char kind[64];
            snprintf(kind, sizeof(kind), ents[i].is_dir ? "Folder" : "File (%s)", ents[i].ext);
            slop_stat_row(x + 24, y + 70, w - 48, "Name", ents[i].name, slop_theme_dark.text);
            slop_stat_row(x + 24, y + 100, w - 48, "Type", kind, slop_theme_dark.text_dim);
            slop_stat_row(x + 24, y + 130, w - 48, "Size", ents[i].is_dir ? "-" : hs,
                          slop_theme_dark.text_dim);
            slop_stat_row(x + 24, y + 160, w - 48, "Modified", when, slop_theme_dark.text_dim);
            char perms[16]; snprintf(perms, sizeof(perms), "%04o", st.st_mode & 07777);
            slop_stat_row(x + 24, y + 190, w - 48, "Permissions", perms, slop_theme_dark.text_dim);
            slop_stat_row(x + 24, y + 220, w - 48, "Path", full, slop_theme_dark.text_mute);
        }
        if (slop_icon_button(x + w - 46, y + 16, 30, "X", 0, "Close")) dlg = DLG_NONE;
        return;
    }

    if (dlg == DLG_DELETE) {
        char msg[320];
        snprintf(msg, sizeof(msg), "Delete \"%s\"?",
                 (ctx_target >= 0 && ctx_target < nents) ? ents[ctx_target].name : "item");
        slop_text(&slop_font_body, x + 24, y + 74, msg, slop_theme_dark.text_dim);
    } else {
        int tmpf = 0;
        slop_textfield(x + 24, y + 66, w - 48, 40, dlg_buf, sizeof(dlg_buf),
                       &dlg_cursor, &tmpf, dlg == DLG_NEWFOLDER ? "Folder name" : "New name");
    }

    slop_button cancel = { x + w - 200, y + h - 56, 84, 38, "Cancel", 0, 1, 0, 0 };
    slop_button ok     = { x + w - 106, y + h - 56, 84, 38, "OK", 1, 1, 0, 0 };
    if (slop_button_draw(&cancel)) dlg = DLG_NONE;
    if (slop_button_draw(&ok)) {
        if (dlg == DLG_RENAME) do_rename(ctx_target, dlg_buf);
        else if (dlg == DLG_NEWFOLDER) do_newfolder(dlg_buf);
        else if (dlg == DLG_DELETE) do_delete(ctx_target);
        dlg = DLG_NONE;
    }
    if (ui.key == SLOP_KEY_ESC) dlg = DLG_NONE;
}

/* ----------------------------------------------------------------- main */
int main(void) {
    if (slop_app_start("Files", SLOP_ICON_FILES) != 0) {
        fprintf(stderr, "slop-files: no display\n"); return 1;
    }
    load_dir("/home/user");
    slop_flush_events();
    int last_click = 0, last_click_i = -1;

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;

        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("Files", status, 0);

        int sbw = 180;
        draw_sidebar(c.x, c.y, sbw, c.h, cwd);

        int cx = c.x + sbw + 12;
        int cw = c.w - sbw - 24;
        draw_toolbar(cx, c.y + 10, cw, 32);
        draw_crumbs(cx, c.y + 50, cw, 32);

        int top = c.y + 92;
        int list_h = c.h - 92 - 40;
        int lx = cx, lw = cw;

        if (dlg == DLG_NONE && ctx_open == 0) {
            if (grid_view) {
                int cell = 108;
                int cols = lw / cell; if (cols < 1) cols = 1;
                int vis_rows = list_h / cell;
                int first = (scroll / cols) * cols;
                for (int k = 0; k < cols * (vis_rows + 1); k++) {
                    int idx = first + k;
                    if (idx >= nents) break;
                    entry_t *e = &ents[idx];
                    if (!match_filter(e)) continue;
                    int gx = lx + (k % cols) * cell + 6;
                    int gy = top + (k / cols) * cell;
                    if (gy > top + list_h) break;
                    int hot = ui.mx >= gx && ui.mx < gx + cell - 12 &&
                              ui.my >= gy && ui.my < gy + cell - 12;
                    if (idx == sel) slop_fill_round(gx, gy, cell - 12, cell - 12, 10, slop_theme_dark.accent_lo);
                    else if (hot) slop_fill_round(gx, gy, cell - 12, cell - 12, 10, slop_theme_dark.surface_hi);
                    draw_file_icon(gx + (cell - 12) / 2 - 26, gy + 18, 52, e);
                    int nw2 = slop_text_w(&slop_font_small, e->name);
                    int lx2 = gx + (cell - 12) / 2 - (nw2 > cell - 20 ? (cell - 20) : nw2) / 2;
                    if (nw2 > cell - 20) {
                        int cn = 0;
                        while (e->name[cn] && slop_text_wn(&slop_font_small, e->name, cn + 1) < cell - 26) cn++;
                        slop_text_n(&slop_font_small, lx2, gy + cell - 26, e->name, cn,
                                    slop_theme_dark.text);
                    } else {
                        slop_text(&slop_font_small, lx2, gy + cell - 26, e->name,
                                  slop_theme_dark.text);
                    }
                    if (hot && ui.pressed) {
                        sel = idx;
                        int now = slop_ticks_ms();
                        if (last_click_i == idx && now - last_click < 450) activate(idx);
                        last_click = now; last_click_i = idx;
                    }
                    if (hot && ui.rpressed) {
                        sel = idx; ctx_target = idx; ctx_open = 1; ctx_x = ui.mx; ctx_y = ui.my;
                    }
                }
            } else {
                int row_h = 34;
                int visible = list_h / row_h;
                int drawn = 0;
                for (int i = 0; i < nents && drawn < visible; i++) {
                    entry_t *e = &ents[i];
                    if (!match_filter(e)) continue;
                    int ry = top + drawn * row_h;
                    int hot = ui.mx >= lx && ui.mx < lx + lw && ui.my >= ry && ui.my < ry + row_h;
                    if (i == sel) slop_fill_round(lx, ry, lw, row_h, 7, slop_theme_dark.accent_lo);
                    else if (hot) slop_fill_round(lx, ry, lw, row_h, 7, slop_theme_dark.surface_hi);
                    draw_file_icon(lx + 10, ry + 7, 22, e);
                    slop_color tc = i == sel ? SLOP_RGB(255,255,255) : slop_theme_dark.text;
                    slop_text_vcenter(&slop_font_body, lx + 44, ry, row_h, e->name, tc);
                    if (!e->is_dir) {
                        char hs[32]; slop_human_size(e->size, hs, sizeof(hs));
                        int sw = slop_text_w(&slop_font_small, hs);
                        slop_text_vcenter(&slop_font_small, lx + lw - sw - 16, ry, row_h, hs,
                                          i == sel ? SLOP_RGB(220,226,255) : slop_theme_dark.text_mute);
                    } else {
                        const char *d = "folder";
                        int sw = slop_text_w(&slop_font_small, d);
                        slop_text_vcenter(&slop_font_small, lx + lw - sw - 16, ry, row_h, d,
                                          i == sel ? SLOP_RGB(220,226,255) : slop_theme_dark.text_mute);
                    }
                    if (hot && ui.pressed) {
                        sel = i;
                        int now = slop_ticks_ms();
                        if (last_click_i == i && now - last_click < 450) activate(i);
                        last_click = now; last_click_i = i;
                    }
                    if (hot && ui.rpressed) {
                        sel = i; ctx_target = i; ctx_open = 1; ctx_x = ui.mx; ctx_y = ui.my;
                    }
                    drawn++;
                }
                slop_scrollbar(lx + lw - 2, top, list_h, nents, visible, &scroll, &scroll_drag);
            }
            if (ui.wheel) { scroll += ui.wheel * 2; if (scroll < 0) scroll = 0; }
            if (scroll < 0) scroll = 0;
            if (scroll >= nents && nents > 0) scroll = nents - 1;
        }

        int sy = c.y + c.h - 30;
        slop_hline(c.x, sy - 6, c.w, slop_theme_dark.border);
        slop_text_vcenter(&slop_font_small, c.x + 12, sy - 6, 30, cwd, slop_theme_dark.text_dim);
        int nw = slop_text_w(&slop_font_small, status);
        slop_text_vcenter(&slop_font_small, c.x + c.w - nw - 12, sy - 6, 30, status,
                          slop_theme_dark.text_mute);

        if (dlg == DLG_NONE && !filter_focus) {
            if (ui.key == SLOP_KEY_DOWN && sel + 1 < nents) sel++;
            if (ui.key == SLOP_KEY_UP && sel > 0) sel--;
            if (ui.key == SLOP_KEY_HOME) sel = 0;
            if (ui.key == SLOP_KEY_END) sel = nents - 1;
            if (ui.key == SLOP_KEY_BACKSPACE || ui.key == SLOP_KEY_LEFT) go_parent();
            if (ui.key == SLOP_KEY_ENTER) activate(sel);
            if (ui.key == SLOP_KEY_F2 && nents > 0) {
                dlg = DLG_RENAME; ctx_target = sel;
                snprintf(dlg_buf, sizeof(dlg_buf), "%s", ents[sel].name);
                dlg_cursor = (int)strlen(dlg_buf);
            }
            if (ui.key == SLOP_KEY_DELETE && nents > 0) { dlg = DLG_DELETE; ctx_target = sel; }
        }
        if (sel < scroll) scroll = sel;

        if (ctx_open) draw_context_menu();
        draw_dialog();

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(5000);
    }
    slop_shutdown();
    return 0;
}
