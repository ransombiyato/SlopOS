/*
 * slop-term -- the SlopOS terminal.
 *
 * A real terminal: it allocates a pty, runs /bin/sh on the slave side, and
 * emulates enough of a VT100/ANSI terminal (SGR colours, cursor movement,
 * erase, scrollback) to run interactive console programs normally.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <pty.h>
#include <termios.h>

/* --------------------------------------------------------------- palette */
static slop_color pal[256];
static void init_palette(void) {
    static const slop_color base[16] = {
        SLOP_RGB(0x1a,0x1c,0x24), SLOP_RGB(0xf0,0x6a,0x6a), SLOP_RGB(0x4a,0xd9,0x9a),
        SLOP_RGB(0xf0,0xb4,0x3c), SLOP_RGB(0x6c,0x8c,0xff), SLOP_RGB(0xc0,0x7a,0xf0),
        SLOP_RGB(0x4a,0xc8,0xd9), SLOP_RGB(0xd0,0xd4,0xde), SLOP_RGB(0x5c,0x64,0x7a),
        SLOP_RGB(0xff,0x8a,0x8a), SLOP_RGB(0x7a,0xf0,0xb8), SLOP_RGB(0xff,0xd0,0x6a),
        SLOP_RGB(0x8a,0xa4,0xff), SLOP_RGB(0xe0,0x9a,0xff), SLOP_RGB(0x7a,0xe8,0xf0),
        SLOP_RGB(0xf4,0xf6,0xfa),
    };
    memcpy(pal, base, sizeof(base));
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                int v = 16 + 36 * r + 6 * g + b;
                pal[v] = SLOP_RGB(r ? 55 + r * 40 : 0, g ? 55 + g * 40 : 0, b ? 55 + b * 40 : 0);
            }
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        pal[232 + i] = SLOP_RGB(v, v, v);
    }
}

/* ------------------------------------------------------------------ cell */
typedef struct { char ch; uint8_t fg, bg, bold; } cell;

static int COLS = 100, ROWS = 30;
static cell *grid;
static int cur_x = 0, cur_y = 0;
static int cur_fg = 7, cur_bg = 0, cur_bold = 0;
static int cursor_visible = 1;
static int master_fd = -1;
static pid_t child = 0;

#define SB_MAX 4000
static cell *sb;
static int sb_count = 0, sb_head = 0;
static int view_off = 0;

static void clear_cell(cell *c) { c->ch = ' '; c->fg = 7; c->bg = 0; c->bold = 0; }

static void grid_scroll_up(void) {
    if (sb) {
        memcpy(&sb[(size_t)sb_head * COLS], grid, sizeof(cell) * COLS);
        sb_head = (sb_head + 1) % SB_MAX;
        if (sb_count < SB_MAX) sb_count++;
    }
    memmove(grid, grid + COLS, sizeof(cell) * (size_t)COLS * (ROWS - 1));
    for (int x = 0; x < COLS; x++) clear_cell(&grid[(size_t)(ROWS - 1) * COLS + x]);
}

static void term_reset(void) {
    for (int i = 0; i < ROWS * COLS; i++) { grid[i].ch = ' '; grid[i].fg = 7; grid[i].bg = 0; grid[i].bold = 0; }
    cur_x = cur_y = 0;
    cur_fg = 7; cur_bg = 0; cur_bold = 0;
}

static void put_ch(char ch) {
    if (ch == '\r') { cur_x = 0; return; }
    if (ch == '\n') { if (++cur_y >= ROWS) { cur_y = ROWS - 1; grid_scroll_up(); } return; }
    if (ch == '\b') { if (cur_x > 0) cur_x--; return; }
    if (ch == '\t') { cur_x = (cur_x + 8) & ~7; if (cur_x >= COLS) cur_x = COLS - 1; return; }
    if (ch == 7 || (unsigned char)ch < 32) return;
    cell *c = &grid[(size_t)cur_y * COLS + cur_x];
    c->ch = ch; c->fg = cur_fg; c->bg = cur_bg; c->bold = cur_bold;
    if (++cur_x >= COLS) { cur_x = 0; if (++cur_y >= ROWS) { cur_y = ROWS - 1; grid_scroll_up(); } }
}

