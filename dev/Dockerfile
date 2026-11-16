# 开发与冒烟测试环境：Debian trixie 自带 wlroots 0.18。
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential meson ninja-build pkg-config \
      libwlroots-0.18-dev libwayland-dev wayland-protocols libxkbcommon-dev \
      libpixman-1-dev libpng-dev \
      sway grim wtype bubblewrap dbus dbus-x11 at-spi2-core passt \
      foot weston wayland-utils fonts-dejavu-core python3 python3-pil procps sysstat jq \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
