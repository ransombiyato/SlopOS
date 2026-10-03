/* libslop implementation. See slop.h for the contract. */
#define _GNU_SOURCE
#include "slop.h"
#include "font_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb_image.h"

slop_display slop;

/* ================================================================= theme */
const slop_theme slop_theme_dark = {
    .bg          = SLOP_RGB(0x0d, 0x0f, 0x16),
    .surface     = SLOP_RGB(0x16, 0x19, 0x24),
    .surface_hi  = SLOP_RGB(0x1e, 0x22, 0x30),
    .surface_lo  = SLOP_RGB(0x10, 0x12, 0x1b),
    .border      = SLOP_RGB(0x26, 0x2b, 0x3a),
    .border_hi   = SLOP_RGB(0x36, 0x3d, 0x52),
    .text        = SLOP_RGB(0xe6, 0xe9, 0xf0),
    .text_dim    = SLOP_RGB(0x9a, 0xa2, 0xb8),
    .text_mute   = SLOP_RGB(0x5c, 0x64, 0x7a),
    .accent      = SLOP_RGB(0x6c, 0x8c, 0xff),
    .accent_hi   = SLOP_RGB(0x8a, 0xa4, 0xff),
    .accent_lo   = SLOP_RGB(0x44, 0x5e, 0xd8),
    .good        = SLOP_RGB(0x4a, 0xd9, 0x9a),
    .warn        = SLOP_RGB(0xf0, 0xb4, 0x3c),
    .bad         = SLOP_RGB(0xf0, 0x6a, 0x6a),
    .titlebar      = SLOP_RGB(0x12, 0x14, 0x1d),
    .titlebar_focus = SLOP_RGB(0x1a, 0x1e, 0x2b),
};

/* ============================================================ drawing core */
static int clip_x0, clip_y0, clip_x1, clip_y1;

static void clip_reset(void) { clip_x0 = 0; clip_y0 = 0; clip_x1 = slop.w; clip_y1 = slop.h; }
static inline void put(int x, int y, slop_color c) {
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    slop.back[(size_t)y * slop.w + x] = c;
}
static inline void put_a(int x, int y, slop_color c, int a) {
    if (a <= 0) return;
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    uint32_t *p = &slop.back[(size_t)y * slop.w + x];
    if (a >= 255) { *p = c; return; }
    slop_color d = *p;
    int ia = 255 - a;
    int r = (((c >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia) >> 8;
    int g = (((c >> 8) & 0xff) * a + ((d >> 8) & 0xff) * ia) >> 8;
    int b = ((c & 0xff) * a + (d & 0xff) * ia) >> 8;
    *p = SLOP_RGB(r, g, b);
}

slop_color slop_mix(slop_color a, slop_color b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int r = (((a >> 16) & 0xff) * (255 - t) + ((b >> 16) & 0xff) * t) >> 8;
    int g = (((a >> 8) & 0xff) * (255 - t) + ((b >> 8) & 0xff) * t) >> 8;
    int bl = ((a & 0xff) * (255 - t) + (b & 0xff) * t) >> 8;
    return SLOP_RGB(r, g, bl);
}
void slop_lighten(slop_color *c, int amt) { *c = slop_mix(*c, SLOP_RGB(255, 255, 255), amt); }
void slop_darken(slop_color *c, int amt)  { *c = slop_mix(*c, SLOP_RGB(0, 0, 0), amt); }
void slop_blend(int x, int y, slop_color c, int alpha) { put_a(x, y, c, alpha); }

void slop_clear(slop_color c) {
    for (size_t i = 0; i < (size_t)slop.w * slop.h; i++) slop.back[i] = c;
}

void slop_fill_rect(int x, int y, int w, int h, slop_color c) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &slop.back[(size_t)yy * slop.w];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}
void slop_fill_rect_a(int x, int y, int w, int h, slop_color c, int a) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) put_a(xx, yy, c, a);
}
void slop_hline(int x, int y, int w, slop_color c) { slop_fill_rect(x, y, w, 1, c); }
void slop_vline(int x, int y, int h, slop_color c) { slop_fill_rect(x, y, 1, h, c); }

void slop_rect(int x, int y, int w, int h, slop_color c) {
    slop_hline(x, y, w, c);
    slop_hline(x, y + h - 1, w, c);
    slop_vline(x, y, h, c);
    slop_vline(x + w - 1, y, h, c);
}
void slop_rect_thick(int x, int y, int w, int h, int t, slop_color c) {
    for (int i = 0; i < t; i++)
        slop_rect(x + i, y + i, w - 2 * i, h - 2 * i, c);
}

