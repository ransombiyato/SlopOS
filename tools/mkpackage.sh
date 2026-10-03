#!/usr/bin/env bash
# Build an installable SlopOS application package from a staged app tree.
#
#   tools/mkpackage.sh <staged-dir> <pack-id> [--name "Display Name"] \
#                      [--desc "one line"] [--size-mb N] [--exec /opt/zen/zen]
#
# Produces dist/pkg/<pack-id>.tar.xz plus dist/pkg/<pack-id>.pkg (a tiny
# key=value descriptor the setup wizard reads). The archive root is the
# filesystem root for the package, so extracting it with `tar -x -C /`
# installs the app at its real paths.
#
# Packages are ordinary tar.xz on purpose: the guest unpacks them with
# busybox (`xz -dc … | tar -x`), so installation needs no native code and no
# extra kernel features.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=${1:?usage: mkpackage.sh <staged-dir> <pack-id> ...}
ID=${2:?missing package id}
shift 2

NAME=$ID
DESC=""
SIZE_MB=0
EXEC=""
while [ $# -gt 0 ]; do
  case "$1" in
    --name) NAME=$2; shift 2 ;;
    --desc) DESC=$2; shift 2 ;;
    --size-mb) SIZE_MB=$2; shift 2 ;;
    --exec) EXEC=$2; shift 2 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done

[ -d "$SRC" ] || { echo "!! no such staged dir: $SRC"; exit 1; }

mkdir -p dist/pkg
OUT=dist/pkg/$ID.tar.xz

echo ">> packing $ID from $SRC"
( cd "$SRC" && tar -cJf "$OLDPWD/$OUT" --owner=0 --group=0 . )

# Actual installed size (uncompressed bytes) so setup can quote real numbers.
BYTES=$(du -sb "$SRC" | awk '{print $1}')
[ "$SIZE_MB" -gt 0 ] 2>/dev/null || SIZE_MB=$(( (BYTES + 1048575) / 1048576 ))

cat > dist/pkg/$ID.pkg <<EOF
id        = $ID
name      = $NAME
desc      = $DESC
archive   = $ID.tar.xz
size_mb   = $SIZE_MB
exec      = $EXEC
EOF

ls -lh "$OUT"
echo ">> package $ID ready"
