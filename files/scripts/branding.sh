#!/usr/bin/env bash
# Fedora branding and GNOME defaults instead of Bazzite's look. Runs before the gschema-overrides module, which
# recompiles the schemas, and before the initramfs module, which puts the restored boot splash into the initramfs.
set -euo pipefail

# Boot splash: Bazzite overwrote Fedora's watermark; reinstalling fedora-logos restores the original file.
dnf -y reinstall fedora-logos

# Name and logo in Settings > About, fastfetch and the boot menu. ID stays bazzite: Bazzite's own tools read it.
. /usr/lib/os-release
sed -i -e 's/^NAME=.*/NAME="Fedora Linux"/' \
       -e "s/^PRETTY_NAME=.*/PRETTY_NAME=\"Fedora Linux ${VERSION_ID} (Silverblue)\"/" \
       -e 's/^LOGO=.*/LOGO=fedora-logo-icon/' /usr/lib/os-release

# Terminal greeting: Bazzite's fish alias forces its own fastfetch config over the user's.
rm -f /usr/share/fish/vendor_conf.d/bazzite-neofetch.fish /etc/profile.d/bazzite-neofetch.sh /etc/profile.d/user-motd.sh

# Bazzite's theme, dock, app-folder and default-extension overrides; zz0-00 (behaviour) stays.
rm -f /usr/share/glib-2.0/schemas/zz0-0{1,2,3,4}-bazzite-desktop-silverblue-*.gschema.override

# Bazzite, uBlue and Steam Deck wallpapers and their picker entries (steamdeck/ keeps an unowned file after its rpm goes).
rm -rf /usr/share/backgrounds/{steamdeck,bazzite-blue.png,bazzite-glass.png,convergence,convergence.jxl,convergence-dynamic.xml,giants.jxl,penguins.jxl,ublue.png}
rm -f /usr/share/gnome-background-properties/{bazziteblue,convergence,convergence-dynamic,glass,giants,penguins,ublue}.xml

# Inactive extensions Bazzite copies in without a package (user-theme is removed with dnf).
rm -rf /usr/share/gnome-shell/extensions/{add-to-steam@pupper.space,burn-my-windows@schneegans.github.com,compiz-alike-magic-lamp-effect@hermes83.github.com,compiz-windows-effect@hermes83.github.com,desktop-cube@schneegans.github.com,hotedge@jonathan.jdoda.ca,logomenu@aryan_k,touchup@mityax}

rm -f /etc/yum.repos.d/tailscale.repo
