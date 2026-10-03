# SlopOS architecture

SlopOS is a compact x86_64 desktop built in layers. Each layer has one job,
and the boundary between the native and compatibility worlds is explicit.

## Layers

```
   apps        files, term, image, view, about, open        (native)
   shell       panel, dock, launcher, toasts, app spawning
   libslop     framebuffer, drawing, fonts, widgets, input
   init        PID 1: mount pseudo-fs, create devices, exec the shell
   kernel      Linux 6.12 x86_64 (drivers, scheduler, syscalls, DRM)
   firmware    BIOS / UEFI -> GRUB -> bzImage + initramfs
```

### Kernel

We build mainline Linux 6.12 for x86_64. Using Linux as the kernel layer
means SlopOS inherits mature drivers (virtio, AHCI, NVMe, USB HID, DRM/KMS),
a correct scheduler, and — crucially — the exact syscall ABI the three
third-party apps were compiled against. `kernel/build.sh` runs `olddefconfig`
on `x86_64_defconfig` with a small SlopOS fragment and produces
`kernel/out/bzImage`.

### init (PID 1)

`init/init.c` is the first userspace process. It:

1. mounts `proc`, `sysfs`, `devtmpfs`, `devpts`, `tmpfs` on `/tmp`, `/run`,
   `/dev/shm`;
2. creates the standard device nodes and the `/home/user` skeleton;
3. shows the boot splash (logo, progress, status line);
4. switches the console to graphics, clears the screen, and `exec`s
   `/usr/bin/slop-shell`.

`devpts` matters: without it the terminal cannot allocate a pseudo-terminal.

### libslop

The whole native GUI is one small C library. It opens `/dev/fb0`, memory-maps
it, and exposes an immediate-mode API:

* drawing: `slop_fill_rect`, `slop_fill_round`, `slop_round_outline`,
  `slop_circle`, `slop_vgradient`, `slop_shadow`, alpha blending;
* text: anti-aliased bitmaps generated from DejaVu by `tools/mkfont.py`
  (`small`, `body`, `bold`, `title`, `mono`, `monob`);
* widgets: window chrome, buttons, toolbars, text fields, scrollbars,
  progress bars, badges, toasts;
* input: reads `/dev/input/event*` (evdev) for keyboard, mouse and wheel, and
  tracks shift/ctrl/alt/super, text and `Ctrl+<key>`.

Rendering is immediate-mode with a single back buffer; `slop_end_frame()`
blits it to the framebuffer. Because everything is local memory, frames are
cheap and never tear.

### shell

`shell/shell.c` is the session. It draws the top panel (menu, live clock and
date, battery/wifi status), the dock, the launcher grid and toasts, and it
spawns apps with `fork`/`execl`, waiting for each child to exit before
returning to the desktop. Only one app owns the screen at a time — a
deliberate simplification that keeps the native world free of a compositor.

Launching shows a splash first, so a slow-starting third-party app never
looks like a hang.

## The compatibility world

This is the part that makes Zen, OBS and Resolve possible, and it applies to
**those apps only** — native SlopOS apps never touch it.

Third-party apps are normal Linux ELF programs. They need:

* glibc and a pile of shared libraries;
* an X11 (or Wayland) display;
* GPU access (OpenGL / Vulkan / OpenCL) and video devices;
* audio (PipeWire / ALSA);
* specific kernel facilities (seccomp, namespaces, memfd, eventfd, epoll,
  inotify, shared memory, …).

`compat/slop-launch.c` is a small launcher that formalises those needs as a
per-app **manifest** (`compat/apps/*.app`). At launch it:

1. parses the manifest;
2. **probes** every kernel feature the app declares (allocating a real
   `memfd`, opening an `eventfd`, an `epoll`, an `inotify`, an `AF_UNIX` and
   an `AF_INET` socket, checking `/dev/dri`, `/dev/snd`, `/proc/filesystems`,
   …);
3. counts how many of the declared libraries resolve from the host rootfs;
4. applies the app's **profile** (`desktop-graphical`, `media-graphical`,
   `workstation-gpu`): sets `XDG_RUNTIME_DIR`, `DISPLAY`, toolkit backends
   (`GDK_BACKEND`, `QT_QPA_PLATFORM`), audio endpoints, OpenCL/VA-API vars,
   and creates the app's data directory;
5. `exec`s the first declared launcher that exists.

If no launcher is found it draws a **readiness screen** listing the probed
features, the library count, the paths it looked in, and an install hint —
instead of silently failing.

Because it checks real kernel features rather than guessing, the readiness
screen is a genuine compatibility report: on a correctly configured SlopOS
kernel every declared feature shows green.

### Profiles

| Profile | Used by | Grants |
|---------|---------|--------|
| `desktop-graphical` | Zen Browser | X11 GTK, EGL, sandbox syscalls |
| `media-graphical` | OBS Studio | + OpenGL/EGL, PipeWire/ALSA, V4L2, render node |
| `workstation-gpu` | DaVinci Resolve | + Qt/xcb, OpenCL, DRM render node, RT sched |

### Running the real apps

The runtime is complete; what is missing on a fresh ISO is the apps
themselves. Install them onto the rootfs and the same manifests launch them:

```sh
# inside the built rootfs, or on a running SlopOS with a writable /opt
tar -C dist/rootfs/opt/zen -xf zen-browser.tar.xz
tar -C dist/rootfs/opt/obs -xf obs-studio.tar.xz
tar -C dist/rootfs/opt/davinci -xf davinci-resolve.run
make iso
```

See [../compat/README.md](../compat/README.md) for details, including the
shared libraries each app expects.

## Boot flow

```
BIOS/UEFI
  -> GRUB (grub.cfg, gfxpayload=keep keeps the framebuffer)
     -> bzImage + initramfs.cpio.gz
        -> /init (PID 1)
           -> mount pseudo-fs, splash
              -> /usr/bin/slop-shell
                 -> dock / launcher -> native apps | compat runtime
```

## Why this shape

* **No display server for native apps.** It keeps the desktop small, fast and
  tear-free, and means the GUI code is ours end to end.
* **Compatibility is opt-in and per-app.** Only the three third-party apps
  pay for it, exactly as asked. Native apps stay simple.
* **Honesty over faking.** The runtime reports what it can and cannot do;
  it never pretends an app is installed.
