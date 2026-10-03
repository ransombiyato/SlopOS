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
make verify-compat   # prove the third-party launch path (dynamic stand-in)
make verify-x11      # prove a real X client renders through a SlopOS window
make clean
```
All userland is linked `-static`; the initramfs must be self-contained.
Exception: `compat/slop-xsession` is dynamically linked (Xlib/XTest), and its
libs are staged by `tools/mkimage.sh` / `tools/stage-dynlibs.py`.

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
- `tools/verify-compat.sh APP` proves the compat *launch* path with a dynamic
  stand-in; it needs `dist/about.png` as a reference (`make shots` first).
- `tools/verify-x11.sh` proves the real X11 path: it autostarts the `x11-demo`
  manifest (a genuine dynamically linked xeyes) and checks the serial log plus
  the screenshot for rendered content. See "X11 compatibility path" below.
- A console is attached (`console=ttyS0`), but the kernel keeps it as a
  non-primary console while the fbcon is primary, so app stderr does not land
  in the serial log. `slop-launch` and `slop-xsession` therefore write their
  own diagnostics directly to `/dev/ttyS0`. Verify launches by framebuffer
  too, not only log scraping.

## X11 compatibility path
The three third-party apps are X11/GL/GTK programs, so the compat runtime
brings up a real X server and bridges it into a normal SlopOS window:

```
Xvfb :1 -screen 0 758x467x24 -fbdir /run/slop/x
      |  (XWD big-endian header, 160 bytes; XRGB8888, bpl = w*4)
slop-xsession  -- mmaps Xvfb_screen0, blits it into its libslop surface,
                  forwards slop_event input back via XTEST, and re-sends the
                  window size after the compositor WELCOME
      |  (compositor unix socket, SLOP_MSG_SURFACE shared memory)
slop-shell     -- composites the surface into a normal window
```
- The shell assigns a fixed 760x500 window, so the X screen matches the 758x467
  content area (760 - 2*BORDER, 500 - TITLEBAR_H - BORDER) for a 1:1 map.
  A larger X screen gets centre-cropped and small clients fall off the edge.
- `compat/xsession.c` is a libslop client, so it is compiled together with
  `libslop/slop.c` rather than linking libslop as a library.
- `slop-launch` relays Xvfb/xsession stderr (`/run/slop/*.err`) to the serial
  log when the display fails to come up.

## Autostart hook (testing)
`slop-shell` reads `/run/slop/autostart`, falling back to
`/usr/share/slop/autostart`. One line: either a dock index (`4`) or a raw
compatibility manifest id (`x11-demo`). Used only by the verify scripts.

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

## Repository / migration state
- Source of truth: `ransombiyato/SlopOS` (default branch `main`), formerly
  `AboIDE`. The repo was renamed and its old IDE content fully replaced by
  SlopOS via a force-push of `master` -> `main` (the previous `main`,
  `6287ac3`, is recoverable from GitHub's reflog if ever needed).
- This workspace (`/workspace/project`) mirrors that history on branch
  `master`; push with `git push origin master:main`.
- Portable full-history archive: `/workspace/slopos-aurora.bundle`
  (`git bundle verify` passes).

