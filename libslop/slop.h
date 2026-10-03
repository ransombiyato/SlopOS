/*
 * libslop -- the SlopOS native UI toolkit.
 *
 * A small, dependency-free immediate-mode toolkit that draws straight to the
 * Linux framebuffer and reads input straight from evdev. No X11, no Wayland,
 * no GPU driver: just pixels and events. Every native SlopOS app is built on
 * this, which is why they all feel identical and start instantly.
 */
#ifndef SLOP_H
#define SLOP_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ colours */
typedef uint32_t slop_color;
#define SLOP_RGB(r, g, b) (((slop_color)(r) << 16) | ((slop_color)(g) << 8) | (slop_color)(b))

/* -------------------------------------------------------------------- fonts */
typedef struct {
    int8_t  bx, by;   /* offset of bitmap from the pen (baseline origin) */
    uint8_t w, h;     /* bitmap size */
    uint8_t adv;      /* advance width */
    uint16_t gx, gy;  /* position of bitmap inside the atlas */
} slop_glyph;

typedef struct {
    uint8_t            height;   /* ascent + descent */
    uint8_t            ascent;
    uint16_t           atlas_w, atlas_h;
    const unsigned char *atlas;  /* 8-bit coverage */
    const slop_glyph   *glyphs;  /* for ASCII 32..126 */
} slop_font;

/* ------------------------------------------------------------------- images */
typedef struct {
    int      w, h;
    uint8_t *rgba;   /* w*h*4, non-premultiplied */
} slop_image;

/* ------------------------------------------------------------------ display */
typedef struct {
    int      w, h, pitch, bpp;
    size_t   size;
    uint8_t *fb;      /* mapped framebuffer */
    uint32_t *back;   /* back buffer (always 32bpp XRGB) */
    int      fd;
    int      mouse_x, mouse_y;
    int      needs_present;
} slop_display;

extern slop_display slop;

/* ------------------------------------------------------------------- events */
typedef enum {
    SLOP_EV_NONE = 0,
    SLOP_EV_KEY,
    SLOP_EV_MOUSE_MOVE,
    SLOP_EV_MOUSE_DOWN,
    SLOP_EV_MOUSE_UP,
    SLOP_EV_WHEEL,
    SLOP_EV_RESIZE,
    SLOP_EV_QUIT,
} slop_event_type;

/* Modifier bits (event.mods). */
#define SLOP_MOD_SHIFT 1
#define SLOP_MOD_CTRL  2
#define SLOP_MOD_ALT   4

typedef struct {
    slop_event_type type;
    int      x, y;        /* pointer position at event time */
    int      button;      /* 0 left, 1 right, 2 middle */
    int      wheel;       /* +1 down, -1 up */
    uint16_t code;        /* raw evdev keycode */
    char     ch;          /* translated character, 0 if none */
    int      mods;        /* SLOP_MOD_* */
    int      key;         /* logical key: SLOP_KEY_* */
} slop_event;

/* Logical (translated) key codes for non-printable keys. */
enum {
    SLOP_KEY_NONE = 0,
    SLOP_KEY_ENTER = 0x100, SLOP_KEY_ESC, SLOP_KEY_BACKSPACE, SLOP_KEY_TAB,
    SLOP_KEY_UP, SLOP_KEY_DOWN, SLOP_KEY_LEFT, SLOP_KEY_RIGHT,
    SLOP_KEY_HOME, SLOP_KEY_END, SLOP_KEY_PGUP, SLOP_KEY_PGDN,
    SLOP_KEY_DELETE, SLOP_KEY_F1, SLOP_KEY_F2, SLOP_KEY_F3, SLOP_KEY_F4,
    SLOP_KEY_F5, SLOP_KEY_F6, SLOP_KEY_F7, SLOP_KEY_F8, SLOP_KEY_F9,
    SLOP_KEY_F10, SLOP_KEY_F11, SLOP_KEY_F12, SLOP_KEY_SUPER,
};

/* --------------------------------------------------------------- lifecycle */
int  slop_init(void);          /* open /dev/fb0 + input devices */
void slop_shutdown(void);
void slop_present(void);       /* back buffer -> framebuffer */
void slop_clear(slop_color c);

/* ---------------------------------------------------------------- drawing */
void slop_fill_rect(int x, int y, int w, int h, slop_color c);
void slop_fill_rect_a(int x, int y, int w, int h, slop_color c, int alpha);
void slop_rect(int x, int y, int w, int h, slop_color c);          /* 1px outline */
void slop_rect_thick(int x, int y, int w, int h, int t, slop_color c);
void slop_hline(int x, int y, int w, slop_color c);
void slop_vline(int x, int y, int h, slop_color c);
void slop_line(int x0, int y0, int x1, int y1, slop_color c);
void slop_fill_round(int x, int y, int w, int h, int r, slop_color c);
void slop_round_outline(int x, int y, int w, int h, int r, int t, slop_color c);
void slop_circle(int cx, int cy, int r, slop_color c);
void slop_fill_circle(int cx, int cy, int r, slop_color c);
void slop_vgradient(int x, int y, int w, int h, slop_color top, slop_color bot);
void slop_shadow(int x, int y, int w, int h, int spread, int strength);
void slop_blend(int x, int y, slop_color c, int alpha);

