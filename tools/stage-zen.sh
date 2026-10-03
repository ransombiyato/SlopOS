#!/usr/bin/env bash
# Stage a Zen Browser tarball into an installable filesystem tree.
#
#   tools/stage-zen.sh <extracted-zen-dir> <stage-root>
#
# Zen is self-contained (it bundles Gecko, NSS, NSPR and the sqlite/codec
# libraries), so the only host libraries it needs are the ordinary desktop
# stack: GTK3, X11, cairo/pango, dbus, alsa. This resolves them from the host
# and drops them into the stage tree, then the tree can be packed with
# tools/mkpackage.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=${1:?usage: stage-zen.sh <extracted-zen-dir> <stage-root>}
STAGE=${2:?missing stage root}

[ -x "$SRC/zen" ] || { echo "!! $SRC/zen not found"; exit 1; }

echo ">> staging Zen into $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/opt/zen" "$STAGE/lib/x86_64-linux-gnu" "$STAGE/lib64"

cp -a "$SRC/." "$STAGE/opt/zen/"
chmod 0755 "$STAGE/opt/zen/zen" "$STAGE/opt/zen/zen-bin" 2>/dev/null || true

# Resolve the real dependency closure of the launcher and the Gecko core,
# letting bundled libraries in /opt/zen satisfy their own deps.
python3 - "$SRC" "$STAGE" <<'PY'
import os, re, shutil, subprocess, sys

src, stage = sys.argv[1], sys.argv[2]
libdir = os.path.join(stage, "lib/x86_64-linux-gnu")
bundle = os.path.abspath(src)
env = dict(os.environ, LD_LIBRARY_PATH=bundle)

def deps(path):
    out = subprocess.run(["ldd", path], capture_output=True, text=True,
                         env=env, timeout=60).stdout
    found = []
    for line in out.splitlines():
        m = re.search(r"=>\s+(\S+)\s+\(", line)
        if m:
            found.append(m.group(1))
        elif re.search(r"^\s*/\S+\s+\(", line):
            found.append(line.split()[0])
    return found

queue = [os.path.join(src, "zen-bin"), os.path.join(src, "libxul.so")]
seen, copied = set(), 0
while queue:
    b = queue.pop()
    if b in seen or not os.path.exists(b):
        continue
    seen.add(b)
    for d in deps(b):
        real = os.path.realpath(d)
        if real.startswith(bundle + os.sep):
            continue                      # shipped inside /opt/zen
        if not os.path.exists(real) or real in seen:
            continue
        if "/ld-linux" in real or re.search(r"/ld-[0-9.]*\.so", real):
            dst = os.path.join(stage, "lib64", os.path.basename(real))
        else:
            dst = os.path.join(libdir, os.path.basename(real))
        if not os.path.exists(dst):
            shutil.copy2(real, dst)
            os.chmod(dst, 0o755)
            copied += 1
        seen.add(real)
        queue.append(real)

print(f">> staged {copied} host libraries for Zen")
PY

# Zen ships no fontconfig cache; give it a writable place and a font dir.
mkdir -p "$STAGE/usr/share/fonts" "$STAGE/etc/fonts"
if [ -d /usr/share/fonts/truetype/dejavu ]; then
  cp -a /usr/share/fonts/truetype/dejavu "$STAGE/usr/share/fonts/" 2>/dev/null || true
fi

echo ">> Zen staged at $STAGE ($(du -sh "$STAGE" | cut -f1))"
