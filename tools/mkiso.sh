#!/usr/bin/env bash
# Build a hybrid BIOS+UEFI bootable ISO for SlopOS using GRUB.
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=dist/slopos.iso
ISODIR=dist/isodir
KERNEL=kernel/out/bzImage
INITRD=dist/initramfs.cpio.gz

[ -f "$KERNEL" ] || { echo "!! $KERNEL missing (run 'make kernel')"; exit 1; }
[ -f "$INITRD" ] || { echo "!! $INITRD missing (run 'make image')"; exit 1; }

rm -rf "$ISODIR"
mkdir -p "$ISODIR/boot/grub/themes/slopos"

cp -f "$KERNEL" "$ISODIR/boot/bzImage"
cp -f "$INITRD" "$ISODIR/boot/initramfs.cpio.gz"

# branded boot menu (falls back to the plain menu if assets are missing)
if [ -f branding/grub/background.png ] && [ -f branding/grub/theme.txt ]; then
  cp -f branding/grub/background.png branding/grub/theme.txt \
        "$ISODIR/boot/grub/themes/slopos/"
  if [ -f /usr/share/grub/unicode.pf2 ]; then
    cp -f /usr/share/grub/unicode.pf2 "$ISODIR/boot/grub/themes/slopos/"
  fi
  THEME='set theme=/boot/grub/themes/slopos/theme.txt'
else
  THEME=''
fi

cat > "$ISODIR/boot/grub/grub.cfg" <<EOF
set timeout=3
set default=0

insmod all_video
insmod gfxterm
insmod png
if [ "\$grub_platform" = "efi" ]; then
    insmod efi_gop
    insmod efi_uga
else
    insmod vbe
    insmod video_bochs
    insmod video_cirrus
fi

set gfxmode=auto
set gfxpayload=keep
terminal_output gfxterm
$THEME

menuentry "SlopOS 0.1" {
    linux /boot/bzImage quiet loglevel=3 console=ttyS0,115200 rdinit=/init
    initrd /boot/initramfs.cpio.gz
}

menuentry "SlopOS 0.1 (verbose boot)" {
    linux /boot/bzImage loglevel=7 console=ttyS0,115200 rdinit=/init
    initrd /boot/initramfs.cpio.gz
}
EOF

echo ">> building $ISO"
grub-mkrescue -o "$ISO" "$ISODIR" \
    --product-name="SlopOS" --product-version="0.1" 2>&1 | tail -5

ls -lh "$ISO"
echo ">> iso ready"