/* ---------------------------------------------------------------- text */
void   slop_text(const slop_font *f, int x, int baseline, const char *s, slop_color c);
void   slop_text_n(const slop_font *f, int x, int baseline, const char *s, int n, slop_color c);
int    slop_text_w(const slop_font *f, const char *s);
int    slop_text_wn(const slop_font *f, const char *s, int n);
int    slop_char_w(const slop_font *f, char c);
/* Draw text vertically centred inside [y, y+h). Returns baseline used. */
int    slop_text_vcenter(const slop_font *f, int x, int y, int h, const char *s, slop_color c);

/* ---------------------------------------------------------------- images */
int  slop_image_load(const char *path, slop_image *out);   /* png/jpg/gif/bmp */
void slop_image_free(slop_image *img);
void slop_image_draw(const slop_image *img, int x, int y);
void slop_image_draw_scaled(const slop_image *img, int x, int y, int w, int h);
void slop_blit_rgba(const uint8_t *rgba, int iw, int ih, int x, int y, int alpha);

/* ---------------------------------------------------------------- input */
int  slop_poll(slop_event *e);       /* 1 if an event was produced */
int  slop_wait_event(slop_event *e, int timeout_ms);
void slop_flush_events(void);
void slop_set_cursor(int x, int y);
void slop_draw_cursor(void);         /* draws arrow cursor at pointer */
int  slop_ticks_ms(void);

/* ---------------------------------------------------- immediate-mode frame
 *
 * Apps run a simple loop:
 *     slop_begin_frame();
 *     ... draw + handle widgets ...
 *     slop_end_frame();
 * slop_begin_frame() drains the input devices into `ui` and resets the
 * per-frame flags (pressed/released/wheel/typed). This keeps every app's
 * event handling identical and free of race conditions.
 */
typedef struct {
    int  mx, my;          /* pointer */
    int  down;            /* left button held */
    int  pressed;         /* left button went down this frame */
    int  released;        /* left button went up this frame */
    int  rdown, rpressed; /* right button */
    int  wheel;           /* accumulated wheel this frame (+down) */
    int  shift, ctrl, alt, super;
    char text[64];        /* printable characters typed this frame */
    int  ntext;
    char ctrl_ch;         /* control byte for Ctrl+<letter> (e.g. ^C -> 3) */
    int  key;             /* last logical key pressed this frame (0 none) */
    int  quit;            /* set when the user asks to close (Esc/Ctrl+Q) */
    int  dt_ms;           /* ms since previous frame (for animations) */
} slop_input;

extern slop_input ui;

void slop_begin_frame(void);
void slop_end_frame(void);       /* draws cursor, presents */

/* ---------------------------------------------------------------- widgets */
typedef struct {
    int         x, y, w, h;
    const char *label;
    int         primary;   /* accent filled */
    int         enabled;
    int         hot;       /* hovered (updated by draw) */
    int         held;      /* pressed inside (updated by draw) */
} slop_button;

int  slop_button_draw(slop_button *b);          /* returns 1 when clicked */
int  slop_icon_button(int x, int y, int s, const char *glyph, int active, const char *tip);
int  slop_toolbar_button(int x, int y, int h, const char *label, int active);
void slop_scrollbar(int x, int y, int h, int total, int view, int *offset, int *dragging);
void slop_list_row(int x, int y, int w, int h, const char *label, int selected, int hover);
int  slop_textfield(int x, int y, int w, int h, char *buf, int cap, int *cursor, int *focused, const char *placeholder);
void slop_progress(int x, int y, int w, int h, int pct, slop_color col);
void slop_badge(int x, int y, const char *text, slop_color col);
int  slop_menuitem(int x, int y, int w, int h, const char *label, const char *shortcut);

/* Window chrome: draws a title bar with traffic-light buttons and returns the
 * content rectangle via out_*. */
typedef struct {
    int x, y, w, h;            /* content rect */
    int close_clicked;
} slop_chrome;
slop_chrome slop_window(const char *title, const char *subtitle, int close_hot);

/* ---------------------------------------------------------------- utils */
slop_color slop_mix(slop_color a, slop_color b, int t /*0..255*/);
void       slop_lighten(slop_color *c, int amt);
void       slop_darken(slop_color *c, int amt);
const char *slop_basename(const char *path);
void        slop_human_size(long bytes, char *out, int cap);
const char *slop_ext(const char *path);

/* Fonts (defined in font_data.h). */
extern const slop_font slop_font_small, slop_font_body, slop_font_bold,
                       slop_font_title, slop_font_mono, slop_font_monob;

/* A shared dark theme so every app looks like part of the same system. */
typedef struct {
    slop_color bg, surface, surface_hi, surface_lo, border, border_hi;
    slop_color text, text_dim, text_mute;
    slop_color accent, accent_hi, accent_lo;
    slop_color good, warn, bad;
    slop_color titlebar, titlebar_focus;
} slop_theme;
extern const slop_theme slop_theme_dark;

#endif /* SLOP_H */
