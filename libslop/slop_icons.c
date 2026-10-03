/* slop_icons.c -- procedural glyphs for the SlopOS icon set. */
#include "slop.h"
#include "slop_icons.h"

const char *slop_icon_name(int icon) {
    switch (icon) {
    case SLOP_ICON_FILES:    return "Files";
    case SLOP_ICON_TERM:     return "Terminal";
    case SLOP_ICON_IMAGE:    return "Image Viewer";
    case SLOP_ICON_VIEW:     return "File Viewer";
    case SLOP_ICON_ZEN:      return "Zen Browser";
    case SLOP_ICON_OBS:      return "OBS Studio";
    case SLOP_ICON_RESOLVE:  return "DaVinci Resolve";
    case SLOP_ICON_ABOUT:    return "System Info";
    case SLOP_ICON_CALC:     return "Calculator";
    case SLOP_ICON_MONITOR:  return "System Monitor";
    case SLOP_ICON_SETTINGS: return "Settings";
    case SLOP_ICON_NOTES:    return "Notes";
    default:                 return "Application";
    }
}

void slop_icon_draw(int icon, int cx, int cy, int s, int hover) {
    int r = s / 2;
    slop_color tile = hover ? slop_theme_dark.surface_hi : slop_theme_dark.surface;
    slop_fill_round(cx - r, cy - r, s, s, s / 5, tile);
    slop_round_outline(cx - r, cy - r, s, s, s / 5, 1, slop_theme_dark.border);

    switch (icon) {
    case SLOP_ICON_FILES: {
        int w = s * 5 / 9, h = s * 4 / 9;
        int x = cx - w / 2, y = cy - h / 2 + 2;
        slop_fill_round(x, y - 4, w / 2, 6, 2, SLOP_RGB(0xf0, 0xb4, 0x3c));
        slop_fill_round(x, y, w, h, 3, SLOP_RGB(0xf0, 0xb4, 0x3c));
        slop_fill_round(x, y, w, h / 2, 3, SLOP_RGB(0xf7, 0xcd, 0x6a));
        break;
    }
    case SLOP_ICON_TERM: {
        int w = s * 3 / 5, h = s * 3 / 5;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 4, SLOP_RGB(0x0a, 0x0c, 0x12));
        slop_round_outline(x, y, w, h, 4, 1, slop_theme_dark.border_hi);
        slop_text(&slop_font_monob, x + 5, y + h / 2 + 2, ">_", slop_theme_dark.good);
        break;
    }
    case SLOP_ICON_IMAGE: {
        int w = s * 3 / 5, h = s * 3 / 5;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 4, SLOP_RGB(0x1d, 0x24, 0x36));
        slop_fill_circle(x + w / 4, y + h / 4, 3, SLOP_RGB(0xf0, 0xb4, 0x3c));
        for (int i = 0; i < w - 8; i++) {
            int hh = (w - 8 - i) / 2;
            slop_vline(x + 4 + i, y + h - 5 - hh, hh, SLOP_RGB(0x4a, 0xd9, 0x9a));
        }
        slop_round_outline(x, y, w, h, 4, 1, slop_theme_dark.border_hi);
        break;
    }
    case SLOP_ICON_VIEW: {
        int w = s * 4 / 9, h = s * 5 / 9;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 3, SLOP_RGB(0xe6, 0xe9, 0xf0));
        for (int i = 0; i < 4; i++)
            slop_fill_rect(x + 4, y + 8 + i * 6, w - 8, 2, SLOP_RGB(0x8a, 0x93, 0xa8));
        break;
    }
    case SLOP_ICON_ZEN: {
        slop_fill_circle(cx, cy, s / 4, SLOP_RGB(0x2b, 0x6c, 0xff));
        slop_circle(cx, cy, s / 4, SLOP_RGB(0x8a, 0xa4, 0xff));
        slop_line(cx - s / 4, cy, cx + s / 4, cy, SLOP_RGB(0xff, 0xff, 0xff));
        slop_line(cx, cy - s / 4, cx, cy + s / 4, SLOP_RGB(0xff, 0xff, 0xff));
        break;
    }
    case SLOP_ICON_OBS: {
        slop_fill_circle(cx, cy, s / 4, SLOP_RGB(0x1b, 0x1e, 0x26));
        slop_circle(cx, cy, s / 4, SLOP_RGB(0x9a, 0xa2, 0xb8));
        slop_fill_circle(cx, cy, s / 8, SLOP_RGB(0xf0, 0x6a, 0x6a));
        break;
    }
    case SLOP_ICON_RESOLVE: {
        int w = s * 3 / 5, h = s * 3 / 5;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 4, SLOP_RGB(0x12, 0x14, 0x1d));
        slop_fill_round(x + 3, y + 3, w / 2 - 4, 5, 2, SLOP_RGB(0x6c, 0x8c, 0xff));
        slop_fill_round(x + 3, y + 10, w / 2 - 4, 5, 2, SLOP_RGB(0x4a, 0xd9, 0x9a));
        slop_fill_round(x + 3, y + 17, w / 2 - 4, 5, 2, SLOP_RGB(0xf0, 0xb4, 0x3c));
        break;
    }
    case SLOP_ICON_ABOUT: {
        slop_fill_circle(cx, cy, s / 4, slop_theme_dark.accent);
        int tw = slop_text_w(&slop_font_bold, "i");
        slop_text(&slop_font_bold, cx - tw / 2, cy + 6, "i", SLOP_RGB(255, 255, 255));
        break;
    }
    case SLOP_ICON_CALC: {
        int w = s * 3 / 5, h = s * 5 / 9;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 4, SLOP_RGB(0x1d, 0x24, 0x36));
        slop_fill_round(x + 4, y + 4, w - 8, 8, 2, SLOP_RGB(0x0a, 0x0c, 0x12));
        for (int gy = 0; gy < 3; gy++)
            for (int gx = 0; gx < 3; gx++)
                slop_fill_round(x + 5 + gx * 7, y + 16 + gy * 6, 5, 4, 1,
                                gy == 2 && gx == 2 ? slop_theme_dark.accent
                                                   : SLOP_RGB(0x6c, 0x8c, 0xff));
        break;
    }
    case SLOP_ICON_MONITOR: {
        int w = s * 3 / 5, h = s * 2 / 5;
        int x = cx - w / 2, y = cy - h / 2 - 2;
        slop_fill_round(x, y, w, h, 3, SLOP_RGB(0x0a, 0x0c, 0x12));
        slop_round_outline(x, y, w, h, 3, 1, slop_theme_dark.border_hi);
        slop_color bars[4] = { slop_theme_dark.good, slop_theme_dark.accent,
                               slop_theme_dark.warn, slop_theme_dark.bad };
        for (int i = 0; i < 4; i++) {
            int bh = 3 + i * 2;
            slop_fill_rect(x + 4 + i * 6, y + h - 3 - bh, 4, bh, bars[i]);
        }
        slop_fill_rect(cx - 2, y + h + 1, 4, 4, slop_theme_dark.text_mute);
        break;
    }
    case SLOP_ICON_SETTINGS: {
        for (int a = 0; a < 360; a += 45) {
            float rad = a * 3.14159265f / 180.0f;
            int x = cx + (int)(__builtin_cosf(rad) * (s / 4));
            int y = cy + (int)(__builtin_sinf(rad) * (s / 4));
            slop_fill_round(x - 2, y - 2, 5, 5, 1, slop_theme_dark.text_dim);
        }
        slop_fill_circle(cx, cy, s / 5, slop_theme_dark.text_dim);
        slop_fill_circle(cx, cy, s / 9, slop_theme_dark.surface);
        break;
    }
    case SLOP_ICON_NOTES: {
        int w = s * 4 / 9, h = s * 5 / 9;
        int x = cx - w / 2, y = cy - h / 2;
        slop_fill_round(x, y, w, h, 3, SLOP_RGB(0xf7, 0xcd, 0x6a));
        for (int i = 0; i < 4; i++)
            slop_fill_rect(x + 4, y + 8 + i * 6, w - 8, 1, SLOP_RGB(0xa8, 0x86, 0x2a));
        break;
    }
    default: break;
    }
}
