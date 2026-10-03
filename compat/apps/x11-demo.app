# SlopOS third-party application manifest
# ----------------------------------------
# A real X11 client (xeyes) used by `make verify-x11` to prove the whole
# compatibility bridge end to end: Xvfb display server -> framebuffer shared
# with slop-xsession -> compositor window -> input forwarded back via XTEST.
# It is not one of the three third-party apps; it is a stand-in chosen because
# it is a genuine, dynamically linked X11 program.

name        = X11 Demo
vendor      = X.Org
id          = x11-demo
icon        = about
runtime     = compat

exec        = /usr/bin/xeyes
exec        = /usr/bin/xclock

profile     = desktop-graphical

needs_libs  = libc.so.6, libX11.so.6, libxcb.so.1, libXext.so.6

needs_kernel = unix_sockets, shm, epoll, evdev
