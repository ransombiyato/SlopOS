#!/usr/bin/env bash
# Assemble the SlopOS root filesystem and pack it into an initramfs.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT=dist/rootfs
BUSYBOX=/usr/bin/busybox

echo ">> staging rootfs"
mkdir -p "$ROOT"/{bin,usr/bin,sbin,etc,proc,sys,dev,tmp,run,home/user}
mkdir -p "$ROOT"/home/user/{Desktop,Documents,Pictures,Downloads,Music,Videos}
mkdir -p "$ROOT"/usr/share/slop/compat
mkdir -p "$ROOT"/opt/{zen,obs,davinci}
mkdir -p "$ROOT"/etc/OpenCL/vendors

# init and native apps are placed by make; verify they exist
for f in init usr/bin/slop-shell usr/bin/slop-files usr/bin/slop-term \
         usr/bin/slop-image usr/bin/slop-view usr/bin/slop-about \
         usr/bin/slop-open usr/bin/slop-launch usr/bin/slop-xsession usr/bin/slop-setup; do
  [ -x "$ROOT/$f" ] || { echo "!! missing $ROOT/$f (run 'make user')"; exit 1; }
done

# The installer is a POSIX shell script: it unpacks app packages with busybox,
# so installing needs no native helper.
install -m 0755 apps/slop-install.sh "$ROOT/usr/bin/slop-install"

# busybox provides the classic unix userland that third-party apps and shells
# expect (ls, cat, grep, mount, ...). Our own apps are separate binaries.
if [ -x "$BUSYBOX" ]; then
  cp -f "$BUSYBOX" "$ROOT/bin/busybox"
  ( cd "$ROOT/bin" && for applet in $(./busybox --list); do
      [ "$applet" = busybox ] || ln -sf busybox "$applet"
    done )
  cp -f "$ROOT/bin/sh" "$ROOT/usr/bin/sh" 2>/dev/null || true
else
  echo "!! busybox not found; install busybox-static"
fi

# our binaries also live under /bin for convenience
for b in slop-shell slop-files slop-term slop-image slop-view slop-about slop-open slop-launch; do
  ln -sf "/usr/bin/$b" "$ROOT/bin/$b"
done

# Dynamic runtime for third-party apps. Native SlopOS apps are static, but
# Zen/OBS/Resolve are ordinary dynamically linked ELF programs and need a
# glibc loader and friends on the image. This is part of the compatibility
# world only; nothing native depends on it.
GLIBC=/lib/x86_64-linux-gnu
mkdir -p "$ROOT/lib/x86_64-linux-gnu" "$ROOT/lib64" "$ROOT/usr/lib"
if [ -e "$GLIBC/libc.so.6" ]; then
  for so in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 \
            libgcc_s.so.1 libresolv.so.2 libnss_dns.so.2 libnss_files.so.2; do
    [ -e "$GLIBC/$so" ] && cp -fL "$GLIBC/$so" "$ROOT/lib/x86_64-linux-gnu/$so"
  done
  # the ELF interpreter lives at /lib64/ld-linux-x86-64.so.2
  if [ -e "$GLIBC/ld-linux-x86-64.so.2" ]; then
    cp -fL "$GLIBC/ld-linux-x86-64.so.2" "$ROOT/lib64/ld-linux-x86-64.so.2"
    ln -sf /lib/x86_64-linux-gnu "$ROOT/usr/lib/x86_64-linux-gnu"
  fi
  echo ">> glibc runtime staged for third-party apps"
else
  echo "!! glibc not found; third-party apps will not be able to exec"
fi

# The real X11 stack. SlopOS runs Xvfb as the display server for third-party
# apps; slop-launch starts it, slop-xsession turns its screen into a SlopOS
# window and forwards input via XTEST. Without this, only the readiness screen
# can be shown. Everything here is compatibility-world only.
X11_BINS=""
for b in /usr/bin/Xvfb /usr/bin/xkbcomp /usr/bin/xauth; do
  [ -x "$b" ] && X11_BINS="$X11_BINS $b"
done
if [ -n "$X11_BINS" ]; then
  for b in $X11_BINS; do cp -fL "$b" "$ROOT/usr/bin/$(basename "$b")"; done
  python3 tools/stage-dynlibs.py "$ROOT" $X11_BINS
  # keyboard config the X server compiles at startup
  if [ -d /usr/share/X11/xkb ]; then
    mkdir -p "$ROOT/usr/share/X11"
    cp -rL /usr/share/X11/xkb "$ROOT/usr/share/X11/" 2>/dev/null || true
  fi
  # a minimal font set so X clients have core fonts
  if [ -d /usr/share/fonts/X11 ]; then
    mkdir -p "$ROOT/usr/share/fonts"
    cp -rL /usr/share/fonts/X11 "$ROOT/usr/share/fonts/" 2>/dev/null || true
  fi
  mkdir -p "$ROOT/tmp/.X11-unix"
  chmod 1777 "$ROOT/tmp"
  echo ">> X11 display server staged for third-party apps"
