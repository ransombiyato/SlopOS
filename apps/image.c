/*
 * slop-image -- the SlopOS image viewer.
 *
 * Decodes PNG/JPEG/GIF/BMP via stb_image, fits the image to the window,
 * supports zoom, pan, rotate, and a filmstrip of the other images in the
 * same folder.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static slop_image img;
static int have_img = 0;
static char cur_path[1024];
static char dir_path[1024];
static char status[256];
static float zoom = 1.0f;
static int pan_x = 0, pan_y = 0;
static int rotation = 0;

static char siblings[256][256];
static int nsib = 0, sib_sel = 0;

static int is_image(const char *name) {
    const char *e = slop_ext(name);
    return !strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") ||
           !strcasecmp(e, "gif") || !strcasecmp(e, "bmp") || !strcasecmp(e, "tga");
}

static void scan_siblings(const char *path) {
    nsib = 0;
    snprintf(dir_path, sizeof(dir_path), "%s", path);
    char *s = strrchr(dir_path, '/');
    if (s) *s = 0; else strcpy(dir_path, ".");
    struct dirent *de;
    DIR *d = opendir(dir_path);
    if (!d) return;
    while ((de = readdir(d)) && nsib < 256) {
        if (de->d_name[0] == '.') continue;
        if (!is_image(de->d_name)) continue;
        if (snprintf(siblings[nsib], sizeof(siblings[0]), "%s", de->d_name)
                >= (int)sizeof(siblings[0])) continue;
        if (!strcmp(siblings[nsib], slop_basename(path))) sib_sel = nsib;
        nsib++;
    }
    closedir(d);
}

static void load(const char *path) {
    if (have_img) { slop_image_free(&img); have_img = 0; }
    if (slop_image_load(path, &img) == 0) {
        have_img = 1;
        snprintf(cur_path, sizeof(cur_path), "%s", path);
        zoom = 1.0f; pan_x = pan_y = 0; rotation = 0;
        snprintf(status, sizeof(status), "%dx%d  -  %s", img.w, img.h, slop_basename(path));
    } else {
        snprintf(status, sizeof(status), "Cannot decode %s", slop_basename(path));
    }
}

static void load_index(int i) {
    if (i < 0 || i >= nsib) return;
    sib_sel = i;
    load(siblings[i]);
}

/* Bilinear-ish scaled draw with rotation (90-degree steps). */
static void draw_image_region(int cx, int cy) {
    if (!have_img) return;
    int dw = (int)(img.w * zoom);
    int dh = (int)(img.h * zoom);
    if (rotation == 90 || rotation == 270) { int t = dw; dw = dh; dh = t; }
    int x = cx - dw / 2 + pan_x;
    int y = cy - dh / 2 + pan_y;
    if (rotation == 0) {
        slop_image_draw_scaled(&img, x, y, dw, dh);
    } else {
        /* nearest-neighbour rotation: iterate destination, map back */
        for (int j = 0; j < dh; j++) {
            for (int i = 0; i < dw; i++) {
                int sx, sy;
                if (rotation == 90)  { sx = j * img.w / dh; sy = img.h - 1 - i * img.h / dw; }
                else if (rotation == 180) { sx = img.w - 1 - i * img.w / dw; sy = img.h - 1 - j * img.h / dh; }
                else { sx = img.w - 1 - j * img.w / dh; sy = i * img.h / dw; }
                if (sx < 0 || sx >= img.w || sy < 0 || sy >= img.h) continue;
                const uint8_t *p = &img.rgba[(size_t)(sy * img.w + sx) * 4];
                slop_blend(x + i, y + j, SLOP_RGB(p[0], p[1], p[2]), p[3]);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (slop_init() != 0) { fprintf(stderr, "slop-image: no framebuffer\n"); return 1; }
    const char *start = argc > 1 ? argv[1] : "/home/user/Pictures";
    struct stat st;
    if (stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* open the first image in the folder so the viewer is never empty */
        DIR *d = opendir(start);
        char first[1024] = "";
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.' || !is_image(de->d_name)) continue;
                snprintf(first, sizeof(first), "%s/%s", start, de->d_name);
                break;
            }
            closedir(d);
        }
        if (first[0]) { scan_siblings(first); load(first); }
        else snprintf(status, sizeof(status), "No images in %s", start);
    } else {
        scan_siblings(start);
        load(start);
    }
    slop_flush_events();
    int dragging = 0, drag_x = 0, drag_y = 0, start_pan_x = 0, start_pan_y = 0;

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;

        slop_clear(SLOP_RGB(0x08, 0x09, 0x0f));
        slop_chrome c = slop_window("Image Viewer", status, 0);

        /* toolbar */
        int tx = c.x + 12, ty = c.y + 8, th = 30;
        if (slop_toolbar_button(tx, ty, th, "-", 0)) { zoom *= 0.8f; if (zoom < 0.05f) zoom = 0.05f; }
        tx += 36;
        if (slop_toolbar_button(tx, ty, th, "+", 0)) { zoom *= 1.25f; if (zoom > 20) zoom = 20; }
        tx += 40;
        if (slop_toolbar_button(tx, ty, th, "Fit", 0) && have_img) {
            float fx = (float)(c.w - 40) / img.w, fy = (float)(c.h - 90) / img.h;
            zoom = fx < fy ? fx : fy; pan_x = pan_y = 0;
        }
        tx += 52;
        if (slop_toolbar_button(tx, ty, th, "1:1", 0)) { zoom = 1.0f; pan_x = pan_y = 0; }
        tx += 52;
        if (slop_toolbar_button(tx, ty, th, "Rotate", 0)) rotation = (rotation + 90) % 360;
        tx += 74;
        if (slop_toolbar_button(tx, ty, th, "<", 0)) load_index(sib_sel - 1);
        tx += 36;
        if (slop_toolbar_button(tx, ty, th, ">", 0)) load_index(sib_sel + 1);

        /* canvas */
        int cvx = c.x, cvy = c.y + 46, cvw = c.w, cvh = c.h - 46 - 74;
        slop_fill_rect(cvx, cvy, cvw, cvh, SLOP_RGB(0x05, 0x06, 0x0a));
        /* checkerboard */
        for (int j = 0; j < cvh; j += 24)
            for (int i = 0; i < cvw; i += 24)
                if (((i / 24) + (j / 24)) & 1)
                    slop_fill_rect(cvx + i, cvy + j, 24, 24, SLOP_RGB(0x0b, 0x0d, 0x14));

        if (have_img) {
            int cxc = cvx + cvw / 2, cyc = cvy + cvh / 2;
            draw_image_region(cxc, cyc);
            /* border */
            int dw = (int)(img.w * zoom), dh = (int)(img.h * zoom);
            if (rotation == 90 || rotation == 270) { int t = dw; dw = dh; dh = t; }
            slop_rect(cxc - dw / 2 + pan_x - 1, cyc - dh / 2 + pan_y - 1, dw + 2, dh + 2,
                      SLOP_RGB(0x30, 0x36, 0x4a));
        } else {
            const char *msg = "Drop in an image, or open one from Files";
            int mw = slop_text_w(&slop_font_body, msg);
            slop_text(&slop_font_body, cvx + cvw / 2 - mw / 2, cvy + cvh / 2, msg, slop_theme_dark.text_mute);
        }

        /* pan / zoom input over canvas */
        int over = ui.mx >= cvx && ui.mx < cvx + cvw && ui.my >= cvy && ui.my < cvy + cvh;
        if (over && ui.pressed) { dragging = 1; drag_x = ui.mx; drag_y = ui.my; start_pan_x = pan_x; start_pan_y = pan_y; }
        if (dragging) {
            if (!ui.down) dragging = 0;
            else { pan_x = start_pan_x + (ui.mx - drag_x); pan_y = start_pan_y + (ui.my - drag_y); }
        }
        if (over && ui.wheel) {
            float old = zoom;
            zoom *= (ui.wheel > 0) ? 0.85f : 1.18f;
            if (zoom < 0.05f) zoom = 0.05f;
            if (zoom > 20) zoom = 20;
            /* zoom toward pointer */
            pan_x = (int)((pan_x) * (zoom / old));
            pan_y = (int)((pan_y) * (zoom / old));
        }

        /* filmstrip */
        int fs_y = c.y + c.h - 66;
        slop_hline(c.x, fs_y - 6, c.w, slop_theme_dark.border);
        int fx = c.x + 8;
        for (int i = 0; i < nsib && fx < c.x + c.w - 60; i++) {
            int hot = ui.mx >= fx && ui.mx < fx + 54 && ui.my >= fs_y && ui.my < fs_y + 54;
            slop_fill_round(fx, fs_y, 54, 54, 6, i == sib_sel ? slop_theme_dark.accent :
                            hot ? slop_theme_dark.surface_hi : slop_theme_dark.surface_lo);
            char full[2048];
            int fl = snprintf(full, sizeof(full), "%s/%s", dir_path, siblings[i]);
            slop_image thumb;
            if (fl >= 0 && fl < (int)sizeof(full) && slop_image_load(full, &thumb) == 0) {
                float sc = 44.0f / thumb.w; if (thumb.h * sc > 44) sc = 44.0f / thumb.h;
                slop_image_draw_scaled(&thumb, fx + 5, fs_y + 5, (int)(thumb.w * sc), (int)(thumb.h * sc));
                slop_image_free(&thumb);
            }
            if (hot && ui.released) load_index(i);
            fx += 60;
        }

        /* keyboard */
        if (ui.key == SLOP_KEY_LEFT) load_index(sib_sel - 1);
        if (ui.key == SLOP_KEY_RIGHT) load_index(sib_sel + 1);
        if (ui.key == SLOP_KEY_UP) { zoom *= 1.15f; }
        if (ui.key == SLOP_KEY_DOWN) { zoom *= 0.87f; }
        if (ui.ctrl && ui.ctrl_ch == 'r') rotation = (rotation + 90) % 360;

        if (c.close_clicked) break;
        slop_end_frame();
        usleep(6000);
    }
    if (have_img) slop_image_free(&img);
    slop_shutdown();
    return 0;
}