void slop_line(int x0, int y0, int x1, int y1, slop_color c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Filled rounded rectangle with a 1px anti-aliased edge. */
void slop_fill_round(int x, int y, int w, int h, int r, slop_color c) {
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r == 0) { slop_fill_rect(x, y, w, h, c); return; }
    slop_fill_rect(x + r, y, w - 2 * r, h, c);
    slop_fill_rect(x, y + r, r, h - 2 * r, c);
    slop_fill_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int ddx = r - i - 1, ddy = r - j - 1;
            int d2 = ddx * ddx + ddy * ddy;
            if (d2 > r * r) continue;
            int a = 255;
            int edge = r * r - d2;
            if (edge < r) a = 128 + edge * 127 / r;
            put_a(x + i, y + j, c, a);
            put_a(x + w - 1 - i, y + j, c, a);
            put_a(x + i, y + h - 1 - j, c, a);
            put_a(x + w - 1 - i, y + h - 1 - j, c, a);
        }
    }
}
/* Outline only: four edges + four anti-aliased corner arcs. */
void slop_round_outline(int x, int y, int w, int h, int r, int t, slop_color c) {
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    slop_fill_rect(x + r, y, w - 2 * r, t, c);
    slop_fill_rect(x + r, y + h - t, w - 2 * r, t, c);
    slop_fill_rect(x, y + r, t, h - 2 * r, c);
    slop_fill_rect(x + w - t, y + r, t, h - 2 * r, c);
    if (r == 0) return;
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int ddx = r - i - 1, ddy = r - j - 1;
            int d2 = ddx * ddx + ddy * ddy;
            if (d2 > r * r) continue;
            int a = 255;
            int edge = r * r - d2;
            if (edge < r) a = 128 + edge * 127 / r;
            if (i < t || j < t) {
                put_a(x + i, y + j, c, a);
                put_a(x + w - 1 - i, y + j, c, a);
                put_a(x + i, y + h - 1 - j, c, a);
                put_a(x + w - 1 - i, y + h - 1 - j, c, a);
            }
        }
    }
}

void slop_circle(int cx, int cy, int r, slop_color c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put(cx + x, cy + y, c); put(cx + y, cy + x, c);
        put(cx - y, cy + x, c); put(cx - x, cy + y, c);
        put(cx - x, cy - y, c); put(cx - y, cy - x, c);
        put(cx + y, cy - x, c); put(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
}
void slop_fill_circle(int cx, int cy, int r, slop_color c) {
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        slop_fill_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

void slop_vgradient(int x, int y, int w, int h, slop_color top, slop_color bot) {
    for (int j = 0; j < h; j++)
        slop_fill_rect(x, y + j, w, 1, slop_mix(top, bot, h > 1 ? j * 255 / (h - 1) : 0));
}

/* Soft drop shadow: an alpha ramp fading outward from the rect edges. */
void slop_shadow(int x, int y, int w, int h, int spread, int strength) {
    for (int s = 1; s <= spread; s++) {
        int a = strength * (spread - s + 1) / (spread * 2);
        if (a <= 0) continue;
        for (int i = -s; i < w + s; i++) {
            put_a(x + i, y - s, 0, a);
            put_a(x + i, y + h + s - 1, 0, a);
        }
        for (int j = -s; j < h + s; j++) {
            put_a(x - s, y + j, 0, a);
            put_a(x + w + s - 1, y + j, 0, a);
        }
    }
}

/* ================================================================ text */
int slop_char_w(const slop_font *f, char c) {
    if (c < 32 || c > 126) c = '?';
    return f->glyphs[c - 32].adv;
}
int slop_text_wn(const slop_font *f, const char *s, int n) {
    int w = 0;
    for (int i = 0; i < n && s[i]; i++) w += slop_char_w(f, s[i]);
    return w;
}
int slop_text_w(const slop_font *f, const char *s) { return slop_text_wn(f, s, 1 << 30); }

void slop_text_n(const slop_font *f, int x, int baseline, const char *s, int n, slop_color c) {
    int pen = x;
    int cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    for (int i = 0; i < n && s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 32 || ch > 126) ch = '?';
        const slop_glyph *g = &f->glyphs[ch - 32];
        if (g->w && g->h) {
            int gx = pen + g->bx, gy = baseline + g->by;
            for (int yy = 0; yy < g->h; yy++) {
                const unsigned char *srow = &f->atlas[(size_t)(g->gy + yy) * f->atlas_w + g->gx];
                int dy = gy + yy;
                for (int xx = 0; xx < g->w; xx++) {
                    int cov = srow[xx];
                    if (!cov) continue;
                    if (cov >= 255) { put(gx + xx, dy, c); continue; }
                    if (gx + xx < clip_x0 || gx + xx >= clip_x1 || dy < clip_y0 || dy >= clip_y1)
                        continue;
                    uint32_t *p = &slop.back[(size_t)dy * slop.w + (gx + xx)];
                    slop_color d = *p;
                    int ia = 255 - cov;
                    int r = (cr * cov + ((d >> 16) & 0xff) * ia) >> 8;
                    int gg = (cg * cov + ((d >> 8) & 0xff) * ia) >> 8;
                    int b = (cb * cov + (d & 0xff) * ia) >> 8;
                    *p = SLOP_RGB(r, gg, b);
                }
            }
        }
        pen += g->adv;
    }
}
void slop_text(const slop_font *f, int x, int baseline, const char *s, slop_color c) {
    slop_text_n(f, x, baseline, s, 1 << 30, c);
}
int slop_text_vcenter(const slop_font *f, int x, int y, int h, const char *s, slop_color c) {
    int bl = y + (h + f->ascent - (f->height - f->ascent)) / 2;
    slop_text(f, x, bl, s, c);
    return bl;
}

/* ================================================================ images */
int slop_image_load(const char *path, slop_image *out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return -1; }
    unsigned char *buf = malloc(sz);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, sz, fp) != (size_t)sz) { free(buf); fclose(fp); return -1; }
    fclose(fp);
    int w, h, comp;
    stbi_uc *px = stbi_load_from_memory(buf, (int)sz, &w, &h, &comp, 4);
    free(buf);
    if (!px) return -1;
    out->w = w; out->h = h; out->rgba = px;
    return 0;
}
void slop_image_free(slop_image *img) {
    if (img && img->rgba) { stbi_image_free(img->rgba); img->rgba = NULL; }
}
void slop_blit_rgba(const uint8_t *rgba, int iw, int ih, int x, int y, int alpha) {
    for (int j = 0; j < ih; j++) {
        int dy = y + j;
        if (dy < clip_y0 || dy >= clip_y1) continue;
        for (int i = 0; i < iw; i++) {
            const uint8_t *p = &rgba[(size_t)(j * iw + i) * 4];
            int a = p[3] * alpha / 255;
            if (!a) continue;
            put_a(x + i, dy, SLOP_RGB(p[0], p[1], p[2]), a);
        }
    }
}
void slop_image_draw(const slop_image *img, int x, int y) {
    slop_blit_rgba(img->rgba, img->w, img->h, x, y, 255);
}
void slop_image_draw_scaled(const slop_image *img, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int sy = j * img->h / h;
        for (int i = 0; i < w; i++) {
            int sx = i * img->w / w;
            const uint8_t *p = &img->rgba[(size_t)(sy * img->w + sx) * 4];
            put_a(x + i, y + j, SLOP_RGB(p[0], p[1], p[2]), p[3]);
        }
    }
}

