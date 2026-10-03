/*
 * slop-settings -- a small settings app.
 *
 * It writes a plain key=value file under /home/user/.config/slop/settings.conf
 * so preferences survive across sessions. Appearance, wallpaper accent and
 * behaviour are covered.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int page = 0;
static int anim = 1, shadows = 1, accent = 0, dock_size = 2, clock24 = 1;

static const char *PAGES[] = { "Appearance", "Desktop", "Behaviour", "About" };
static const int NPAGES = 4;
static slop_color ACCENT_COLORS[] = {
    SLOP_RGB(0x6c, 0x8c, 0xff), SLOP_RGB(0xff, 0x7a, 0x59),
    SLOP_RGB(0x4a, 0xd9, 0x9a), SLOP_RGB(0x9a, 0xa2, 0xb8),
};

static const char *CONF = "/home/user/.config/slop/settings.conf";

static void save(void) {
    mkdir("/home/user/.config", 0755);
    mkdir("/home/user/.config/slop", 0755);
    FILE *f = fopen(CONF, "w");
    if (!f) return;
    fprintf(f, "animations=%d\nshadows=%d\naccent=%d\ndock_size=%d\nclock24=%d\n",
            anim, shadows, accent, dock_size, clock24);
    fclose(f);
}
static void load(void) {
    FILE *f = fopen(CONF, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "animations")) anim = v;
        else if (!strcmp(line, "shadows")) shadows = v;
        else if (!strcmp(line, "accent")) accent = v;
        else if (!strcmp(line, "dock_size")) dock_size = v;
        else if (!strcmp(line, "clock24")) clock24 = v;
    }
    fclose(f);
}

int main(void) {
    if (slop_app_start("Settings", SLOP_ICON_SETTINGS) != 0) return 1;
    load();
    slop_flush_events();
    int dirty = 0;

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;
        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("Settings", "changes are saved automatically", 0);

        /* sidebar */
        int sbw = 200;
        slop_fill_rect(c.x, c.y, sbw, c.h, slop_theme_dark.surface_lo);
        slop_vline(c.x + sbw - 1, c.y, c.h, slop_theme_dark.border);
        int py = c.y + 20;
        for (int i = 0; i < NPAGES; i++) {
            int hot = ui.mx >= c.x && ui.mx < c.x + sbw - 1 && ui.my >= py && ui.my < py + 40;
            if (page == i) slop_fill_round(c.x + 8, py, sbw - 20, 40, 8, slop_theme_dark.accent_lo);
            else if (hot) slop_fill_round(c.x + 8, py, sbw - 20, 40, 8, slop_theme_dark.surface_hi);
            slop_text_vcenter(&slop_font_body, c.x + 24, py, 40, PAGES[i],
                              page == i ? slop_theme_dark.text : slop_theme_dark.text_dim);
            if (hot && ui.released) page = i;
            py += 44;
        }

        int px = c.x + sbw + 24, pw = c.w - sbw - 48;
        int ry = c.y + 24;

        if (page == 0) {
            slop_text(&slop_font_title, px, ry + 8, "Appearance", slop_theme_dark.text);
            ry += 44;
            slop_card(px, ry, pw, 60, "Accent colour", "used across the whole desktop");
            for (int i = 0; i < 4; i++) {
                int swx = px + 24 + i * 60;
                int swy = ry + 34;
                int hot = ui.mx >= swx - 4 && ui.mx < swx + 36 && ui.my >= swy - 4 && ui.my < swy + 36;
                slop_fill_circle(swx + 16, swy + 16, 16, ACCENT_COLORS[i]);
                if (accent == i) slop_circle(swx + 16, swy + 16, 18, slop_theme_dark.text);
                if (hot && ui.released) { accent = i; dirty = 1; }
            }
            ry += 76;
            slop_card(px, ry, pw, 56, "Animations", "smooth window and toast motion");
            if (slop_toggle(px + pw - 72, ry + 16, &anim)) dirty = 1;
            ry += 72;
            slop_card(px, ry, pw, 56, "Window shadows", "soft depth behind windows");
            if (slop_toggle(px + pw - 72, ry + 16, &shadows)) dirty = 1;
        } else if (page == 1) {
            slop_text(&slop_font_title, px, ry + 8, "Desktop", slop_theme_dark.text);
            ry += 44;
            slop_card(px, ry, pw, 70, "Dock icon size", "small, medium or large");
            static const char *sizes[] = { "Small", "Medium", "Large" };
            slop_segmented(px + 24, ry + 26, 220, 32, sizes, 3, &dock_size);
            ry += 90;
            slop_card(px, ry, pw, 56, "24-hour clock", "show 13:00 instead of 1:00 PM");
            if (slop_toggle(px + pw - 72, ry + 16, &clock24)) dirty = 1;
        } else if (page == 2) {
            slop_text(&slop_font_title, px, ry + 8, "Behaviour", slop_theme_dark.text);
            ry += 44;
            slop_text(&slop_font_body, px, ry + 10, "Keyboard shortcuts", slop_theme_dark.text_dim);
            ry += 30;
            const char *keys[][2] = {
                { "M", "Open the application launcher" },
                { "1-9", "Launch an app from the dock" },
                { "L", "Lock the screen" },
                { "Alt+Tab", "Switch between windows" },
                { "Ctrl+Q", "Close the focused window" },
                { "Esc", "Dismiss menus and dialogs" },
            };
            for (int i = 0; i < 6; i++) {
                slop_card(px, ry, pw, 40, keys[i][0], keys[i][1]);
                ry += 46;
            }
        } else {
            slop_text(&slop_font_title, px, ry + 8, "About", slop_theme_dark.text);
            ry += 44;
            slop_stat_row(px, ry, pw, "System", "SlopOS 0.2 \"Aurora\"", slop_theme_dark.accent_hi);
            slop_stat_row(px, ry + 30, pw, "Toolkit", "libslop (native, no compat layer)",
                          slop_theme_dark.text_dim);
            slop_stat_row(px, ry + 60, pw, "Compositor", "shared-memory window manager",
                          slop_theme_dark.text_dim);
            slop_stat_row(px, ry + 90, pw, "Third-party", "Zen, OBS, Resolve via compatibility",
                          slop_theme_dark.warn);
        }

        if (dirty) { save(); dirty = 0; }

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(5000);
    }
    save();
    slop_shutdown();
    return 0;
}