else
  echo "!! Xvfb not found; third-party apps cannot start their display"
fi

# sample X11 clients, used by `make verify-x11` to prove the whole bridge
# (X server -> window surface -> input) with a real program, not a stand-in.
X11_DEMO=""
for b in /usr/bin/xeyes /usr/bin/xclock; do
  [ -x "$b" ] && X11_DEMO="$X11_DEMO $b"
done
if [ -n "$X11_DEMO" ]; then
  for b in $X11_DEMO; do cp -fL "$b" "$ROOT/usr/bin/$(basename "$b")"; done
  python3 tools/stage-dynlibs.py "$ROOT" $X11_DEMO
  echo ">> X11 demo clients staged (xeyes, xclock)"
fi

# slop-xsession is dynamically linked against Xlib/XTest, so stage its own
# dependencies as well (the Xvfb deps cover most, but be explicit).
[ -x "$ROOT/usr/bin/slop-xsession" ] && \
  python3 tools/stage-dynlibs.py "$ROOT" "$ROOT/usr/bin/slop-xsession"

# compatibility manifests
cp -f compat/apps/*.app "$ROOT/usr/share/slop/compat/"

# The installer is a POSIX shell script: it unpacks app packages with busybox,
# so installing needs no native helper.
install -m 0755 apps/slop-install.sh "$ROOT/usr/bin/slop-install"

# Bundled third-party app packages live on the installation medium, not in the
# initramfs: they are hundreds of megabytes and already compressed, so packing
# them into the ramdisk would waste RAM and boot time. mkiso.sh copies them
# into the ISO's /packages, and slop-install reads them from there (or from
# /usr/share/slop/packages once installed).
mkdir -p "$ROOT/usr/share/slop/packages"

# a couple of sample files so the viewers have something to show
cat > "$ROOT/home/user/readme.txt" <<'EOF'
Welcome to SlopOS
=================

This is a small, from-scratch x86_64 desktop. Everything you see is drawn
directly to the framebuffer by our own toolkit (libslop); there is no X11 or
Wayland server underneath the native apps.

Try these:

  * Files        browse the home directory
  * Terminal     run commands in a real pty
  * Image Viewer open a PNG or JPEG
  * File Viewer  read this file with syntax colouring
  * System Info  see live values from /proc

Third-party apps (Zen Browser, OBS Studio, DaVinci Resolve) run through the
SlopOS compatibility runtime, which verifies their kernel and library
requirements before launching them.

Tip: press Ctrl+Q or the red traffic light to leave any app.
EOF

cat > "$ROOT/home/user/hello.c" <<'EOF'
/* A sample C file so the File Viewer has something to colour. */
#include <stdio.h>

int main(void) {
    for (int i = 0; i < 3; i++) {
        printf("hello from SlopOS %d\n", i);
    }
    return 0;
}
EOF

cat > "$ROOT/etc/motd" <<'EOF'
SlopOS 0.2 -- a smooth little x86_64 desktop
EOF

# sample images for the image viewer (generated at build time; needs Pillow)
if python3 -c "import PIL" 2>/dev/null; then
  python3 - "$ROOT/home/user/Pictures" <<'PY'
import sys
from PIL import Image, ImageDraw
out = sys.argv[1]
w, h = 640, 400
img = Image.new("RGB", (w, h))
d = ImageDraw.Draw(img)
for y in range(h):
    t = y / h
    d.line([(0, y), (w, y)], fill=(int(30+180*t), int(80+120*(1-t)), int(200-60*t)))
d.ellipse([80, 80, 280, 280], fill=(255, 210, 90))
d.rectangle([340, 120, 580, 300], fill=(74, 217, 154))
d.polygon([(320, 340), (400, 220), (480, 340)], fill=(240, 106, 106))
d.text((20, 20), "SlopOS sample image", fill=(255, 255, 255))
img.save(out + "/sample.png")
for name, col in [("one.png", (120, 140, 255)), ("two.png", (74, 217, 154)),
                  ("three.png", (240, 180, 60))]:
    t = Image.new("RGB", (200, 150), col)
    ImageDraw.Draw(t).ellipse([40, 30, 160, 120], fill=(255, 255, 255))
    t.save(out + "/" + name)
print(">> sample images generated")
PY
else
  echo "!! Pillow missing; skipping sample images"
fi

echo ">> packing initramfs"
( cd "$ROOT" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 ) > dist/initramfs.cpio.gz
ls -lh dist/initramfs.cpio.gz
echo ">> rootfs ready"
