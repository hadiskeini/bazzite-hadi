#!/usr/bin/env bash
# silverblue-vm: OmacVM's kernel modules from the stage "omacvm-modules", each only when it was built for this
# image's kernel. Without one, nothing loads it (its modules-load.d file goes) and its agent finds no device.
set -euo pipefail
in=/tmp/omacvm-modules
kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
for name in omacvm_battery omacvm_backlight; do
  conf=/usr/lib/modules-load.d/${name//_/-}.conf
  if [[ -f $in/$name.ko && $(cat "$in/kernel") == "$kernel" ]]; then
    install -Dm644 "$in/$name.ko" "/usr/lib/modules/$kernel/extra/$name.ko"
    echo "$name for $kernel"
  else
    rm -f "$conf"
    echo "No $name for $kernel (see the stage omacvm-modules)"
  fi
done
depmod -a "$kernel"
rm -rf "$in"
