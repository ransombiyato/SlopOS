/*
 * slop-monitor -- a live system monitor.
 *
 * Reads /proc for CPU, memory, load and uptime, draws ring and bar gauges, and
 * lists the running processes sorted by CPU time. Updates once a second.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

typedef struct { int pid; char comm[32]; unsigned long utime, stime; long rss; } proc_t;
static proc_t procs[256];
static int nprocs = 0;

static unsigned long long prev_total = 0, prev_idle = 0;
static int cpu_pct = 0;
static long mem_total = 0, mem_avail = 0, mem_used = 0;
static double load1 = 0, load5 = 0, load15 = 0;
static double uptime = 0;
static int ncpu = 1;

static int cmp_proc(const void *a, const void *b) {
    const proc_t *x = a, *y = b;
    unsigned long tx = x->utime + x->stime, ty = y->utime + y->stime;
    if (tx != ty) return ty > tx ? 1 : -1;
    return x->pid - y->pid;
}

static void sample(void) {
    /* CPU from /proc/stat */
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        char line[512];
        if (fgets(line, sizeof(line), f)) {
            unsigned long long v[10] = {0};
            int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
            if (n >= 4) {
                unsigned long long idle = v[3] + v[4];
                unsigned long long total = 0;
                for (int i = 0; i < n && i < 10; i++) total += v[i];
                unsigned long long dt = total - prev_total;
                unsigned long long di = idle - prev_idle;
                if (dt > 0) cpu_pct = (int)(100 * (dt - di) / dt);
                prev_total = total; prev_idle = idle;
            }
        }
        fclose(f);
    }
    /* memory */
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char key[64], unit[16]; long val;
        mem_total = mem_avail = 0;
        while (fscanf(f, "%63s %ld %15s", key, &val, unit) == 3) {
            if (!strcmp(key, "MemTotal:")) mem_total = val;
            else if (!strcmp(key, "MemAvailable:")) mem_avail = val;
        }
        fclose(f);
        mem_used = mem_total - mem_avail;
    }
    /* load + uptime */
    f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf %lf %lf", &load1, &load5, &load15) != 3) {} fclose(f); }
    f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &uptime) != 1) {} fclose(f); }
    /* cpu count */
    ncpu = 0;
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) if (!strncmp(line, "processor", 9)) ncpu++;
        fclose(f);
    }
    if (ncpu < 1) ncpu = 1;

    /* processes */
    nprocs = 0;
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && nprocs < 256) {
            if (!isdigit((unsigned char)de->d_name[0])) continue;
            int pid = atoi(de->d_name);
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/stat", pid);
            FILE *pf = fopen(path, "r");
            if (!pf) continue;
            char buf[1024];
            if (fgets(buf, sizeof(buf), pf)) {
                /* comm is in parentheses and may contain spaces */
                char *l = strchr(buf, '(');
                char *r = strrchr(buf, ')');
                if (l && r && r > l) {
                    proc_t *p = &procs[nprocs];
                    p->pid = pid;
                    int cl = (int)(r - l - 1);
                    if (cl > 31) cl = 31;
                    memcpy(p->comm, l + 1, cl); p->comm[cl] = 0;
                    /* fields after ')': state, ppid, ... utime=14, stime=15, rss=24 */
                    unsigned long long vals[32] = {0};
                    int n = 0;
                    char *tok = strtok(r + 2, " ");
                    while (tok && n < 32) { vals[n++] = strtoull(tok, NULL, 10); tok = strtok(NULL, " "); }
                    if (n > 15) { p->utime = (unsigned long)vals[11]; p->stime = (unsigned long)vals[12]; }
                    if (n > 21) p->rss = (long)vals[21];
                    nprocs++;
                }
            }
            fclose(pf);
        }
        closedir(d);
    }
    qsort(procs, nprocs, sizeof(proc_t), cmp_proc);
}

