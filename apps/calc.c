/*
 * slop-calc -- a small calculator with a real keypad and keyboard support.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

static char display[32] = "0";
static double acc = 0;
static char pending = 0;
static int fresh = 1;         /* next digit starts a new number */
static char history[64] = "";
static char entry[32] = "0";  /* the number being typed */

static void set_entry_from(double v) {
    if (v == (long long)v) snprintf(entry, sizeof(entry), "%lld", (long long)v);
    else snprintf(entry, sizeof(entry), "%g", v);
}

static void apply(char op) {
    double cur = atof(entry);
    if (pending) {
        switch (pending) {
        case '+': acc += cur; break;
        case '-': acc -= cur; break;
        case '*': acc *= cur; break;
        case '/': acc = cur != 0 ? acc / cur : 0; break;
        }
    } else acc = cur;
    set_entry_from(acc);
    pending = op;
    fresh = 1;
    snprintf(history, sizeof(history), "%g %c", acc, op);
}

static void digit(char d) {
    if (fresh) { entry[0] = 0; fresh = 0; }
    int n = (int)strlen(entry);
    if (n < (int)sizeof(entry) - 2) { entry[n] = d; entry[n + 1] = 0; }
}
static void clear_all(void) { entry[0] = '0'; entry[1] = 0; acc = 0; pending = 0; fresh = 1; history[0] = 0; }
static void backspace(void) {
    int n = (int)strlen(entry);
    if (n > 1) entry[n - 1] = 0; else { entry[0] = '0'; entry[1] = 0; }
}

int main(void) {
    if (slop_app_start("Calculator", SLOP_ICON_CALC) != 0) return 1;
    slop_flush_events();

    const char *keys[5][4] = {
        { "C", "(", ")", "/" },
        { "7", "8", "9", "*" },
        { "4", "5", "6", "-" },
        { "1", "2", "3", "+" },
        { "0", ".", "<", "=" },
    };

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;
        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("Calculator", "basic arithmetic", 0);

        /* display */
        int dx = c.x + 16, dy = c.y + 16, dw = c.w - 32, dh = 84;
        slop_fill_round(dx, dy, dw, dh, 12, slop_theme_dark.surface_lo);
        slop_round_outline(dx, dy, dw, dh, 12, 1, slop_theme_dark.border);
        if (history[0]) {
            int hw = slop_text_w(&slop_font_small, history);
            slop_text(&slop_font_small, dx + dw - hw - 14, dy + 24, history, slop_theme_dark.text_mute);
        }
        int ew = slop_text_w(&slop_font_title, entry);
        slop_text(&slop_font_title, dx + dw - ew - 14, dy + 66, entry, slop_theme_dark.text);

        /* keypad */
        int gx = c.x + 16, gy = c.y + 112;
        int gw = c.w - 32, gh = c.h - 128;
        int kw = gw / 4, kh = gh / 5;
        for (int r = 0; r < 5; r++) {
            for (int col = 0; col < 4; col++) {
                const char *k = keys[r][col];
                int x = gx + col * kw, y = gy + r * kh;
                int hot = ui.mx >= x + 4 && ui.mx < x + kw - 4 && ui.my >= y + 4 && ui.my < y + kh - 4;
                int is_op = (col == 3);
                int is_act = (!strcmp(k, "C") || !strcmp(k, "<"));
                int is_eq = !strcmp(k, "=");
                slop_color bg = is_eq ? slop_theme_dark.accent
                             : is_op ? slop_theme_dark.surface_hi
                             : is_act ? SLOP_RGB(0x3a, 0x22, 0x28)
                                      : slop_theme_dark.surface;
                if (hot) slop_lighten(&bg, 20);
                slop_fill_round(x + 4, y + 4, kw - 8, kh - 8, 10, bg);
                slop_round_outline(x + 4, y + 4, kw - 8, kh - 8, 10, 1, slop_theme_dark.border);
                int tw = slop_text_w(&slop_font_bold, k);
                slop_text(&slop_font_bold, x + kw / 2 - tw / 2, y + kh / 2 + 6, k,
                          is_eq ? SLOP_RGB(255,255,255) : slop_theme_dark.text);
                if (hot && ui.released) {
                    if (k[0] >= '0' && k[0] <= '9') digit(k[0]);
                    else if (!strcmp(k, ".")) { if (!strchr(entry, '.')) { if (fresh) { strcpy(entry, "0."); fresh = 0; } else strcat(entry, "."); } }
                    else if (!strcmp(k, "C")) clear_all();
                    else if (!strcmp(k, "<")) backspace();
                    else if (!strcmp(k, "=")) { apply(0); pending = 0; history[0] = 0; }
                    else apply(k[0]);
                }
            }
        }

        /* keyboard */
        for (int i = 0; i < ui.ntext; i++) {
            char ch = ui.text[i];
            if (ch >= '0' && ch <= '9') digit(ch);
            else if (ch == '.') { if (!strchr(entry, '.')) strcat(entry, "."); }
            else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') apply(ch);
            else if (ch == '=' || ch == '\n') { apply(0); pending = 0; }
            else if (ch == 'c' || ch == 'C') clear_all();
        }
        if (ui.key == SLOP_KEY_BACKSPACE) backspace();
        if (ui.key == SLOP_KEY_ESC) clear_all();
        (void)display;

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(5000);
    }
    slop_shutdown();
    return 0;
}