/* ================================================================ present */
void slop_present(void) {
    if (!slop.fb) return;
    if (slop.bpp == 32) {
        for (int y = 0; y < slop.h; y++) {
            uint32_t *dst = (uint32_t *)(slop.fb + (size_t)y * slop.pitch);
            uint32_t *src = &slop.back[(size_t)y * slop.w];
            memcpy(dst, src, (size_t)slop.w * 4);
        }
    } else if (slop.bpp == 16) {
        for (int y = 0; y < slop.h; y++) {
            uint16_t *dst = (uint16_t *)(slop.fb + (size_t)y * slop.pitch);
            uint32_t *src = &slop.back[(size_t)y * slop.w];
            for (int x = 0; x < slop.w; x++) {
                uint32_t c = src[x];
                dst[x] = (uint16_t)((((c >> 19) & 0x1f) << 11) | (((c >> 10) & 0x3f) << 5) | ((c >> 3) & 0x1f));
            }
        }
    }
}

/* ================================================================ input */
static int kbd_fd = -1;
static int mouse_fds[4]; static int mouse_n = 0;
static int abs_min_x, abs_max_x, abs_min_y, abs_max_y;
static int shift_down = 0, ctrl_down = 0, alt_down = 0;

static const char keymap[256][2] = {
    [KEY_ESC] = {27, 27},
    [KEY_1] = {'1','!'}, [KEY_2]={'2','@'}, [KEY_3]={'3','#'}, [KEY_4]={'4','$'},
    [KEY_5]={'5','%'}, [KEY_6]={'6','^'}, [KEY_7]={'7','&'}, [KEY_8]={'8','*'},
    [KEY_9]={'9','('}, [KEY_0]={'0',')'}, [KEY_MINUS]={'-','_'}, [KEY_EQUAL]={'=','+'},
    [KEY_BACKSPACE]={8,8}, [KEY_TAB]={9,9},
    [KEY_Q]={'q','Q'},[KEY_W]={'w','W'},[KEY_E]={'e','E'},[KEY_R]={'r','R'},
    [KEY_T]={'t','T'},[KEY_Y]={'y','Y'},[KEY_U]={'u','U'},[KEY_I]={'i','I'},
    [KEY_O]={'o','O'},[KEY_P]={'p','P'},[KEY_LEFTBRACE]={'[','{'},[KEY_RIGHTBRACE]={']','}'},
    [KEY_ENTER]={13,13},
    [KEY_A]={'a','A'},[KEY_S]={'s','S'},[KEY_D]={'d','D'},[KEY_F]={'f','F'},
    [KEY_G]={'g','G'},[KEY_H]={'h','H'},[KEY_J]={'j','J'},[KEY_K]={'k','K'},
    [KEY_L]={'l','L'},[KEY_SEMICOLON]={';',':'},[KEY_APOSTROPHE]={'\'','"'},
    [KEY_GRAVE]={'`','~'},[KEY_BACKSLASH]={'\\','|'},
    [KEY_Z]={'z','Z'},[KEY_X]={'x','X'},[KEY_C]={'c','C'},[KEY_V]={'v','V'},
    [KEY_B]={'b','B'},[KEY_N]={'n','N'},[KEY_M]={'m','M'},
    [KEY_COMMA]={',','<'},[KEY_DOT]={'.','>'},[KEY_SLASH]={'/','?'},
    [KEY_SPACE]={' ',' '},
    [KEY_KP0]={'0',0},[KEY_KP1]={'1',0},[KEY_KP2]={'2',0},[KEY_KP3]={'3',0},
    [KEY_KP4]={'4',0},[KEY_KP5]={'5',0},[KEY_KP6]={'6',0},[KEY_KP7]={'7',0},
    [KEY_KP8]={'8',0},[KEY_KP9]={'9',0},[KEY_KPDOT]={'.',0},
    [KEY_KPMINUS]={'-',0},[KEY_KPPLUS]={'+',0},[KEY_KPENTER]={13,13},
};

