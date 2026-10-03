#!/bin/sh
# slop-install -- install a SlopOS application package.
#
#   slop-install <pack-id>
#
# Packages are plain tar.xz archives rooted at the filesystem root, plus a
# small key=value descriptor. Setup and Settings call this to install the
# bundled third-party apps (Zen Browser, OBS Studio). It writes a status line
# to /run/slop/install.status so the calling UI can show progress.
#
# Deliberately POSIX shell: it runs under the image's busybox, so installing
# needs no native helper and no extra kernel features.
set -u

id=${1:-}
if [ -z "$id" ]; then
    echo "usage: slop-install <package-id>" >&2
    exit 2
fi

# Packages can live on the installation medium (/media/cdrom/packages) or,
# once installed, under /usr/share/slop/packages.
pkg=""
for dir in /usr/share/slop/packages /media/cdrom/packages /run/slop/packages; do
    if [ -f "$dir/$id.pkg" ]; then
        pkg="$dir/$id.pkg"
        break
    fi
done
if [ -z "$pkg" ]; then
    echo "error: no package '$id'" >&2
    exit 1
fi
pkgdir=$(dirname "$pkg")

val() {
    # print the value of a key in the descriptor, trimmed
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$pkg" | head -n1 | \
        sed 's/[[:space:]]*$//'
}

name=$(val name)
archive=$(val archive)
size_mb=$(val size_mb)
: "${name:=$id}"

# The archive path in the descriptor is relative to the package directory.
case "$archive" in
    /*) : ;;
    *)  archive="$pkgdir/$archive" ;;
esac

mkdir -p /run/slop
echo "started $id" > /run/slop/install.status

echo "Installing $name ($size_mb MB)..." >&2

if [ ! -f "$archive" ]; then
    echo "failed $id" > /run/slop/install.status
    echo "error: archive $archive missing" >&2
    exit 1
fi

# Unpack straight over the root. xz -dc | tar -x keeps memory use flat and
# needs only the busybox applets already on the image.
if ! xz -dc "$archive" | tar -x -C / 2>/run/slop/install.err; then
    echo "failed $id" > /run/slop/install.status
    echo "error: unpack failed (see /run/slop/install.err)" >&2
    exit 1
fi

# Make sure installed launchers are executable (tar normally preserves this,
# but a package built on a restrictive umask can lose the bit).
exec_path=$(val exec)
if [ -n "$exec_path" ] && [ -e "$exec_path" ]; then
    chmod 0755 "$exec_path" 2>/dev/null || true
fi

echo "done $id" > /run/slop/install.status
echo "$name installed" >&2
exit 0
