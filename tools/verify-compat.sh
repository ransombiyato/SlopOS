#!/usr/bin/env bash
# Verify the compatibility runtime's *launch* path, end to end, without the
# real (non-redistributable) third-party apps.
#
# It installs a tiny dynamically linked stand-in at Zen's declared path. The
# stand-in only proceeds if slop-launch actually applied the desktop-graphical
# profile, and then execs a native app we can see. A successful run therefore
# proves: manifest parsed -> kernel features probed -> dynamic binary detected
# and exec'd via the staged glibc loader -> profile environment applied.
#
# Usage: tools/verify-compat.sh [zen|obs|resolve]
set -euo pipefail
cd "$(dirname "$0")/.."

APP="${1:-zen}"
case "$APP" in
  zen)     ICON=5;  PATH_EXE=/opt/zen/zen           ;;
  obs)     ICON=6;  PATH_EXE=/opt/obs/bin/obs       ;;
  resolve) ICON=7;  PATH_EXE=/opt/davinci/bin/resolve ;;
  *) echo "unknown app: $APP (zen|obs|resolve)"; exit 2;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/standin.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
int main(void) {
    /* Require the graphical profile slop-launch promised us: all three
       profiles set a toolkit backend, and Zen/OBS also set GDK_BACKEND. */
    const char *g = getenv("GDK_BACKEND");
    const char *q = getenv("QT_QPA_PLATFORM");
    fprintf(stderr, "standin: GDK_BACKEND=%s QT_QPA_PLATFORM=%s\n",
            g ? g : "(unset)", q ? q : "(unset)");
    fprintf(stderr, "standin: THIS BINARY IS DYNAMIC\n");
    if ((g && !strcmp(g, "x11")) || (q && !strcmp(q, "xcb")))
        execl("/usr/bin/slop-about", "slop-about", (char *)NULL);
    return 3;
}
EOF

# dynamic (not -static): exercises the glibc loader we stage for third-party apps
gcc -O2 -o "$TMP/standin" "$TMP/standin.c"
mkdir -p "dist/rootfs$(dirname "$PATH_EXE")"
cp -f "$TMP/standin" "dist/rootfs$PATH_EXE"

echo ">> repacking initramfs with a dynamic stand-in at $PATH_EXE"
( cd dist/rootfs && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 ) > dist/initramfs.cpio.gz
./tools/mkiso.sh >/dev/null

echo ">> booting and launching slot $ICON"
python3 tools/screenshot.py dist/verify-$APP.png --wait 22 --keys "$ICON" --post-wait 8

echo ">> serial log:"
grep -i "standin" /tmp/slopos-serial.log || echo "  (no standin output captured)"

# If the launch path ran, our stand-in exec'd slop-about, so the frame must be
# pixel-identical to the About screenshot; the readiness screen is not.
python3 - "$APP" <<'PY'
import sys
from PIL import Image, ImageChops
app = sys.argv[1]
def diff(a, b):
    A = Image.open(a).convert("RGB"); B = Image.open(b).convert("RGB")
    d = ImageChops.difference(A, B).convert("L").load(); w, h = A.size
    return sum(1 for y in range(0, h, 2) for x in range(0, w, 2) if d[x, y] > 20)
shot = f"dist/verify-{app}.png"
try:
    same_as_about = diff(shot, "dist/about.png")
except FileNotFoundError:
    print("!! missing reference screenshots; run 'make shots' first")
    sys.exit(2)
print(f">> pixels differing from About screen: {same_as_about}")
# The panel clock ticks between captures, so allow a small delta; the
# readiness screen differs by ~10,000 pixels, so this cleanly separates them.
if same_as_about <= 100:
    print(">> PASS: slop-launch detected and launched the dynamic app")
else:
    print(">> FAIL: launch path did not run (readiness screen shown instead)")
    sys.exit(1)
PY
