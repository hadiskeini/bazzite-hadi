#!/usr/bin/env bash
# silverblue-vm: OmacVM's battery module from the stage "omacvm-battery", only when it was built for this image's
# kernel. Without it, nothing loads it and the agent's unit does not start (ConditionPathExists).
set -euo pipefail
in=/tmp/omacvm-battery
kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
if [[ -f $in/omacvm_battery.ko && $(cat "$in/kernel") == "$kernel" ]]; then
  install -Dm644 "$in/omacvm_battery.ko" "/usr/lib/modules/$kernel/extra/omacvm_battery.ko"
  depmod -a "$kernel"
  echo "OmacVM's battery module for $kernel"
else
  rm -f /usr/lib/modules-load.d/omacvm-battery.conf
  echo "No OmacVM battery module for $kernel (see the stage omacvm-battery)"
fi
rm -rf "$in"
