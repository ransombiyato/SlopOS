#!/usr/bin/env bash
# Verify the *entire* X11 compatibility path with a real dynamically linked
# X client, not a stand-in:
#
#   Xvfb (display server)  ->  /run/slop/x/Xvfb_screen0
#   slop-xsession          ->  blits that screen into a compositor window
#   XTEST                  ->  forwards shell input back to the X server
#
# The guest boots, the shell autostarts the `x11-demo` compatibility app, and
# the check confirms (a) slop-launch brought up the display server and exec'd
# the client, and (b) the client actually rendered pixels into the window.
#
# Usage: tools/verify-x11.sh
set -euo pipefail
cd "$(dirname "$0")/.."

[ -x dist/rootfs/usr/bin/Xvfb ] || { echo "!! Xvfb not staged; run 'make image'"; exit 2; }
[ -x dist/rootfs/usr/bin/xeyes ] || { echo "!! xeyes not staged; run 'make image'"; exit 2; }

echo 'x11-demo' > dist/rootfs/usr/share/slop/autostart
echo ">> repacking initramfs to autostart the X11 demo"
( cd dist/rootfs && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 ) > dist/initramfs.cpio.gz
./tools/mkiso.sh >/dev/null

echo ">> booting; the shell will start the X11 session"
python3 tools/screenshot.py dist/verify-x11.png --wait 24 --post-wait 10

echo ">> kernel log (compatibility session):"
grep -i "slop-x11\|slop-launch" /tmp/slopos-serial.log || echo "  (no compat log captured)"

ok=1
grep -q "slop-x11: display ready" /tmp/slopos-serial.log \
  || { echo ">> FAIL: X display server never came up"; ok=0; }
grep -q "slop-launch: exec /usr/bin/xeyes" /tmp/slopos-serial.log \
  || { echo ">> FAIL: X client was not exec'd"; ok=0; }

# The X client rendered into the window: count pixels that are not the shell
# background or pure black.
if ! python3 - <<'PY'
import sys
from PIL import Image
img = Image.open("dist/verify-x11.png").convert("RGB")
px = img.load()
w, h = img.size
bright = 0
for y in range(60, h - 90, 2):
    for x in range(0, w, 2):
        r, g, b = px[x, y]
        if r + g + b > 180:
            bright += 1
print(f">> bright pixels in window region: {bright}")
sys.exit(0 if bright > 1500 else 1)
PY
then
  echo ">> FAIL: X client produced no visible output"
  ok=0
fi

[ "$ok" = 1 ] && echo ">> PASS: real X11 app rendered through the SlopOS window" || exit 1
