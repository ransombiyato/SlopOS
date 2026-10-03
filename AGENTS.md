# AGENTS.md — SlopOS repository notes

## What this is
A from-scratch x86_64 desktop OS. Linux 6.12 is the kernel/ABI layer; a small
native GUI (libslop) draws to the framebuffer; three third-party apps (Zen
Browser, OBS Studio, DaVinci Resolve) run through a compatibility runtime.

## Golden rule
**Only the three third-party apps use the compatibility layer.** Native apps
(shell, files, term, image, view, about) must never depend on X11, Wayland,
glibc-specific shims, or `compat/`. Keep the two worlds separate.

## Build commands
```sh
make user     # libslop + shell + native apps + compat runtime (fast)
make kernel   # Linux 6.12 bzImage (slow, ~7 min, needs network)
make image    # rootfs + initramfs.cpio.gz
make iso      # dist/slopos.iso (hybrid BIOS+UEFI)
make run      # boot in QEMU
make shots    # boot per app and capture dist/*.png + docs/screenshots.png
make clean
```
All userland is linked `-static`; the initramfs must be self-contained.

## Conventions
- C11, `-Wall -Wextra`, no warnings. `_GNU_SOURCE` is defined per-source, not
  on the command line (avoid the redefine warning).
- Native apps: `slop_init()` → immediate-mode loop (`slop_begin_frame` /
  `slop_end_frame`) → `slop_shutdown()`. Exit on `ui.quit` (Ctrl+Q).
- The shell must never exit on Esc — it is the session. Esc only closes the
  launcher/dialogs.
- Keep the theme in `libslop/slop.c` (`slop_theme_dark`) rather than hard
  -coding colours in apps.

## Testing / verification
- Headless: `python3 tools/screenshot.py OUT.png --wait N [--keys ...] [--uefi]`
  boots QEMU, injects keys, dumps the framebuffer, and prints the serial log.
- The shell maps number keys 1-8 to dock apps in `APPS[]` order:
  1 Files, 2 Terminal, 3 Image, 4 Viewer, 5 Zen, 6 OBS, 7 Resolve, 8 About.
  Use these to automate per-app screenshots.
- `tools/screenshot.py` needs Pillow. UEFI test uses OVMF if present.
- `/dev/pts` must be mounted by `init` or the terminal has no pty.

## Gotchas
- GRUB config must guard BIOS-only modules (`vbe`, `video_bochs`) behind
  `if [ "$grub_platform" != "efi" ]` or UEFI boot fails on `vbe.mod`.
- The kernel source tree (`kernel/linux-6.12`, ~2.4 GB) and `dist/` are
  git-ignored; rebuild with `make kernel`.
- `busybox` applet symlinks: skip the `busybox` entry itself (`ln` errors on
  self-link).
- Font atlases are generated: `libslop/font_data.h` comes from
  `tools/mkfont.py` (needs Pillow + DejaVu fonts).

## Key files
- `init/init.c` — PID 1, mounts, splash.
- `libslop/slop.{h,c}` — the GUI toolkit and theme.
- `shell/shell.c` — desktop; `APPS[]`/`launch()` live here.
- `compat/slop-launch.c` — compat runtime; manifests in `compat/apps/*.app`.
- `kernel/build.sh` — kernel config knobs (DRM, evdev, V4L2, seccomp, …).
- `tools/mkimage.sh`, `tools/mkiso.sh`, `tools/screenshot.py`.