static int code_to_logical(uint16_t code) {
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: return SLOP_KEY_ENTER;
    case KEY_ESC: return SLOP_KEY_ESC;
    case KEY_BACKSPACE: return SLOP_KEY_BACKSPACE;
    case KEY_TAB: return SLOP_KEY_TAB;
    case KEY_UP: return SLOP_KEY_UP;
    case KEY_DOWN: return SLOP_KEY_DOWN;
    case KEY_LEFT: return SLOP_KEY_LEFT;
    case KEY_RIGHT: return SLOP_KEY_RIGHT;
    case KEY_HOME: return SLOP_KEY_HOME;
    case KEY_END: return SLOP_KEY_END;
    case KEY_PAGEUP: return SLOP_KEY_PGUP;
    case KEY_PAGEDOWN: return SLOP_KEY_PGDN;
    case KEY_DELETE: return SLOP_KEY_DELETE;
    case KEY_F1: return SLOP_KEY_F1; case KEY_F2: return SLOP_KEY_F2;
    case KEY_F3: return SLOP_KEY_F3; case KEY_F4: return SLOP_KEY_F4;
    case KEY_F5: return SLOP_KEY_F5; case KEY_F6: return SLOP_KEY_F6;
    case KEY_F7: return SLOP_KEY_F7; case KEY_F8: return SLOP_KEY_F8;
    case KEY_F9: return SLOP_KEY_F9; case KEY_F10: return SLOP_KEY_F10;
    case KEY_F11: return SLOP_KEY_F11; case KEY_F12: return SLOP_KEY_F12;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: return SLOP_KEY_SUPER;
    default: return SLOP_KEY_NONE;
    }
}

static int has_bit(const unsigned long *bits, int bit) {
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
}
static int device_is_keyboard(int fd) {
    unsigned long bits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return 0;
    return has_bit(bits, KEY_A) && has_bit(bits, KEY_Z) && has_bit(bits, KEY_SPACE);
}
static int device_is_mouse(int fd) {
    unsigned long bits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return 0;
    if (!(has_bit(bits, BTN_LEFT) || has_bit(bits, BTN_MOUSE))) return 0;
    unsigned long abs[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0 && has_bit(abs, ABS_X)) {
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) {
            abs_min_x = ai.minimum; abs_max_x = ai.maximum;
            if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) {
                abs_min_y = ai.minimum; abs_max_y = ai.maximum;
            }
        }
    }
    return 1;
}

static void open_input_devices(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[320];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (kbd_fd < 0 && device_is_keyboard(fd)) { kbd_fd = fd; continue; }
        if (mouse_n < 4 && device_is_mouse(fd)) { mouse_fds[mouse_n++] = fd; continue; }
        close(fd);
    }
    closedir(d);
}

