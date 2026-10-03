/*
 * slop_icons.h -- the SlopOS icon set.
 *
 * Icons are drawn procedurally (no image assets) so they stay crisp at any
 * size and cost nothing on the initramfs. The shell's dock and launcher and
 * the apps' title bars all use this one set, which is a big part of why the
 * desktop looks like one system rather than a pile of programs.
 */
#ifndef SLOP_ICONS_H
#define SLOP_ICONS_H

enum {
    SLOP_ICON_FILES = 0,
    SLOP_ICON_TERM,
    SLOP_ICON_IMAGE,
    SLOP_ICON_VIEW,
    SLOP_ICON_ZEN,
    SLOP_ICON_OBS,
    SLOP_ICON_RESOLVE,
    SLOP_ICON_ABOUT,
    SLOP_ICON_CALC,
    SLOP_ICON_MONITOR,
    SLOP_ICON_SETTINGS,
    SLOP_ICON_NOTES,
    SLOP_ICON_COUNT
};

/* The human-readable name for an icon id (used by the task switcher and
 * notifications). */
const char *slop_icon_name(int icon);

/* Draw icon `id` centred at (cx, cy) with a box of `s` pixels. When `hover`
 * is set the tile brightens. */
void slop_icon_draw(int id, int cx, int cy, int s, int hover);

#endif /* SLOP_ICONS_H */
