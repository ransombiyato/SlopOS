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
LDLIBS  := -lutil -lm

DIST    := dist
ROOT    := $(DIST)/rootfs
BIN     := $(ROOT)/usr/bin

LIBSLOP := libslop/slop.c libslop/slop_icons.c
FONT    := libslop/font_data.h

SHELL_BIN := $(BIN)/slop-shell
INIT_BIN  := $(ROOT)/init
APPS := files term image view about open calc monitor settings notes setup
APP_BINS := $(addprefix $(BIN)/slop-,$(APPS))
COMPAT_BIN := $(BIN)/slop-launch
XSESSION_BIN := $(BIN)/slop-xsession

# slop-xsession is part of the compatibility world (it speaks X11), so unlike
# the native apps it is dynamically linked and links Xlib/XTest.
X11_CFLAGS := $(shell pkg-config --cflags x11 xtst 2>/dev/null)
X11_LIBS   := $(shell pkg-config --libs x11 xtst 2>/dev/null || echo -lX11 -lXtst)

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
	$(CC) $(CFLAGS) $(LDFLAGS) init/init.c $(LIBSLOP) -o $@ $(LDLIBS)

$(BIN)/slop-%: apps/%.c $(LIBSLOP) libslop/slop.h $(FONT)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIBSLOP) -o $@ $(LDLIBS)

$(COMPAT_BIN): compat/slop-launch.c $(LIBSLOP) libslop/slop.h $(FONT)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) compat/slop-launch.c $(LIBSLOP) -o $@ $(LDLIBS)

$(XSESSION_BIN): compat/xsession.c $(LIBSLOP) libslop/slop.h $(FONT)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(X11_CFLAGS) compat/xsession.c $(LIBSLOP) -o $@ $(X11_LIBS) $(LDLIBS)

user: $(INIT_BIN) $(SHELL_BIN) $(APP_BINS) $(COMPAT_BIN) $(XSESSION_BIN)
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
# Apps are opened by clicking their dock icon, which also exercises the shell's
# hit-testing and launch path rather than only the keyboard shortcuts.
DOCK_X := 299 361 423 485 547 609 671 733 795 857 919 981
SHOT_NAMES := files term image view zen obs resolve calc monitor settings notes about
shots: iso
	@mkdir -p dist
	python3 tools/screenshot.py dist/shot-boot.png --wait 6 --post-wait 0.5
	python3 tools/screenshot.py dist/shot-desktop.png --wait 20 --post-wait 1
	python3 tools/screenshot.py dist/shot-launcher.png --wait 20 --keys m --post-wait 2
	@i=0; for a in $(SHOT_NAMES); do \
	  x=$$(echo $(DOCK_X) | cut -d' ' -f$$((i+1))); \
	  echo ">> shot $$a (dock x=$$x)"; \
	  python3 tools/screenshot.py dist/shot-$$a.png --wait 20 --click $$x,755 --post-wait 5; \
	  i=$$((i+1)); \
	done
	python3 tools/montage.py

# Prove the third-party launch path with a dynamic stand-in (see the script).
verify-compat: iso
	./tools/verify-compat.sh zen
	./tools/verify-compat.sh obs
	./tools/verify-compat.sh resolve

# Prove the real X11 path: a genuine X client renders into a SlopOS window.
verify-x11: iso
	./tools/verify-x11.sh

clean:
	rm -rf $(DIST) $(BIN) libslop/*.o
