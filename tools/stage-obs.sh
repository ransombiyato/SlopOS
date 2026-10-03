#!/usr/bin/env bash
# Stage the prebuilt OBS Studio .deb into an installable filesystem tree.
#
#   tools/stage-obs.sh <extracted-obs-dir> <stage-root>
#
# The upstream OBS release is built for Ubuntu 24.04, so its runtime (Qt6,
# FFmpeg 6, mbedTLS 2.28, pipewire, ...) is fetched from the noble archive and
# placed on the standard search path. Stack libraries Debian already provides
# are left to the base system.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=${1:?usage: stage-obs.sh <extracted-obs-dir> <stage-root>}
STAGE=${2:?missing stage root}

[ -x "$SRC/usr/local/bin/obs" ] || { echo "!! $SRC/usr/local/bin/obs not found"; exit 1; }

echo ">> staging OBS into $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/opt/obs" "$STAGE/lib/x86_64-linux-gnu" "$STAGE/lib64" \
         "$STAGE/etc" "$STAGE/usr/share"

# OBS's /usr/local tree becomes /opt/obs. Its plugins and data keep their
# relative layout so the bundled obs finds them via its own search paths.
cp -a "$SRC/usr/local/bin" "$STAGE/opt/obs/"
cp -a "$SRC/usr/local/lib" "$STAGE/opt/obs/"
cp -a "$SRC/usr/local/share" "$STAGE/opt/obs/"

# Wrapper that points OBS at its relocated tree.
cat > "$STAGE/opt/obs/obs" <<'EOF'
#!/bin/sh
# Launch OBS Studio from its SlopOS install tree. OBS looks for its plugins
# and data relative to the binary, so keep the tree together under /opt/obs.
here=$(cd "$(dirname "$0")" && pwd)
export OBS_PLUGINS_PATH="${OBS_PLUGINS_PATH:-$here/lib/x86_64-linux-gnu/obs-plugins}"
export OBS_DATA_PATH="${OBS_DATA_PATH:-$here/share/obs}"
# Qt needs to be told where its platform plugins (libqxcb) live, since the
# compatibility runtime is not a full distro install.
export QT_PLUGIN_PATH="$here/../../usr/lib/x86_64-linux-gnu/qt6/plugins"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export LD_LIBRARY_PATH="$here/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
exec "$here/bin/obs" "$@"
EOF
chmod 0755 "$STAGE/opt/obs/obs"

# The bundled Python scripting plugin dlopens CPython, which the image does
# not carry; leave it out so OBS starts cleanly.
find "$STAGE/opt/obs/lib" -name "_obspython.so" -exec sh -c 'mv "$1" "$1.disabled"' _ {} \; 2>/dev/null || true

# The glibc loader on the image is Debian 13's; OBS needs >= 2.38, which
# Debian 13 satisfies (2.41). Base sonames stay Debian's.
echo ">> fetching the Ubuntu runtime OBS expects"
python3 tools/fetch-ubuntu-deb.py "$STAGE" \
  libqt6core6t64 libqt6gui6t64 libqt6widgets6t64 libqt6svg6 \
  libqt6network6t64 libqt6dbus6t64 libqt6xml6t64 \
  libavcodec60 libavformat60 libavutil58 libswscale7 libswresample4 libavdevice60 \
  libmbedtls14t64 libmbedcrypto7t64 libmbedx509-1t64 \
  libpci3 libpython3.12t64 librist4 libsrt1.5-openssl \
  libluajit-5.1-2 libqrcodegencpp1 libvpl2 libx264-164 libspeexdsp1 \
  libpipewire-0.3-0t64 libpulse0 libv4l-0t64 libva2 libva-drm2 \
  libpgm-5.3-0t64 libproxy1v5 \
  libnspr4 libnss3 2>&1 | tail -8

# GPU/OpenGL and audio client libraries OBS dlopens at runtime.
for extra in /usr/lib/x86_64-linux-gnu/libEGL.so.1 /usr/lib/x86_64-linux-gnu/libGL.so.1 \
             /usr/lib/x86_64-linux-gnu/libGLX.so.0 /usr/lib/x86_64-linux-gnu/libGLdispatch.so.0 \
             /usr/lib/x86_64-linux-gnu/libOpenGL.so.0 /usr/lib/x86_64-linux-gnu/libgbm.so.1 \
             /usr/lib/x86_64-linux-gnu/libdrm.so.2 /usr/lib/x86_64-linux-gnu/libasound.so.2; do
  [ -e "$extra" ] && cp -fL "$extra" "$STAGE/lib/x86_64-linux-gnu/"
done

# Some Ubuntu packages nest helper libraries one level down (libproxy ships
# libpxbackend in a libproxy/ subdir). OBS links it directly, so expose it on
# the standard search path too.
for sub in "$STAGE"/usr/lib/x86_64-linux-gnu/*/; do
  [ -d "$sub" ] || continue
  for so in "$sub"*; do
    [ -e "$so" ] || continue
    ln -sf "$(basename "$sub")/$(basename "$so")" \
           "$STAGE/usr/lib/x86_64-linux-gnu/$(basename "$so")"
  done
done

echo ">> OBS staged at $STAGE ($(du -sh "$STAGE" | cut -f1))"
