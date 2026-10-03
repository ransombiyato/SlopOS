#!/usr/bin/env bash
# Boot SlopOS in QEMU. Uses the Bochs framebuffer so /dev/fb0 works, plus a
# PS/2 keyboard and USB tablet for input.
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=dist/slopos.iso
MEM=${MEM:-2048}
CPUS=${CPUS:-2}
MODE=${1:-gui}   # gui | nographic | test

COMMON=(
  -m "$MEM" -smp "$CPUS"
  -vga std
  -display none
  -serial stdio
  -monitor none
  -no-reboot
)

case "$MODE" in
  nographic)
    exec qemu-system-x86_64 -cdrom "$ISO" -nographic -m "$MEM" -smp "$CPUS" -vga std
    ;;
  test)
    # boot, give it time, then power off; used by CI/verification
    timeout 45 qemu-system-x86_64 -cdrom "$ISO" "${COMMON[@]}" \
        -device virtio-rng-pci 2>&1 || true
    ;;
  *)
    if [ -n "${DISPLAY:-}" ]; then
      exec qemu-system-x86_64 -cdrom "$ISO" -m "$MEM" -smp "$CPUS" -vga std \
           -device usb-tablet -serial mon:stdio
    else
      echo ">> no DISPLAY; booting headless and capturing the framebuffer"
      exec qemu-system-x86_64 -cdrom "$ISO" "${COMMON[@]}"
    fi
    ;;
esac
