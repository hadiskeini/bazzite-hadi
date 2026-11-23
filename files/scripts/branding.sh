#!/usr/bin/env bash
# Fedora branding instead of Bazzite's. Runs before the initramfs module, which puts the restored boot splash into
# the initramfs.
set -euo pipefail

# Boot splash: Bazzite overwrote Fedora's watermark; reinstalling fedora-logos restores the original file.
dnf -y reinstall fedora-logos

# Name and logo in System Settings > About, fastfetch and the boot menu. ID stays bazzite: Bazzite's own tools read it.
. /usr/lib/os-release
sed -i -e 's/^NAME=.*/NAME="Fedora Linux"/' \
       -e "s/^PRETTY_NAME=.*/PRETTY_NAME=\"Fedora Linux ${VERSION_ID} (Kinoite)\"/" \
       -e 's/^LOGO=.*/LOGO=fedora-logo-icon/' /usr/lib/os-release

# Terminal greeting: Bazzite's fish alias forces its own fastfetch config over the user's.
rm -f /usr/share/fish/vendor_conf.d/bazzite-neofetch.fish /etc/profile.d/bazzite-neofetch.sh /etc/profile.d/user-motd.sh

rm -f /etc/yum.repos.d/tailscale.repo
