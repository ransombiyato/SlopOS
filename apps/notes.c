/*
 * slop-notes -- a tiny notes app that saves plain text files.
 *
 * Notes live in /home/user/Documents/Notes and are listed down the left. Text
 * editing supports the usual keys, and a character counter keeps you honest.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define NOTES_DIR "/home/user/Documents/Notes"
#define MAX_TEXT  65536

static char text[MAX_TEXT] = "";
static int  cursor = 0;
static int  focused = 1;
static char files[64][128];
static int  nfiles = 0;
static int  cur_file = -1;
static char title[128] = "untitled.txt";
static char status[128] = "ready";

static void scan_files(void) {
    nfiles = 0;
    DIR *d = opendir(NOTES_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && nfiles < 64) {
        if (de->d_name[0] == '.') continue;
        snprintf(files[nfiles], sizeof(files[0]), "%.120s", de->d_name);
        nfiles++;
    }
    closedir(d);
}

static void note_path(char *out, int cap, const char *name) {
    snprintf(out, cap, NOTES_DIR "/%s", name);
}

static void open_file(const char *name) {
    char path[256]; note_path(path, sizeof(path), name);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(status, sizeof(status), "cannot open %s", name); return; }
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = 0;
    fclose(f);
    cursor = (int)n;
    snprintf(title, sizeof(title), "%.120s", name);
    snprintf(status, sizeof(status), "%d bytes", (int)n);
    for (int i = 0; i < nfiles; i++) if (!strcmp(files[i], name)) cur_file = i;
}

static void save_file(void) {
    mkdir("/home/user/Documents", 0755);
    mkdir(NOTES_DIR, 0755);
    char path[256]; note_path(path, sizeof(path), title);
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(status, sizeof(status), "cannot save"); return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    snprintf(status, sizeof(status), "saved %.100s", title);
    scan_files();
}

static void new_file(void) {
    text[0] = 0; cursor = 0;
    snprintf(title, sizeof(title), "note-%d.txt", (int)(slop_ticks_ms() % 100000));
    snprintf(status, sizeof(status), "new note");
    cur_file = -1;
}

int main(void) {
    if (slop_app_start("Notes", SLOP_ICON_NOTES) != 0) return 1;
    mkdir("/home/user/Documents", 0755);
    mkdir(NOTES_DIR, 0755);
    scan_files();
    if (nfiles > 0) open_file(files[0]); else new_file();
    slop_flush_events();

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;
        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("Notes", status, 0);

        /* file list */
        int sbw = 190;
        slop_fill_rect(c.x, c.y, sbw, c.h, slop_theme_dark.surface_lo);
        slop_vline(c.x + sbw - 1, c.y, c.h, slop_theme_dark.border);
        slop_text(&slop_font_small, c.x + 16, c.y + 26, "NOTES", slop_theme_dark.text_mute);
        int ny = c.y + 42;
        for (int i = 0; i < nfiles; i++) {
            int hot = ui.mx >= c.x && ui.mx < c.x + sbw - 1 && ui.my >= ny && ui.my < ny + 30;
            if (i == cur_file) slop_fill_round(c.x + 8, ny, sbw - 20, 30, 7, slop_theme_dark.accent_lo);
            else if (hot) slop_fill_round(c.x + 8, ny, sbw - 20, 30, 7, slop_theme_dark.surface_hi);
            slop_text_vcenter(&slop_font_body, c.x + 18, ny, 30, files[i],
                              i == cur_file ? slop_theme_dark.text : slop_theme_dark.text_dim);
            if (hot && ui.released) open_file(files[i]);
            ny += 32;
        }
        /* new-note button */
        slop_button nb = { c.x + 12, c.y + c.h - 50, sbw - 28, 36, "New note", 1, 1, 0, 0 };
        if (slop_button_draw(&nb)) new_file();

        /* editor */
        int ex = c.x + sbw + 16, ew = c.w - sbw - 32;
        int ey = c.y + 16, eh = c.h - 76;
        slop_fill_round(ex, ey, ew, eh, 10, slop_theme_dark.surface_lo);
        slop_round_outline(ex, ey, ew, eh, 10, 1, slop_theme_dark.border);

        /* title field */
        slop_textfield(ex, ey + 6, ew, 34, title, sizeof(title), &cursor, &focused, "file name");

        int ty = ey + 52;
        slop_text(&slop_font_mono, ex + 14, ty, text, slop_theme_dark.text);
        /* caret */
        if (focused) {
            int before = cursor;
            int lines = 0;
            for (int i = 0; i < before && i < (int)strlen(text); i++) if (text[i] == '\n') lines++;
            int col = 0;
            for (int i = before - 1; i >= 0 && text[i] != '\n'; i--) col++;
            slop_fill_rect(ex + 14 + col * 8, ty + lines * 16 - 12, 1, 16, slop_theme_dark.accent_hi);
        }

        /* toolbar */
        slop_button sv = { c.x + c.w - 220, c.y + c.h - 50, 90, 36, "Save", 1, 1, 0, 0 };
        if (slop_button_draw(&sv)) save_file();
        char cc[48]; snprintf(cc, sizeof(cc), "%d chars", (int)strlen(text));
        slop_text_vcenter(&slop_font_small, c.x + 24, c.y + c.h - 50, 36, cc, slop_theme_dark.text_mute);

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(5000);
    }
    slop_shutdown();
    return 0;
}