static int pump_one(slop_event *e) {
    if (kbd_fd >= 0) {
        struct input_event ev;
        while (read(kbd_fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                switch (ev.code) {
                case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift_down = ev.value; continue;
                case KEY_LEFTCTRL: case KEY_RIGHTCTRL: ctrl_down = ev.value; continue;
                case KEY_LEFTALT: case KEY_RIGHTALT: alt_down = ev.value; continue;
                }
                if (ev.value == 1) {
                    e->type = SLOP_EV_KEY;
                    e->x = slop.mouse_x; e->y = slop.mouse_y;
                    e->code = ev.code;
                    e->key = code_to_logical(ev.code);
                    e->mods = (shift_down ? SLOP_MOD_SHIFT : 0) | (ctrl_down ? SLOP_MOD_CTRL : 0)
                            | (alt_down ? SLOP_MOD_ALT : 0);
                    if (ev.code < 256 && keymap[ev.code][0]) {
                        char base = keymap[ev.code][shift_down ? 1 : 0];
                        e->ch = base ? base : keymap[ev.code][0];
                    }
                    return 1;
                }
            }
        }
    }
    for (int i = 0; i < mouse_n; i++) {
        struct input_event ev;
        while (read(mouse_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) slop.mouse_x += ev.value;
                else if (ev.code == REL_Y) slop.mouse_y += ev.value;
                else if (ev.code == REL_WHEEL) {
                    e->type = SLOP_EV_WHEEL;
                    e->wheel = ev.value > 0 ? 1 : -1;
                    e->x = slop.mouse_x; e->y = slop.mouse_y;
                    return 1;
                }
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_X && abs_max_x > abs_min_x)
                    slop.mouse_x = (ev.value - abs_min_x) * slop.w / (abs_max_x - abs_min_x);
                else if (ev.code == ABS_Y && abs_max_y > abs_min_y)
                    slop.mouse_y = (ev.value - abs_min_y) * slop.h / (abs_max_y - abs_min_y);
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                    int btn = ev.code == BTN_LEFT ? 0 : ev.code == BTN_RIGHT ? 1 : 2;
                    e->type = ev.value ? SLOP_EV_MOUSE_DOWN : SLOP_EV_MOUSE_UP;
                    e->button = btn;
                    e->x = slop.mouse_x; e->y = slop.mouse_y;
                    return 1;
                }
            }
        }
    }
    if (slop.mouse_x < 0) slop.mouse_x = 0;
    if (slop.mouse_y < 0) slop.mouse_y = 0;
    if (slop.mouse_x > slop.w - 1) slop.mouse_x = slop.w - 1;
    if (slop.mouse_y > slop.h - 1) slop.mouse_y = slop.h - 1;
    return 0;
}

int slop_poll(slop_event *e) {
    memset(e, 0, sizeof(*e));
    return pump_one(e);
}
int slop_wait_event(slop_event *e, int timeout_ms) {
    if (slop_poll(e)) return 1;
    if (timeout_ms > 0) {
        struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    } else {
        usleep(8000);
    }
    return slop_poll(e);
}
void slop_flush_events(void) { slop_event e; while (slop_poll(&e)) {} }
void slop_set_cursor(int x, int y) { slop.mouse_x = x; slop.mouse_y = y; }

void slop_draw_cursor(void) {
    int x = slop.mouse_x, y = slop.mouse_y;
    static const char *shape[] = {
        "X          ", "XX         ", "X.X        ", "X..X       ",
        "X...X      ", "X....X     ", "X.....X    ", "X......X   ",
        "X.......X  ", "X....XXXXX ", "X..X..X    ", "X.X X..X   ",
        "XX  X..X   ", "X    X..X  ", "     X..X  ", "      XX   ",
    };
    for (int j = 0; j < 16; j++)
        for (int i = 0; shape[j][i]; i++)
            if (shape[j][i] == 'X') put(x + i, y + j, SLOP_RGB(0, 0, 0));
    for (int j = 0; j < 16; j++)
        for (int i = 0; shape[j][i]; i++)
            if (shape[j][i] == '.') put(x + i, y + j, SLOP_RGB(255, 255, 255));
}

