/*
 * slop-about -- System Info.
 *
 * Reads live values straight from /proc and /sys so the numbers are real,
 * not hard-coded. Doubles as the "what is this OS" screen.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

static void read_first_line(const char *path, char *out, int cap, const char *dflt) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(out, cap, "%s", dflt); return; }
    if (!fgets(out, cap, f)) snprintf(out, cap, "%s", dflt);
    int n = strlen(out);
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = 0;
    fclose(f);
}

static void row(int x, int y, int w, const char *label, const char *value) {
    slop_text_vcenter(&slop_font_body, x, y, 34, label, slop_theme_dark.text_dim);
    slop_text_vcenter(&slop_font_bold, x + 220, y, 34, value, slop_theme_dark.text);
    slop_hline(x, y + 34, w, slop_theme_dark.border);
}

int main(void) {
    if (slop_app_start("System Info", SLOP_ICON_ABOUT) != 0) { fprintf(stderr, "slop-about: no display\n"); return 1; }
    slop_flush_events();

    struct utsname u;
    uname(&u);
    struct sysinfo si;
    sysinfo(&si);
    char cpu[256], memtotal[64], memfree[64];
    read_first_line("/proc/cpuinfo", cpu, sizeof(cpu), "unknown CPU");
    /* extract "model name" */
    char model[256] = "x86_64 processor";
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "model name", 10)) {
                char *c = strchr(line, ':');
                if (c) { snprintf(model, sizeof(model), "%s", c + 2); int n = strlen(model); while (n && (model[n-1]=='\n')) model[--n]=0; }
                break;
            }
        }
        fclose(f);
    }
    slop_human_size(si.totalram * si.mem_unit, memtotal, sizeof(memtotal));
    slop_human_size(si.freeram * si.mem_unit, memfree, sizeof(memfree));
    char up[64];
    snprintf(up, sizeof(up), "%ld min", si.uptime / 60);

    int cpu_count = 0;
    f = fopen("/proc/cpuinfo", "r");
    if (f) { char line[256]; while (fgets(line, sizeof(line), f)) if (!strncmp(line, "processor", 9)) cpu_count++; fclose(f); }
    char cputxt[64]; snprintf(cputxt, sizeof(cputxt), "%d cores", cpu_count ? cpu_count : 1);

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;

        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("System Info", "SlopOS 0.1", 0);

        /* hero card */
        int hx = c.x + 24, hy = c.y + 20, hw = c.w - 48, hh = 110;
        slop_fill_round(hx, hy, hw, hh, 14, slop_theme_dark.surface);
        slop_round_outline(hx, hy, hw, hh, 14, 1, slop_theme_dark.border);
        slop_fill_round(hx + 20, hy + 20, 70, 70, 18, slop_theme_dark.accent);
        int sw = slop_text_w(&slop_font_title, "S");
        slop_text(&slop_font_title, hx + 20 + 35 - sw / 2, hy + 20 + 45, "S", SLOP_RGB(255,255,255));
        slop_text(&slop_font_title, hx + 110, hy + 52, "SlopOS", slop_theme_dark.text);
        slop_text(&slop_font_small, hx + 110, hy + 74, "native desktop  -  x86_64  -  Linux ABI", slop_theme_dark.text_mute);

        int y = hy + hh + 24;
        int x = c.x + 24, w = c.w - 48;
        row(x, y, w, "Kernel", u.release); y += 36;
        row(x, y, w, "Architecture", u.machine); y += 36;
        row(x, y, w, "Processor", cputxt); y += 36;
        row(x, y, w, "CPU model", model); y += 36;
        row(x, y, w, "Memory", memtotal); y += 36;
        char mf[96]; snprintf(mf, sizeof(mf), "%s free", memfree);
        row(x, y, w, "Memory free", mf); y += 36;
        row(x, y, w, "Uptime", up); y += 36;
        row(x, y, w, "Shell", "slop-shell (framebuffer)"); y += 36;
        row(x, y, w, "Native apps", "Files, Terminal, Image, Viewer"); y += 36;
        row(x, y, w, "Compat apps", "Zen Browser, OBS Studio, DaVinci Resolve"); y += 40;

        const char *note = "Native apps talk straight to the framebuffer. Third-party apps run through the compatibility runtime.";
        slop_text(&slop_font_small, x, y + 4, note, slop_theme_dark.text_mute);

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(15000);
    }
    slop_shutdown();
    return 0;
}
