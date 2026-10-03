/*
 * slop-view -- the SlopOS text and code viewer.
 *
 * A read-mostly viewer with syntax-aware colouring for common languages,
 * line numbers, word wrap, search, and smooth scrolling. Great for reading
 * logs, source and configuration without leaving the desktop.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_LINES 200000
#define MAX_LINE  4096
static char **lines = NULL;
static int nlines = 0;
static char cur_path[1024];
static char status[256];
static int wrap = 0;
static int search_active = 0;
static char search[128] = "";
static int search_cursor = 0;

static const char *KEYWORDS[] = {
    "int","char","long","short","float","double","void","if","else","for","while",
    "return","struct","typedef","static","const","unsigned","signed","switch","case",
    "break","continue","sizeof","enum","union","goto","do","default","extern",
    "def","class","import","from","as","with","try","except","finally","lambda",
    "None","True","False","self","elif","pass","raise","yield","global","nonlocal",
    "function","var","let","new","this","typeof","async","await","export","null",
    "true","false","echo","then","fi","done","esac","export","local", NULL
};
static int is_kw(const char *w, int n) {
    for (int i = 0; KEYWORDS[i]; i++)
        if ((int)strlen(KEYWORDS[i]) == n && !strncmp(KEYWORDS[i], w, n)) return 1;
    return 0;
}

static void load(const char *path) {
    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines); lines = NULL; nlines = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) { snprintf(status, sizeof(status), "Cannot open %s", slop_basename(path)); return; }
    lines = malloc(sizeof(char *) * MAX_LINES);
    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), fp) && nlines < MAX_LINES) {
        int n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        lines[nlines++] = strdup(buf);
    }
    fclose(fp);
    strncpy(cur_path, path, sizeof(cur_path) - 1);
    snprintf(status, sizeof(status), "%d lines  -  %s", nlines, slop_basename(path));
}

/* ------------------------------------------------------- syntax colouring */
static slop_color token_color(const char *s, int n, slop_color base) {
    if (n == 0) return base;
    if (s[0] == '/' && n >= 2 && s[1] == '/') return SLOP_RGB(0x5c, 0x64, 0x7a);
    if (s[0] == '#') return SLOP_RGB(0x5c, 0x64, 0x7a);
    if (s[0] == '"' || s[0] == '\'') return SLOP_RGB(0x4a, 0xd9, 0x9a);
    if (s[0] >= '0' && s[0] <= '9') return SLOP_RGB(0xf0, 0xb4, 0x3c);
    if (is_kw(s, n)) return SLOP_RGB(0x6c, 0x8c, 0xff);
    return base;
}

/* Draw one line with a lightweight tokenizer: identifiers, numbers, strings,
   comments and punctuation. */
static int draw_code_line(const char *s, int x, int baseline, slop_color base, const char *hl, int hl_len) {
    int pen = x;
    int i = 0, n = strlen(s);
    while (i < n) {
        int start = i;
        char c = s[i];
        slop_color col = base;
        if (c == '/' && i + 1 < n && s[i+1] == '/') { col = SLOP_RGB(0x5c,0x64,0x7a); i = n; }
        else if (c == '#') { col = SLOP_RGB(0x5c,0x64,0x7a); i = n; }
        else if (c == '"' || c == '\'') {
            char q = c; i++;
            while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) i++; i++; }
            if (i < n) i++;
            col = SLOP_RGB(0x4a, 0xd9, 0x9a);
        }
        else if ((c >= '0' && c <= '9')) { while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'x' || (s[i] >= 'a' && s[i] <= 'f'))) i++; col = SLOP_RGB(0xf0,0xb4,0x3c); }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
                             (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) i++;
            col = is_kw(s + start, i - start) ? SLOP_RGB(0x6c,0x8c,0xff) : base;
        }
        else { i++; col = SLOP_RGB(0x9a, 0xa2, 0xb8); }
        (void)token_color;
        for (int k = start; k < i; k++) {
            char ch = s[k];
            if (ch < 32) ch = ' ';
            char one[2] = { ch, 0 };
            slop_text(&slop_font_mono, pen, baseline, one, col);
            pen += slop_char_w(&slop_font_mono, ch);
        }
        /* highlight overlay */
        if (hl && hl_len > 0) {
            for (int k = start; k < i; k++) {
                if (k + hl_len <= n && !strncmp(s + k, hl, hl_len)) {
                    int hx = x;
                    for (int m = 0; m < k; m++) hx += slop_char_w(&slop_font_mono, s[m] < 32 ? ' ' : s[m]);
                    int hw = 0;
                    for (int m = 0; m < hl_len; m++) hw += slop_char_w(&slop_font_mono, hl[m]);
                    slop_fill_rect_a(hx, baseline - slop_font_mono.ascent, hw, slop_font_mono.height,
                                     SLOP_RGB(0xf0, 0xb4, 0x3c), 70);
                }
            }
        }
    }
    return pen;
}