int slop_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ================================================================ lifecycle */
int slop_init(void) {
    memset(&slop, 0, sizeof(slop));
    kbd_fd = -1; mouse_n = 0;
    slop.fd = open("/dev/fb0", O_RDWR);
    if (slop.fd < 0) { fprintf(stderr, "slop: /dev/fb0: %s\n", strerror(errno)); return -1; }
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(slop.fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(slop.fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        fprintf(stderr, "slop: fb ioctl failed\n"); return -1;
    }
    slop.w = vi.xres; slop.h = vi.yres; slop.bpp = vi.bits_per_pixel;
    slop.pitch = fi.line_length ? fi.line_length : vi.xres * (vi.bits_per_pixel / 8);
    slop.size = (size_t)slop.pitch * vi.yres;
    slop.fb = mmap(NULL, slop.size, PROT_READ | PROT_WRITE, MAP_SHARED, slop.fd, 0);
    if (slop.fb == MAP_FAILED) { fprintf(stderr, "slop: mmap fb failed\n"); return -1; }
    slop.back = calloc((size_t)slop.w * slop.h, 4);
    if (!slop.back) return -1;
    slop.mouse_x = slop.w / 2; slop.mouse_y = slop.h / 2;
    clip_reset();
    open_input_devices();
    return 0;
}
void slop_shutdown(void) {
    if (slop.back) { free(slop.back); slop.back = NULL; }
    if (slop.fb && slop.fb != MAP_FAILED) munmap(slop.fb, slop.size);
    if (slop.fd >= 0) close(slop.fd);
    if (kbd_fd >= 0) close(kbd_fd);
    for (int i = 0; i < mouse_n; i++) close(mouse_fds[i]);
}

/* ======================================================== immediate frame */
slop_input ui;
static int prev_left_down = 0, prev_right_down = 0;
static int last_frame_ms = 0;

void slop_begin_frame(void) {
    int now = slop_ticks_ms();
    ui.dt_ms = last_frame_ms ? now - last_frame_ms : 16;
    if (ui.dt_ms > 200) ui.dt_ms = 200;
    last_frame_ms = now;

    ui.pressed = ui.released = ui.rpressed = 0;
    ui.wheel = 0;
    ui.ntext = 0;
    ui.ctrl_ch = 0;
    ui.key = 0;
    ui.shift = ui.ctrl = ui.alt = ui.super = 0;

    slop_event e;
    while (slop_poll(&e)) {
        switch (e.type) {
        case SLOP_EV_MOUSE_DOWN:
            if (e.button == 0) ui.down = 1;
            else if (e.button == 1) ui.rdown = 1;
            break;
        case SLOP_EV_MOUSE_UP:
            if (e.button == 0) ui.down = 0;
            else if (e.button == 1) ui.rdown = 0;
            break;
        case SLOP_EV_WHEEL: ui.wheel += e.wheel; break;
        case SLOP_EV_KEY:
            ui.shift |= (e.mods & SLOP_MOD_SHIFT) != 0;
            ui.ctrl  |= (e.mods & SLOP_MOD_CTRL) != 0;
            ui.alt   |= (e.mods & SLOP_MOD_ALT) != 0;
            if (e.key) ui.key = e.key;
            if (e.ch >= 32 && e.ch < 127 && ui.ntext < (int)sizeof(ui.text) - 1) {
                if (!(e.mods & SLOP_MOD_CTRL)) ui.text[ui.ntext++] = e.ch;
            }
            if ((e.mods & SLOP_MOD_CTRL) && e.ch) {
                char lower = e.ch;
                if (lower >= 'A' && lower <= 'Z') lower += 32;
                if (lower >= 'a' && lower <= 'z') ui.ctrl_ch = lower - 'a' + 1;
                else if (lower == '[') ui.ctrl_ch = 27;
                else if (lower == '\\') ui.ctrl_ch = 28;
                else if (lower == ']') ui.ctrl_ch = 29;
                else if (lower == ' ') ui.ctrl_ch = 0;
            }
            if (e.key == SLOP_KEY_ESC || ((e.mods & SLOP_MOD_CTRL) && (e.ch == 'q' || e.ch == 'Q')))
                ui.quit = 1;
            break;
        default: break;
        }
    }
    if (ui.down && !prev_left_down) ui.pressed = 1;
    if (!ui.down && prev_left_down) ui.released = 1;
    if (ui.rdown && !prev_right_down) ui.rpressed = 1;
    prev_left_down = ui.down;
    prev_right_down = ui.rdown;
    ui.mx = slop.mouse_x;
    ui.my = slop.mouse_y;
    clip_reset();
}

void slop_end_frame(void) {
    slop_draw_cursor();
    slop_present();
}

/* ============================================================== widgets */
static int in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

int slop_button_draw(slop_button *b) {
    int hot = in_rect(ui.mx, ui.my, b->x, b->y, b->w, b->h);
    int clicked = 0;
    b->hot = hot;
    if (!b->enabled) {
        slop_fill_round(b->x, b->y, b->w, b->h, 8, slop_theme_dark.surface_lo);
        slop_round_outline(b->x, b->y, b->w, b->h, 8, 1, slop_theme_dark.border);
        slop_text_vcenter(&slop_font_bold, b->x + b->w / 2 - slop_text_w(&slop_font_bold, b->label) / 2,
                          b->y, b->h, b->label, slop_theme_dark.text_mute);
        return 0;
    }
    if (hot && ui.pressed) b->held = 1;
    if (ui.released) {
        if (b->held && hot) clicked = 1;
        b->held = 0;
    }
    slop_color base = b->primary ? slop_theme_dark.accent : slop_theme_dark.surface_hi;
    if (b->held && hot) base = b->primary ? slop_theme_dark.accent_lo : slop_theme_dark.surface_lo;
    else if (hot) base = b->primary ? slop_theme_dark.accent_hi : slop_theme_dark.border_hi;
    slop_fill_round(b->x, b->y, b->w, b->h, 8, base);
    if (!b->primary)
        slop_round_outline(b->x, b->y, b->w, b->h, 8, 1, slop_theme_dark.border_hi);
    slop_color tc = b->primary ? SLOP_RGB(0xff, 0xff, 0xff) : slop_theme_dark.text;
    int tw = slop_text_w(&slop_font_bold, b->label);
    slop_text_vcenter(&slop_font_bold, b->x + (b->w - tw) / 2, b->y, b->h, b->label, tc);
    return clicked;
}

int slop_icon_button(int x, int y, int s, const char *glyph, int active, const char *tip) {
    (void)tip;
    int hot = in_rect(ui.mx, ui.my, x, y, s, s);
    int clicked = 0;
    if (hot && ui.released) clicked = 1;
    if (active || hot)
        slop_fill_round(x, y, s, s, 7, active ? slop_theme_dark.accent : slop_theme_dark.surface_hi);
    int tw = slop_text_w(&slop_font_bold, glyph);
    slop_text_vcenter(&slop_font_bold, x + (s - tw) / 2, y, s, glyph,
                      active ? SLOP_RGB(255, 255, 255) : slop_theme_dark.text_dim);
    return clicked;
}

int slop_toolbar_button(int x, int y, int h, const char *label, int active) {
    int tw = slop_text_w(&slop_font_body, label);
    int w = tw + 22;
    int hot = in_rect(ui.mx, ui.my, x, y, w, h);
    int clicked = 0;
    if (hot && ui.released) clicked = 1;
    if (active) slop_fill_round(x, y, w, h, 7, slop_theme_dark.accent);
    else if (hot) slop_fill_round(x, y, w, h, 7, slop_theme_dark.surface_hi);
    slop_text_vcenter(&slop_font_body, x + 11, y, h, label,
                      active ? SLOP_RGB(255, 255, 255) : slop_theme_dark.text_dim);
    return clicked;
}

void slop_scrollbar(int x, int y, int h, int total, int view, int *offset, int *dragging) {
    if (total <= view || view <= 0) return;
    int track_w = 10;
    slop_fill_round(x, y, track_w, h, 5, slop_theme_dark.surface_lo);
    int thumb_h = h * view / total;
    if (thumb_h < 28) thumb_h = 28;
    if (thumb_h > h) thumb_h = h;
    int max_off = total - view;
    int thumb_y = y + (*offset) * (h - thumb_h) / (max_off ? max_off : 1);
    static int grab_dy = 0;
    int hot = in_rect(ui.mx, ui.my, x, y, track_w, h);
    if (hot && ui.pressed && in_rect(ui.mx, ui.my, x, thumb_y, track_w, thumb_h)) {
        *dragging = 1;
        grab_dy = ui.my - thumb_y;
    }
    if (*dragging) {
        if (!ui.down) *dragging = 0;
        else {
            int ty = ui.my - grab_dy;
            int span = h - thumb_h;
            *offset = span > 0 ? (ty - y) * max_off / span : 0;
            if (*offset < 0) *offset = 0;
            if (*offset > max_off) *offset = max_off;
            thumb_y = y + (*offset) * (h - thumb_h) / (max_off ? max_off : 1);
        }
    }
    slop_color tc = (*dragging || hot) ? slop_theme_dark.border_hi : slop_theme_dark.text_mute;
    slop_fill_round(x, thumb_y, track_w, thumb_h, 5, tc);
}

void slop_list_row(int x, int y, int w, int h, const char *label, int selected, int hover) {
    if (selected) slop_fill_round(x, y, w, h, 6, slop_theme_dark.accent_lo);
    else if (hover) slop_fill_round(x, y, w, h, 6, slop_theme_dark.surface_hi);
    slop_text_vcenter(&slop_font_body, x + 12, y, h, label,
                      selected ? SLOP_RGB(255, 255, 255) : slop_theme_dark.text);
}

int slop_textfield(int x, int y, int w, int h, char *buf, int cap, int *cursor, int *focused, const char *placeholder) {
    int hot = in_rect(ui.mx, ui.my, x, y, w, h);
    if (ui.pressed) *focused = hot;
    int len = (int)strlen(buf);
    if (*cursor > len) *cursor = len;
    if (*focused) {
        for (int i = 0; i < ui.ntext; i++) {
            if (len < cap - 1) {
                memmove(buf + *cursor + 1, buf + *cursor, len - *cursor + 1);
                buf[*cursor] = ui.text[i];
                (*cursor)++; len++;
            }
        }
        if (ui.key == SLOP_KEY_BACKSPACE && *cursor > 0) {
            memmove(buf + *cursor - 1, buf + *cursor, len - *cursor + 1);
            (*cursor)--; len--;
        }
        if (ui.key == SLOP_KEY_DELETE && *cursor < len) {
            memmove(buf + *cursor, buf + *cursor + 1, len - *cursor);
            len--;
        }
        if (ui.key == SLOP_KEY_LEFT && *cursor > 0) (*cursor)--;
        if (ui.key == SLOP_KEY_RIGHT && *cursor < len) (*cursor)++;
        if (ui.key == SLOP_KEY_HOME) *cursor = 0;
        if (ui.key == SLOP_KEY_END) *cursor = len;
    }
    slop_fill_round(x, y, w, h, 7, slop_theme_dark.surface_lo);
    slop_round_outline(x, y, w, h, 7, 1, *focused ? slop_theme_dark.accent : slop_theme_dark.border);
    int bl = y + (h + slop_font_body.ascent - (slop_font_body.height - slop_font_body.ascent)) / 2;
    if (len == 0 && !*focused)
        slop_text(&slop_font_body, x + 10, bl, placeholder, slop_theme_dark.text_mute);
    else
        slop_text(&slop_font_body, x + 10, bl, buf, slop_theme_dark.text);
    if (*focused && ((slop_ticks_ms() / 500) & 1)) {
        int cx = x + 10 + slop_text_wn(&slop_font_body, buf, *cursor);
        slop_vline(cx, y + 4, h - 8, slop_theme_dark.accent);
    }
    return hot;
}

void slop_progress(int x, int y, int w, int h, int pct, slop_color col) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    slop_fill_round(x, y, w, h, h / 2, slop_theme_dark.surface_lo);
    int fw = w * pct / 100;
    if (fw >= h) slop_fill_round(x, y, fw, h, h / 2, col);
    else if (fw > 0) slop_fill_round(x, y, h, h, h / 2, col);
}

