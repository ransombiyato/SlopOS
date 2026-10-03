# SlopOS third-party application manifest
# ----------------------------------------
# Zen Browser is a Firefox (Gecko) fork. It is dynamically linked against
# glibc and needs the usual desktop stack, so it is declared to run under the
# SlopOS compatibility runtime rather than natively.

name        = Zen Browser
vendor      = Zen Team (Firefox fork)
id          = zen
icon        = zen
runtime     = compat

# Candidate launchers, tried in order. The first is the real package; the
# second is the flatpak fallback that ships in the app catalog.
exec        = /opt/zen/zen
exec        = /usr/bin/zen-browser
exec        = flatpak run app.zen_browser.zen

# Compatibility profile applied before exec (see compat/README.md).
profile     = desktop-graphical

# Dynamic loader and libraries the app needs from the host rootfs.
needs_libs  = libc.so.6, libpthread.so.0, libdl.so.2, librt.so.1,
              libgtk-3.so.0, libgdk-3.so.0, libglib-2.0.so.0,
              libgobject-2.0.so.0, libgio-2.0.so.0, libX11.so.6,
              libxcb.so.1, libXcomposite.so.1, libXdamage.so.1,
              libXfixes.so.3, libXrandr.so.2, libasound.so.2,
              libdbus-1.so.3, libnss3.so, libnspr4.so, libexpat.so.1,
              libfontconfig.so.1, libfreetype.so.6, libz.so.1

# Kernel facilities the app expects to find at run time.
needs_kernel = seccomp, namespaces, user_ns, memfd, eventfd, signalfd,
               epoll, inotify, unix_sockets, tcp, shm, drm, evdev

# Where the app's profile/cache live.
data_dir    = /home/user/.mozilla/zen