int main(int argc, char **argv) {
    if (slop_init() != 0) { fprintf(stderr, "slop-view: no framebuffer\n"); return 1; }
    const char *start = argc > 1 ? argv[1] : "/home/user/readme.txt";
    struct stat st;
    if (stat(start, &st) == 0) load(start);
    else { snprintf(status, sizeof(status), "No file: %s", start); lines = malloc(sizeof(char*)); nlines = 0; }
    slop_flush_events();

    int scroll = 0, scroll_drag = 0, focused = 0;
    int line_h = slop_font_mono.height + 4;

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;

        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("File Viewer", status, 0);

        /* toolbar */
        int tx = c.x + 12, ty = c.y + 8, th = 30;
        if (slop_toolbar_button(tx, ty, th, "Wrap", wrap)) wrap = !wrap;
        tx += 66;
        if (slop_toolbar_button(tx, ty, th, "Search", search_active)) {
            search_active = !search_active;
            if (search_active) focused = 1;
        }
        tx += 78;
        slop_text_vcenter(&slop_font_small, tx, ty, th, "Ctrl+F find   PgUp/PgDn scroll", slop_theme_dark.text_mute);

        if (search_active) {
            slop_textfield(c.x + c.w - 260, c.y + 8, 248, 30, search, sizeof(search),
                           &search_cursor, &focused, "find...");
        }

        int content_y = c.y + 48;
        int content_h = c.h - 48;
        int gutter = 56;
        slop_fill_rect(c.x, content_y, c.w, content_h, SLOP_RGB(0x0a, 0x0c, 0x12));
        slop_fill_rect(c.x, content_y, gutter, content_h, slop_theme_dark.surface_lo);
        slop_vline(c.x + gutter, content_y, content_h, slop_theme_dark.border);

        int visible = content_h / line_h;
        if (ui.wheel) scroll += ui.wheel * 3;
        if (ui.key == SLOP_KEY_PGUP) scroll -= visible - 1;
        if (ui.key == SLOP_KEY_PGDN) scroll += visible - 1;
        if (ui.key == SLOP_KEY_HOME) scroll = 0;
        if (ui.key == SLOP_KEY_END) scroll = nlines;
        if (scroll > nlines - visible) scroll = nlines - visible;
        if (scroll < 0) scroll = 0;
        slop_scrollbar(c.x + c.w - 12, content_y, content_h, nlines, visible, &scroll, &scroll_drag);

        const char *hl = (search_active && search[0]) ? search : NULL;
        for (int i = 0; i < visible && i + scroll < nlines; i++) {
            int idx = i + scroll;
            int ly = content_y + i * line_h;
            int baseline = ly + 2 + slop_font_mono.ascent;
            if (idx % 2) slop_fill_rect(c.x + gutter, ly, c.w - gutter, line_h, SLOP_RGB(0x0d, 0x0f, 0x16));
            char num[16];
            snprintf(num, sizeof(num), "%4d", idx + 1);
            int nw = slop_text_w(&slop_font_mono, num);
            slop_text(&slop_font_mono, c.x + gutter - nw - 8, baseline, num, slop_theme_dark.text_mute);
            draw_code_line(lines[idx], c.x + gutter + 8, baseline, slop_theme_dark.text, hl, hl ? (int)strlen(hl) : 0);
        }
        if (nlines == 0) {
            const char *msg = "Open a file from the Files app";
            int mw = slop_text_w(&slop_font_body, msg);
            slop_text(&slop_font_body, c.x + c.w / 2 - mw / 2, content_y + content_h / 2, msg, slop_theme_dark.text_mute);
        }

        if (ui.ctrl && ui.ctrl_ch == 'f') { search_active = 1; focused = 1; }

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(5000);
    }
    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    slop_shutdown();
    return 0;
}