int main(void) {
    if (slop_app_start("System Monitor", SLOP_ICON_MONITOR) != 0) return 1;
    slop_flush_events();
    sample();
    int last = 0;

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;
        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("System Monitor", "live", 0);

        if (slop_ticks_ms() - last > 1000) { sample(); last = slop_ticks_ms(); }

        /* gauges */
        int gx = c.x + 40, gy = c.y + 120;
        slop_ring_gauge(gx, gy, 52, cpu_pct, slop_theme_dark.accent, NULL);
        char pbuf[16]; snprintf(pbuf, sizeof(pbuf), "%d%%", cpu_pct);
        int pw = slop_text_w(&slop_font_title, pbuf);
        slop_text(&slop_font_title, gx - pw / 2, gy + 4, pbuf, slop_theme_dark.text);
        slop_text(&slop_font_small, gx - slop_text_w(&slop_font_small, "CPU") / 2, gy + 70, "CPU",
                  slop_theme_dark.text_mute);

        int mpct = mem_total > 0 ? (int)(100 * mem_used / mem_total) : 0;
        int mx = c.x + 180;
        slop_ring_gauge(mx, gy, 52, mpct, slop_theme_dark.good, NULL);
        char mbuf[16]; snprintf(mbuf, sizeof(mbuf), "%d%%", mpct);
        int mw = slop_text_w(&slop_font_title, mbuf);
        slop_text(&slop_font_title, mx - mw / 2, gy + 4, mbuf, slop_theme_dark.text);
        slop_text(&slop_font_small, mx - slop_text_w(&slop_font_small, "MEMORY") / 2, gy + 70,
                  "MEMORY", slop_theme_dark.text_mute);

        /* stats panel */
        int sx = c.x + 300, sw = c.w - 320;
        slop_card(sx, c.y + 16, sw, 168, "System", NULL);
        char t1[32], t2[32], t3[32], t4[32];
        snprintf(t1, sizeof(t1), "%.2f", load1);
        snprintf(t2, sizeof(t2), "%.2f", load5);
        snprintf(t3, sizeof(t3), "%.2f", load15);
        snprintf(t4, sizeof(t4), "%d", ncpu);
        slop_stat_row(sx + 16, c.y + 56, sw - 32, "Load average (1m)", t1, slop_theme_dark.text);
        slop_stat_row(sx + 16, c.y + 86, sw - 32, "Load average (5m)", t2, slop_theme_dark.text_dim);
        slop_stat_row(sx + 16, c.y + 116, sw - 32, "Load average (15m)", t3, slop_theme_dark.text_dim);
        slop_stat_row(sx + 16, c.y + 146, sw - 32, "Processors", t4, slop_theme_dark.accent_hi);

        int by = c.y + 200;
        slop_text(&slop_font_small, sx, by, "MEMORY USE", slop_theme_dark.text_mute);
        char hu[32], ht[32];
        slop_human_size(mem_used * 1024, hu, sizeof(hu));
        slop_human_size(mem_total * 1024, ht, sizeof(ht));
        char mline[80]; snprintf(mline, sizeof(mline), "%s of %s", hu, ht);
        int mlw = slop_text_w(&slop_font_small, mline);
        slop_text(&slop_font_small, sx + sw - mlw, by, mline, slop_theme_dark.text_dim);
        slop_bar_gauge(sx, by + 10, sw, 12, mpct, slop_theme_dark.good);

        /* uptime */
        int uh = (int)(uptime / 3600), um = (int)((uptime - uh * 3600) / 60);
        char up[48]; snprintf(up, sizeof(up), "up %dh %dm", uh, um);
        slop_text(&slop_font_small, sx, by + 40, up, slop_theme_dark.text_mute);

        /* process list */
        int py = c.y + 268;
        slop_hline(c.x + 20, py - 12, c.w - 40, slop_theme_dark.border);
        slop_text(&slop_font_small, c.x + 24, py, "PID", slop_theme_dark.text_mute);
        slop_text(&slop_font_small, c.x + 84, py, "PROCESS", slop_theme_dark.text_mute);
        slop_text(&slop_font_small, c.x + c.w - 120, py, "CPU", slop_theme_dark.text_mute);
        slop_text(&slop_font_small, c.x + c.w - 60, py, "RSS", slop_theme_dark.text_mute);
        int rows = (c.y + c.h - 40 - py - 20) / 24;
        for (int i = 0; i < rows && i < nprocs; i++) {
            proc_t *p = &procs[i];
            int ry = py + 20 + i * 24;
            if (i % 2) slop_fill_rect(c.x + 20, ry, c.w - 40, 22, slop_theme_dark.surface_lo);
            char pid[16]; snprintf(pid, sizeof(pid), "%d", p->pid);
            slop_text_vcenter(&slop_font_mono, c.x + 24, ry, 22, pid, slop_theme_dark.text_dim);
            slop_text_vcenter(&slop_font_body, c.x + 84, ry, 22, p->comm, slop_theme_dark.text);
            char t[24]; snprintf(t, sizeof(t), "%lu", p->utime + p->stime);
            slop_text_vcenter(&slop_font_mono, c.x + c.w - 120, ry, 22, t, slop_theme_dark.accent_hi);
            char rs[24]; slop_human_size(p->rss * 4096, rs, sizeof(rs));
            slop_text_vcenter(&slop_font_mono, c.x + c.w - 60, ry, 22, rs, slop_theme_dark.text_dim);
        }

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(16000);
    }
    slop_shutdown();
    return 0;
}