void slop_badge(int x, int y, const char *text, slop_color col) {
    int tw = slop_text_w(&slop_font_small, text);
    int w = tw + 14, h = 18;
    slop_fill_round(x, y, w, h, 9, col);
    slop_text_vcenter(&slop_font_small, x + 7, y, h, text, SLOP_RGB(0x0d, 0x0f, 0x16));
}

int slop_menuitem(int x, int y, int w, int h, const char *label, const char *shortcut) {
    int hot = in_rect(ui.mx, ui.my, x, y, w, h);
    int clicked = 0;
    if (hot && ui.released) clicked = 1;
    if (hot) slop_fill_round(x + 4, y, w - 8, h, 6, slop_theme_dark.accent);
    slop_text_vcenter(&slop_font_body, x + 14, y, h, label,
                      hot ? SLOP_RGB(255, 255, 255) : slop_theme_dark.text);
    if (shortcut && *shortcut) {
        int sw = slop_text_w(&slop_font_small, shortcut);
        slop_text_vcenter(&slop_font_small, x + w - 14 - sw, y, h, shortcut,
                          hot ? SLOP_RGB(220, 226, 255) : slop_theme_dark.text_mute);
    }
    return clicked;
}

slop_chrome slop_window(const char *title, const char *subtitle, int close_hot) {
    slop_chrome c = {0};
    int pad = 24;
    c.x = pad; c.y = pad;
    c.w = slop.w - 2 * pad; c.h = slop.h - 2 * pad;
    /* backdrop */
    slop_fill_round(c.x - 6, c.y - 6, c.w + 12, c.h + 12, 18, slop_theme_dark.titlebar_focus);
    slop_round_outline(c.x - 6, c.y - 6, c.w + 12, c.h + 12, 18, 1, slop_theme_dark.border_hi);
    /* traffic lights */
    int bx = c.x + 8, by = c.y + 8;
    int hot_red = in_rect(ui.mx, ui.my, bx, by, 26, 26);
    slop_fill_circle(bx + 13, by + 13, 7, hot_red ? SLOP_RGB(0xff, 0x5f, 0x57) : SLOP_RGB(0xff, 0x5f, 0x57));
    slop_fill_circle(bx + 37, by + 13, 7, SLOP_RGB(0xfe, 0xbc, 0x2e));
    slop_fill_circle(bx + 61, by + 13, 7, SLOP_RGB(0x28, 0xc8, 0x40));
    if (hot_red && ui.released) c.close_clicked = 1;
    /* title */
    int tw = slop_text_w(&slop_font_bold, title);
    slop_text(&slop_font_bold, c.x + c.w / 2 - tw / 2, c.y + 20, title, slop_theme_dark.text);
    if (subtitle && *subtitle) {
        int sw = slop_text_w(&slop_font_small, subtitle);
        slop_text(&slop_font_small, c.x + c.w / 2 - sw / 2, c.y + 36, subtitle, slop_theme_dark.text_mute);
    }
    slop_hline(c.x, c.y + 48, c.w, slop_theme_dark.border);
    c.y += 48;
    c.h -= 48;
    (void)close_hot;
    return c;
}

/* ================================================================ utils */
const char *slop_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}
const char *slop_ext(const char *path) {
    const char *base = slop_basename(path);
    const char *d = strrchr(base, '.');
    return d ? d + 1 : "";
}
void slop_human_size(long bytes, char *out, int cap) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, cap, "%ld %s", bytes, u[i]);
    else snprintf(out, cap, "%.1f %s", v, u[i]);
}
