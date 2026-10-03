# SlopOS compatibility runtime

This directory contains the **only** compatibility layer in SlopOS, and it
exists for exactly three programs: **Zen Browser**, **OBS Studio** and
**DaVinci Resolve**. Native SlopOS apps do not use it.

## Why it is needed

Those three apps are ordinary Linux/ELF programs. They were built against
glibc and a full desktop stack, so they expect:

* shared libraries (`libgtk-3`, `libX11`, `libGL`, `libasound`, `libnss3`, …);
* a display server (X11 / Wayland);
* GPU and media devices (`/dev/dri`, `/dev/video*`, `/dev/snd`);
* kernel facilities (`seccomp`, namespaces, `memfd`, `eventfd`, `epoll`,
  `inotify`, POSIX shared memory, …).

Reimplementing all of that natively would be pointless. Instead, SlopOS runs
them through `slop-launch`, which verifies their requirements and then execs
them on top of the Linux kernel layer.

## Manifest format

Each app has a text manifest in `apps/<id>.app`:

```
name         = Zen Browser          # display name
id           = zen                  # manifest id
icon         = zen                  # dock icon
runtime      = compat
exec         = /opt/zen/zen         # launchers, tried in order
exec         = /usr/bin/zen-browser
profile      = desktop-graphical    # environment profile
needs_libs   = libc.so.6, libgtk-3.so.0, ...      # host libraries
needs_kernel = seccomp, namespaces, memfd, ...    # kernel features
needs_dev    = /dev/dri/renderD128, /dev/snd      # device nodes
min_ram_mb   = 2048
min_cores    = 4
data_dir     = /home/user/.mozilla/zen
```

`slop-launch <name> <id>`:

1. parses the manifest;
2. probes every `needs_kernel` feature for real (it allocates a `memfd`,
   opens `eventfd`/`epoll`/`inotify`, opens `AF_UNIX` and `AF_INET` sockets,
   stats `/dev/dri`, `/dev/snd`, `/dev/video0`, checks
   `/proc/sys/kernel/seccomp/actions_avail`, …);
3. counts how many `needs_libs` resolve from `/lib`, `/usr/lib`, `/lib64`;
4. applies the `profile` (see below);
5. `exec`s the first `exec` path that is executable.

If nothing is installed it shows the readiness screen — the same checks,
plus the list of paths it looked in — so you can see exactly what is missing.

## Profiles

| Profile | For | Environment it sets |
|---------|-----|--------------------|
| `desktop-graphical` | Zen Browser | `GDK_BACKEND=x11`, `MOZ_X11_EGL=1`, `XDG_RUNTIME_DIR` |
| `media-graphical` | OBS Studio | GTK/X11 + `PULSE_SERVER`, `PIPEWIRE_RUNTIME_DIR`, `OBS_USE_EGL` |
| `workstation-gpu` | DaVinci Resolve | `QT_QPA_PLATFORM=xcb`, `OCL_ICD_VENDORS`, `LIBVA_DRIVER_NAME`, `RESOLVE_GPU_MODE` |

All profiles also set `HOME=/home/user`, a `PATH` that includes `/opt/*`, and
create the app's `data_dir`.

## Installing the apps

The runtime ships ready; the apps themselves are not redistributable, so
install them onto the rootfs yourself.

### Zen Browser

```sh
mkdir -p dist/rootfs/opt/zen
tar -C dist/rootfs/opt/zen -xf zen.linux-x86_64.tar.xz
# or point the manifest at a flatpak and add flatpak to the image
```

Needs at least: `libc6`, `libgtk-3-0`, `libdbus-glib-1-2`, `libasound2`,
`libx11-xcb1`, `libxcomposite1`, `libxdamage1`, `libxrandr2`, `libnss3`,
`libnspr4`.

### OBS Studio

```sh
mkdir -p dist/rootfs/opt/obs
tar -C dist/rootfs/opt/obs -xf obs-studio.tar.gz
```

Needs `libgtk-3-0`, `libgl1`, `libegl1`, `libgbm1`, `libasound2`,
`libpipewire-0.3-0`, `libpulse0`, FFmpeg (`libavcodec59` …), `libudev1`,
`libv4l-0`. Add the user to the `video` and `audio` groups for device access.

### DaVinci Resolve

```sh
mkdir -p dist/rootfs/opt/davinci
sh DaVinci_Resolve_*.run --target dist/rootfs/opt/davinci --noexec --nox11
```

The heaviest case: Qt5, OpenCL (`ocl-icd-libopencl1` + a vendor ICD),
`libGL`, `libGLU`, `libX11`/`xcb` stack, and 2 GB+ RAM. If the GPU probe
fails it falls back to software rendering; `RESOLVE_GPU_MODE=auto` lets the
runtime pick.

## The dynamic runtime

Native SlopOS apps are statically linked. Third-party apps are not: they are
ordinary dynamically linked ELF binaries that begin with

```
/lib64/ld-linux-x86-64.so.2
```

so the image ships a glibc runtime for them (`tools/mkimage.sh` copies
`ld-linux-x86-64.so.2` plus `libc/libm/libdl/libpthread/librt/libgcc_s/...`
into the rootfs). Nothing native depends on it; it exists only so the
compatibility world can `exec` real programs.

## Verifying the launch path without the apps

`tools/verify-compat.sh <zen|obs|resolve>` proves the whole chain without the
(non-redistributable) apps. It installs a tiny **dynamically linked** stand-in
at the app's declared path; the stand-in only proceeds if `slop-launch` applied
the graphical profile, and then execs a native app we can see. A run therefore
proves, in order:

1. the manifest was parsed;
2. the kernel features were probed;
3. the dynamic binary was detected and exec'd through the staged glibc loader;
4. the profile environment reached the child.

```sh
make verify-compat        # all three
./tools/verify-compat.sh zen
```

A pass means the frame is pixel-identical to the About screen (the stand-in
exec'd it); a fail leaves the readiness screen on screen.

The readiness screen itself is a real report: every kernel feature that SlopOS
supports shows a green dot, and the library counter reflects the host rootfs.
