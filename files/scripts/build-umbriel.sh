#!/usr/bin/env bash
# Build stage "umbriel": builds Umbriel, its portal and Noctalia from the main branch into /out,
# and lists the shared libraries the binaries link against in /out/usr/share/umbriel-session/rpm-deps.txt.
set -euo pipefail

dnf -y install git file gcc-c++ meson ninja-build pkgconf-pkg-config wayland-devel wayland-protocols-devel \
  'pkgconfig(wlroots-0.20)' libxkbcommon-devel libinput-devel libeis-devel systemd-devel pixman-devel \
  libdrm-devel libdisplay-info-devel cairo-devel pango-devel tomlplusplus-devel json-devel libxcb-devel \
  xcb-util-wm-devel mesa-libEGL-devel mesa-libGLES-devel mesa-libgbm-devel lcms2-devel jemalloc-devel \
  pipewire-devel sdbus-cpp-devel gtk4-devel freetype-devel fontconfig-devel harfbuzz-devel librsvg2-devel \
  glib2-devel libsecret-devel libsodium-devel polkit-devel wireplumber-devel libcurl-devel libqalculate-devel \
  pam-devel libxml2-devel md4c-devel libical-devel libwebp-devel libjxl-devel libsndfile-devel stb-devel \
  stb_image_resize2-devel stb_image_write-devel

info=/out/usr/share/umbriel-session
mkdir -p "$info"
for repo in umbriel xdg-desktop-portal-umbriel noctalia; do
  git clone --depth 1 "https://github.com/noctalia-dev/$repo" "/build/$repo"
  echo "$repo $(git -C "/build/$repo" rev-parse HEAD)" >> "$info/revisions.txt"
done

cd /build/umbriel
meson setup build --buildtype=release --prefix=/usr -Dtests=disabled
meson install -C build --destdir /out
# The packaged default config starts no shell and binds kitty, which is not installed.
sed -i -e 's|^# autostart = \["noctalia", "kitty"\]|autostart = ["noctalia"]|' -e 's|spawn:kitty|spawn:ptyxis|' \
  /out/usr/share/umbriel/config.toml
grep -q '^autostart = \["noctalia"\]' /out/usr/share/umbriel/config.toml

cd /build/xdg-desktop-portal-umbriel
meson setup build --buildtype=release --prefix=/usr -Dtests=false
meson install -C build --destdir /out

cd /build/noctalia
meson setup build --buildtype=release --prefix=/usr -Dtests=disabled
meson install -C build --destdir /out
mkdir -p /out/usr/share/fish/vendor_completions.d
./build/noctalia completions fish > /out/usr/share/fish/vendor_completions.d/noctalia.fish
# Keep Noctalia out of the GNOME app grid and the image "Open With" list. The session sets
# XDG_CURRENT_DESKTOP=umbriel (lowercase) although umbriel.desktop says Umbriel; the match is case-sensitive.
for f in /out/usr/share/applications/dev.noctalia.Noctalia*.desktop; do
  sed -i '/^\[Desktop Entry\]/a OnlyShowIn=Umbriel;umbriel;' "$f"
done

# Sonames, not package names: Bazzite replaces some Fedora library packages (terra-wireplumber-libs).
find /out -type f -executable -exec sh -c 'file -b "$1" | grep -q ELF && ldd "$1"' _ {} \; \
  | awk '$2 == "=>" && $3 ~ /^\// { print $1 "()(64bit)" }' | sort -u > "$info/rpm-deps.txt"
cat "$info/revisions.txt" "$info/rpm-deps.txt"
