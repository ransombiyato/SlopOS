# SlopOS build system.
#
#   make kernel   build the Linux kernel (hardware/ABI layer)
#   make user     build libslop, the shell, native apps and the compat runtime
#   make image    assemble the rootfs + initramfs
#   make iso      produce dist/slopos.iso (BIOS + UEFI bootable)
#   make run      boot it in QEMU
#   make all      kernel + user + image + iso
#
# Every SlopOS userland binary is linked statically so the initramfs is
# self-contained and boots on any x86_64 machine.

CC      := gcc
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -I libslop
LDFLAGS := -static
LDLIBS  := -lutil

DIST    := dist
ROOT    := $(DIST)/rootfs
BIN     := $(ROOT)/usr/bin

LIBSLOP := libslop/slop.c
FONT    := libslop/font_data.h

SHELL_BIN := $(BIN)/slop-shell
INIT_BIN  := $(ROOT)/init
APPS := files term image view about open
APP_BINS := $(addprefix $(BIN)/slop-,$(APPS))
COMPAT_BIN := $(BIN)/slop-launch

.PHONY: all kernel user image iso run clean fonts shots

all: iso

fonts: $(FONT)
$(FONT): tools/mkfont.py
	python3 tools/mkfont.py $(FONT)

$(LIBSLOP:.c=.o): $(LIBSLOP) $(FONT) libslop/slop.h
	$(CC) $(CFLAGS) -c $(LIBSLOP) -o $@

$(SHELL_BIN): shell/shell.c $(LIBSLOP) libslop/slop.h $(FONT)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) shell/shell.c $(LIBSLOP) -o $@ $(LDLIBS)

$(INIT_BIN): init/init.c $(LIBSLOP) libslop/slop.h $(FONT)
	@mkdir -p $(ROOT)
	$(CC) $(CFLAGS) $(LDFLAGS) init/init.c $(LIBSLOP) -o $@

$(BIN)/slop-%: apps/%.c $(LIBSLOP) libslop/slop.h $(FONT)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIBSLOP) -o $@ $(LDLIBS)

$(COMPAT_BIN): compat/slop-launch.c $(LIBSLOP) libslop/slop.h $(FONT)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) compat/slop-launch.c $(LIBSLOP) -o $@ $(LDLIBS)

user: $(INIT_BIN) $(SHELL_BIN) $(APP_BINS) $(COMPAT_BIN)
	@echo ">> userland built"

kernel:
	./kernel/build.sh

image:
	@mkdir -p dist
	rm -rf dist/rootfs
	$(MAKE) user
	./tools/mkimage.sh

iso: image
	./tools/mkiso.sh

run: iso
	./tools/run-qemu.sh

# Boot once per app and capture the framebuffer (needs a working qemu + Pillow).
shots: iso
	@mkdir -p dist
	python3 tools/screenshot.py dist/splash_5.png --wait 5
	python3 tools/screenshot.py dist/boot.png  --wait 20
	python3 tools/screenshot.py dist/launcher.png --wait 20 --keys m --post-wait 3
	python3 tools/screenshot.py dist/files.png --wait 20 --keys 1 --post-wait 4
	python3 tools/screenshot.py dist/term2.png --wait 20 --keys 2 --post-wait 5
	python3 tools/screenshot.py dist/image.png --wait 20 --keys 3 --post-wait 4
	python3 tools/screenshot.py dist/view.png  --wait 20 --keys 4 --post-wait 4
	python3 tools/screenshot.py dist/about.png --wait 20 --keys 8 --post-wait 4
	python3 tools/screenshot.py dist/compat.png --wait 20 --keys 5 --post-wait 5
	python3 tools/montage.py

# Prove the third-party launch path with a dynamic stand-in (see the script).
verify-compat: iso
	./tools/verify-compat.sh zen
	./tools/verify-compat.sh obs
	./tools/verify-compat.sh resolve

clean:
	rm -rf $(DIST) $(BIN) libslop/*.o
