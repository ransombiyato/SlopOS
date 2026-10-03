# SlopOS third-party application manifest
# ----------------------------------------
# OBS Studio needs graphics capture (OpenGL/Vulkan), audio capture
# (PipeWire/ALSA) and a compositor. It runs under the compatibility runtime
# with the media profile, which grants the extra device nodes it asks for.

name        = OBS Studio
vendor      = OBS Project
id          = obs
icon        = obs
runtime     = compat

exec        = /opt/obs/obs
exec        = /usr/bin/obs
exec        = flatpak run com.obsproject.Studio

profile     = media-graphical

needs_libs  = libc.so.6, libpthread.so.0, libdl.so.2, librt.so.1,
              libgtk-3.so.0, libgdk-3.so.0, libglib-2.0.so.0,
              libgobject-2.0.so.0, libgio-2.0.so.0, libX11.so.6,
              libxcb.so.1, libGL.so.1, libEGL.so.1, libgbm.so.1,
              libasound.so.2, libpulse.so.0, libpipewire-0.3.so.0,
              libavcodec.so.59, libavformat.so.59, libavutil.so.57,
              libswscale.so.6, libswresample.so.4, libz.so.1,
              libdbus-1.so.3, libudev.so.1, libv4l2.so.0

needs_kernel = seccomp, namespaces, memfd, eventfd, epoll, inotify,
               unix_sockets, tcp, shm, drm, evdev, v4l2, snd, drm_render

needs_dev   = /dev/dri/renderD128, /dev/video0, /dev/snd

data_dir    = /home/user/.config/obs-studio
