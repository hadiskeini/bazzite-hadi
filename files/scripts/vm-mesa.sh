#!/usr/bin/env bash
# Mesa for silverblue-vm: Vulkan through Venus needs Mesa >= 26.2.4. Until Fedora's stable updates have it, Mesa
# comes from updates-testing; after that this takes the stable one, and the script can go.
set -euo pipefail

need=26.2.4
stable=$(dnf repoquery --quiet --latest-limit=1 --queryformat '%{version}\n' --repo=fedora,updates mesa-dri-drivers | tail -n1)
newest() { printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n1; }
if [ "$(newest "$stable" "$need")" = "$stable" ]; then
  echo "Mesa $stable from the stable repos"
  dnf -y upgrade 'mesa-*'
else
  echo "Stable Mesa is $stable, below $need: taking Mesa from updates-testing"
  dnf -y upgrade --enablerepo=updates-testing 'mesa-*'
fi
installed=$(rpm -q --queryformat '%{VERSION}\n' mesa-dri-drivers)
[ "$(newest "$installed" "$need")" = "$installed" ] || { echo "Mesa $installed is below $need" >&2; exit 1; }
