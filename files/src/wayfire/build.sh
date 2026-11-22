#!/usr/bin/env bash
# Builds the Wayfire stack from upstream main into /out (a root tree for the image).
# Fedora's package is stuck at 0.10.1 (non-responsive maintainer). Each repo builds at the commit in its variable
# (wf_config, wayfire, ...) or else at the head of main. main of each repo expects main of the others.
set -euo pipefail

order=(wf-config wayfire wf-shell wcm wayfire-plugins-extra)

dnf -y install --setopt=install_weak_deps=False \
  meson ninja-build gcc-c++ pkgconf-pkg-config cmake git \
  wlroots-devel libxml2-devel libevdev-devel glm-devel \
  cairo-devel pango-devel mesa-libEGL-devel mesa-libGLES-devel mesa-libgbm-devel libdrm-devel \
  libinput-devel libjpeg-turbo-devel libpng-devel systemd-devel pixman-devel vulkan-loader-devel \
  wayland-devel wayland-protocols-devel libxcb-devel libxkbcommon-devel xcb-util-wm-devel \
  xorg-x11-server-Xwayland-devel libdisplay-info-devel libliftoff-devel hwdata-devel \
  gtkmm4.0-devel gtkmm3.0-devel gtk4-layer-shell-devel libdbusmenu-devel libepoxy-devel openssl-devel \
  pam-devel pipewire-devel wireplumber-devel pulseaudio-libs-devel alsa-lib-devel glibmm2.4-devel yyjson-devel fmt-devel boost-devel \
  gobject-introspection-devel vala

mkdir -p /build /out/usr/share/wayfire
for name in "${order[@]}"; do
  src="/build/$name"
  git clone -q --filter=blob:none "https://github.com/WayfireWM/$name" "$src"
  var=${name//-/_}
  git -C "$src" checkout -q "${!var:-origin/HEAD}"
  echo "$name $(git -C "$src" rev-parse HEAD)" >> /out/usr/share/wayfire/stack-commits
  git -C "$src" submodule -q update --init --recursive --filter=blob:none
  meson setup "$src/build" "$src" --prefix=/usr --libdir=lib64 --buildtype=release -Dtests=disabled 2>/dev/null \
    || meson setup "$src/build" "$src" --prefix=/usr --libdir=lib64 --buildtype=release
  meson compile -C "$src/build"
  meson install -C "$src/build" --destdir /out
  # Later builds in this stage link against the earlier ones.
  meson install -C "$src/build"
done