static void erase_line(int mode) {
    int y = cur_y;
    if (mode == 0) for (int x = cur_x; x < COLS; x++) clear_cell(&grid[(size_t)y * COLS + x]);
    else if (mode == 1) for (int x = 0; x <= cur_x && x < COLS; x++) clear_cell(&grid[(size_t)y * COLS + x]);
    else for (int x = 0; x < COLS; x++) clear_cell(&grid[(size_t)y * COLS + x]);
}
static void erase_display(int mode) {
    if (mode >= 2) { term_reset(); return; }
    if (mode == 0) {
        for (int x = cur_x; x < COLS; x++) clear_cell(&grid[(size_t)cur_y * COLS + x]);
        for (int y = cur_y + 1; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) clear_cell(&grid[(size_t)y * COLS + x]);
    } else if (mode == 1) {
        for (int x = 0; x <= cur_x && x < COLS; x++) clear_cell(&grid[(size_t)cur_y * COLS + x]);
        for (int y = 0; y < cur_y; y++)
            for (int x = 0; x < COLS; x++) clear_cell(&grid[(size_t)y * COLS + x]);
    }
}

/* ------------------------------------------------------------ ANSI parser */
static int parse_state = 0;
static char csi[64];
static int csi_len = 0;

static void sgr(const char *p) {
    if (!*p) { cur_fg = 7; cur_bg = 0; cur_bold = 0; return; }
    while (*p) {
        int n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (n == 0) { cur_fg = 7; cur_bg = 0; cur_bold = 0; }
        else if (n == 1) cur_bold = 1;
        else if (n == 22) cur_bold = 0;
        else if (n >= 30 && n <= 37) cur_fg = n - 30;
        else if (n == 39) cur_fg = 7;
        else if (n >= 40 && n <= 47) cur_bg = n - 40;
        else if (n == 49) cur_bg = 0;
        else if (n >= 90 && n <= 97) cur_fg = n - 90 + 8;
        else if (n >= 100 && n <= 107) cur_bg = n - 100 + 8;
        else if (n == 38 || n == 48) {
            int isfg = (n == 38);
            if (*p == ';') p++;
            if (*p == '5') {
                p++; if (*p == ';') p++;
                int v = 0; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                if (isfg) cur_fg = v & 0xff; else cur_bg = v & 0xff;
            } else if (*p == '2') {
                p++;
                int rgb[3] = {0, 0, 0};
                for (int i = 0; i < 3; i++) {
                    if (*p == ';') p++;
                    while (*p >= '0' && *p <= '9') { rgb[i] = rgb[i] * 10 + (*p - '0'); p++; }
                }
                int v = 16 + (rgb[0] / 51) * 36 + (rgb[1] / 51) * 6 + (rgb[2] / 51);
                if (isfg) cur_fg = v; else cur_bg = v;
            }
        }
        if (*p == ';') p++;
    }
}

static void csi_dispatch(char final) {
    int a = 0, b = 0, seen = 0;
    if (csi_len) {
        const char *p = csi;
        while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
        if (*p == ';') { p++; seen = 1; while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; } }
    }
    switch (final) {
    case 'm': sgr(csi); break;
    case 'H': case 'f':
        cur_y = (a ? a - 1 : 0); cur_x = (seen && b ? b - 1 : 0);
        if (cur_y >= ROWS) cur_y = ROWS - 1;
        if (cur_x >= COLS) cur_x = COLS - 1;
        break;
    case 'A': cur_y -= (a ? a : 1); if (cur_y < 0) cur_y = 0; break;
    case 'B': cur_y += (a ? a : 1); if (cur_y >= ROWS) cur_y = ROWS - 1; break;
    case 'C': cur_x += (a ? a : 1); if (cur_x >= COLS) cur_x = COLS - 1; break;
    case 'D': cur_x -= (a ? a : 1); if (cur_x < 0) cur_x = 0; break;
    case 'G': cur_x = (a ? a - 1 : 0); break;
    case 'd': cur_y = (a ? a - 1 : 0); break;
    case 'J': erase_display(a); break;
    case 'K': erase_line(a); break;
    case 'h': if (!strcmp(csi, "?25")) cursor_visible = 1; break;
    case 'l': if (!strcmp(csi, "?25")) cursor_visible = 0; break;
    default: break;
    }
}

