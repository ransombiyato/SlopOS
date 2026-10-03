#!/usr/bin/env bash
# Build the SlopOS Linux kernel (the hardware/ABI layer under our userland).
# Output: kernel/out/bzImage
set -euo pipefail

KDIR="$(cd "$(dirname "$0")" && pwd)"
VER="${KERNEL_VER:-6.12}"
SRC="$KDIR/linux-$VER"
OUT="$KDIR/out"
JOBS="$(nproc)"

mkdir -p "$OUT" "$KDIR/dl"

if [ ! -d "$SRC" ]; then
  TARBALL="$KDIR/dl/linux-$VER.tar.xz"
  if [ ! -f "$TARBALL" ]; then
    echo ">> downloading linux-$VER"
    curl -fL --retry 3 -o "$TARBALL" \
      "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz"
  fi
  echo ">> extracting"
  tar -C "$KDIR" -xf "$TARBALL"
fi

cd "$SRC"
if [ ! -f .config ]; then
  echo ">> configuring"
  make x86_64_defconfig
  # --- SlopOS kernel feature set -------------------------------------------
  # Graphics: DRM + framebuffer so our native GUI has /dev/fb0.
  scripts/config -e DRM -e DRM_FBDEV_EMULATION -e DRM_BOCHS -e DRM_VIRTIO_GPU \
                 -e DRM_SIMPLEDRM -e FB -e FB_VESA -e FB_EFI -e SYSFB_SIMPLEFB \
                 -e FRAMEBUFFER_CONSOLE -e DRM_TTM
  # Input: evdev is the single source of truth for our GUI input stack.
  scripts/config -e INPUT_EVDEV -e INPUT_MOUSEDEV -e KEYBOARD_ATKBD \
                 -e MOUSE_PS2 -e HID -e USB_HID -e USB_XHCI_HCD -e USB_EHCI_HCD
  # Audio (OBS Studio compatibility layer).
  scripts/config -e SOUND -e SND -e SND_HDA_INTEL -e SND_PCI -e SND_VIRTIO \
                 -e SND_PCM -e SND_TIMER
  # Video capture / media (OBS Studio camera and capture devices).
  scripts/config -e MEDIA_SUPPORT -e MEDIA_CAMERA_SUPPORT -e VIDEO_DEV \
                 -e V4L2 -e VIDEO_V4L2 -e MEDIA_CONTROLLER -e USB_VIDEO_CLASS \
                 -e V4L2_MEM2MEM_DEV -e VIDEOBUF2_CORE -e VIDEOBUF2_V4L2 \
                 -e VIDEOBUF2_MEMOPS -e VIDEOBUF2_VMALLOC
  # Virtio (fast QEMU I/O).
  scripts/config -e VIRTIO -e VIRTIO_PCI -e VIRTIO_MMIO -e VIRTIO_BLK \
                 -e VIRTIO_NET -e VIRTIO_CONSOLE -e VIRTIO_INPUT
  # Filesystems / sandboxing needed by third-party desktop apps.
  scripts/config -e EXT4_FS -e SQUASHFS -e OVERLAY_FS -e FUSE_FS -e LOOP \
                 -e TMPFS -e DEVTMPFS -e DEVTMPFS_MOUNT -e PROC_FS -e SYSFS \
                 -e BINFMT_ELF -e BINFMT_SCRIPT -e BINFMT_MISC \
                 -e SECCOMP -e SECCOMP_FILTER -e NAMESPACES -e USER_NS \
                 -e CGROUPS -e MEMFD_CREATE -e EPOLL -e EVENTFD -e SIGNALFD \
                 -e TIMERFD -e AIO -e FUTEX -e UNIX -e INET -e PACKET -e NET \
                 -e UNIX98_PTYS -e TTY -e VT -e KALLSYMS
  make olddefconfig
fi

echo ">> building with -j$JOBS"
make -j"$JOBS" bzImage
cp -f arch/x86/boot/bzImage "$OUT/bzImage"
echo ">> done: $OUT/bzImage"
