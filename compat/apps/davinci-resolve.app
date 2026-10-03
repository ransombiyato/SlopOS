# SlopOS third-party application manifest
# ----------------------------------------
# DaVinci Resolve is the demanding case: Qt + OpenCL + OpenGL, a large
# working set, and a CPU/GPU feature probe at startup. The workstation
# profile grants OpenCL, more shared memory, and the realtime scheduling it
# requests. If the GPU probe fails it falls back to the software path.

name        = DaVinci Resolve
vendor      = Blackmagic Design
id          = resolve
icon        = resolve
runtime     = compat

exec        = /opt/davinci/bin/resolve
exec        = /usr/bin/davinci-resolve
exec        = /opt/resolve/bin/resolve

profile     = workstation-gpu

needs_libs  = libc.so.6, libm.so.6, libpthread.so.0, libdl.so.2, librt.so.1,
              libGL.so.1, libEGL.so.1, libGLU.so.1, libOpenCL.so.1,
              libX11.so.6, libxcb.so.1, libxcb-util.so.1,
              libXext.so.6, libXrender.so.1, libXfixes.so.3,
              libQt5Core.so.5, libQt5Gui.so.5, libQt5Widgets.so.5,
              libQt5OpenGL.so.5, libglib-2.0.so.0, libgobject-2.0.so.0,
              libfontconfig.so.1, libfreetype.so.6, libz.so.1,
              libasound.so.2, libdbus-1.so.3, libudev.so.1,
              libssl.so.3, libcrypto.so.3

needs_kernel = seccomp, namespaces, memfd, eventfd, epoll, inotify,
               unix_sockets, tcp, shm, drm, drm_render, evdev, snd,
               sched_rt, futex, aio

needs_dev   = /dev/dri/renderD128, /dev/dri/card0

# Resolve refuses to start below this much RAM.
min_ram_mb  = 2048
min_cores   = 4

data_dir    = /home/user/.local/share/DaVinciResolve