static void feed(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (parse_state == 1) {           /* saw ESC */
            if (ch == '[') { parse_state = 2; csi_len = 0; }
            else if (ch == ']') parse_state = 3;
            else parse_state = 0;
            continue;
        }
        if (parse_state == 3) {           /* OSC */
            if (ch == 7 || ch == '\\') parse_state = 0;
            continue;
        }
        if (parse_state == 2) {           /* CSI */
            if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?') {
                if (csi_len < (int)sizeof(csi) - 1) csi[csi_len++] = ch;
            } else {
                csi[csi_len] = 0;
                csi_dispatch(ch);
                parse_state = 0;
            }
            continue;
        }
        if (ch == 27) { parse_state = 1; continue; }
        put_ch(ch);
    }
}

/* ------------------------------------------------------------ pty plumbing */
static void spawn_shell(void) {
    struct winsize ws = { ROWS, COLS, 0, 0 };
    child = forkpty(&master_fd, NULL, NULL, &ws);
    if (child == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("PS1", "\\[\\e[1;34m\\]slop\\[\\e[0m\\]:\\w$ ", 1);
        setenv("HOME", "/home/user", 1);
        setenv("PATH", "/usr/bin:/bin:/sbin", 1);
        execl("/bin/sh", "sh", (char *)NULL);
        execl("/usr/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    fcntl(master_fd, F_SETFL, O_NONBLOCK);
}

static void pty_pump(void) {
    char buf[4096];
    for (;;) {
        int n = read(master_fd, buf, sizeof(buf));
        if (n <= 0) break;
        feed(buf, n);
    }
}
static void pty_send(const char *s, int n) { if (master_fd >= 0) { ssize_t r = write(master_fd, s, n); (void)r; } }

/* ------------------------------------------------------------------ render */
static int g_cw, g_chh;

static void render(int wx, int wy, int ww, int wh) {
    int cw = g_cw, chh = g_chh;
    slop_fill_rect(wx, wy, ww, wh, SLOP_RGB(0x0a, 0x0c, 0x12));
    for (int ry = 0; ry < ROWS; ry++) {
        int dy = wy + ry * chh;
        if (dy + chh < wy || dy > wy + wh) continue;
        const cell *row;
        int li = ry + (sb_count > view_off ? sb_count - view_off : 0);
        if (view_off > 0 && li < sb_count) {
            int ring = ((sb_head - sb_count + li) % SB_MAX + SB_MAX) % SB_MAX;
            row = &sb[(size_t)ring * COLS];
        } else {
            row = &grid[(size_t)ry * COLS];
        }
        int baseline = dy + 2 + slop_font_mono.ascent;
        int px = wx + 6;
        for (int rx = 0; rx < COLS; rx++) {
            const cell *c = &row[rx];
            if (c->bg) slop_fill_rect(px, dy, cw, chh, pal[c->bg & 0xff]);
            if (c->ch != ' ') {
                slop_color fg = pal[c->fg & 0xff];
                if (c->bold) fg = slop_mix(fg, SLOP_RGB(255, 255, 255), 60);
                char s[2] = { c->ch, 0 };
                slop_text(&slop_font_mono, px, baseline, s, fg);
            }
            px += cw;
        }
    }
    if (view_off == 0 && cursor_visible && ((slop_ticks_ms() / 500) & 1)) {
        int dy = wy + cur_y * chh;
        int px = wx + 6 + cur_x * cw;
        slop_fill_rect(px, dy, cw, chh, SLOP_RGB(0x8a, 0xa4, 0xff));
        if (grid[(size_t)cur_y * COLS + cur_x].ch != ' ') {
            char s[2] = { grid[(size_t)cur_y * COLS + cur_x].ch, 0 };
            slop_text(&slop_font_mono, px, dy + 2 + slop_font_mono.ascent, s, SLOP_RGB(0x0a, 0x0c, 0x12));
        }
    }
}

static void on_resize(int ww, int wh) {
    int nc = (ww - 12) / g_cw;
    int nr = wh / g_chh;
    if (nc < 20) nc = 20;
    if (nr < 5) nr = 5;
    if (nc == COLS && nr == ROWS) return;
    cell *ng = calloc((size_t)nc * nr, sizeof(cell));
    for (int i = 0; i < nc * nr; i++) { ng[i].ch = ' '; ng[i].fg = 7; ng[i].bg = 0; }
    int cpr = nc < COLS ? nc : COLS, rpr = nr < ROWS ? nr : ROWS;
    for (int y = 0; y < rpr; y++)
        memcpy(&ng[(size_t)y * nc], &grid[(size_t)y * COLS], sizeof(cell) * cpr);
    free(grid); free(sb);
    grid = ng; COLS = nc; ROWS = nr;
    sb = calloc((size_t)SB_MAX * COLS, sizeof(cell));
    sb_count = 0; sb_head = 0; view_off = 0;
    if (cur_x >= COLS) cur_x = COLS - 1;
    if (cur_y >= ROWS) cur_y = ROWS - 1;
    if (master_fd >= 0) { struct winsize ws = { ROWS, COLS, 0, 0 }; ioctl(master_fd, TIOCSWINSZ, &ws); }
}

int main(void) {
    if (slop_app_start("Terminal", SLOP_ICON_TERM) != 0) { fprintf(stderr, "slop-term: no display\n"); return 1; }
    init_palette();
    g_cw = slop_font_mono.glyphs['M' - 32].adv;
    g_chh = slop_font_mono.height + 4;
    COLS = (slop.w - 2 * 24 - 12) / g_cw;
    ROWS = (slop.h - 2 * 24 - 48) / g_chh;
    if (COLS < 20) COLS = 20;
    if (ROWS < 5) ROWS = 5;
    grid = calloc((size_t)COLS * ROWS, sizeof(cell));
    sb = calloc((size_t)SB_MAX * COLS, sizeof(cell));
    term_reset();
    spawn_shell();
    slop_flush_events();

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;
        pty_pump();

        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("Terminal", "pty session  -  /bin/sh", 0);
        slop_badge(c.x + c.w - 92, c.y - 42, "pty", slop_theme_dark.good);

        on_resize(c.w, c.h);

        if (ui.wheel) {
            view_off -= ui.wheel * 3;
            if (view_off < 0) view_off = 0;
            if (view_off > sb_count) view_off = sb_count;
        }
        if (ui.key == SLOP_KEY_PGUP) view_off += ROWS - 1;
        if (ui.key == SLOP_KEY_PGDN) view_off -= ROWS - 1;
        if (view_off < 0) view_off = 0;
        if (view_off > sb_count) view_off = sb_count;

        render(c.x, c.y, c.w, c.h);

        if (view_off == 0) {
            if (ui.ntext) pty_send(ui.text, ui.ntext);
            if (ui.ctrl_ch) pty_send(&ui.ctrl_ch, 1);
            switch (ui.key) {
            case SLOP_KEY_ENTER: pty_send("\r", 1); break;
            case SLOP_KEY_BACKSPACE: pty_send("\x7f", 1); break;
            case SLOP_KEY_TAB: pty_send("\t", 1); break;
            case SLOP_KEY_UP: pty_send("\x1b[A", 3); break;
            case SLOP_KEY_DOWN: pty_send("\x1b[B", 3); break;
            case SLOP_KEY_RIGHT: pty_send("\x1b[C", 3); break;
            case SLOP_KEY_LEFT: pty_send("\x1b[D", 3); break;
            case SLOP_KEY_HOME: pty_send("\x1b[H", 3); break;
            case SLOP_KEY_END: pty_send("\x1b[F", 3); break;
            case SLOP_KEY_DELETE: pty_send("\x1b[3~", 4); break;
            default: break;
            }
        }

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(5000);
    }

    if (master_fd >= 0) close(master_fd);
    if (child > 0) { kill(child, SIGHUP); waitpid(child, NULL, WNOHANG); }
    slop_shutdown();
    return 0;
}
