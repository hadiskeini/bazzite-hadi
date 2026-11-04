#!/usr/bin/env bash
# silverblue-vm: OmacVM's GPU parts from the stage "omacvm-gpu" into the image. The driver goes to updates/, which
# depmod puts ahead of the stock one. No driver for this image's kernel fails the build.
set -euo pipefail
in=/tmp/omacvm-gpu
kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
install -Dm755 "$in/omacvm_drv_video.so" /usr/lib64/dri/omacvm_drv_video.so
install -Dm755 "$in/libEGL_omacvm.so.0" /usr/lib64/libEGL_omacvm.so.0
if [[ ! -f $in/virtio-gpu.ko || $(cat "$in/kernel") != "$kernel" ]]; then
  echo "No OmacVM virtio-gpu driver for $kernel (see the stage omacvm-gpu)" >&2; exit 1
fi
install -Dm644 "$in/virtio-gpu.ko" "/usr/lib/modules/$kernel/updates/virtio-gpu.ko"
depmod -a "$kernel"
# modinfo gives /lib/modules/..., a symlink to /usr/lib/modules/....
resolved=$(realpath -e "$(modinfo -k "$kernel" -n virtio-gpu)")
if [[ $resolved != "/usr/lib/modules/$kernel/updates/virtio-gpu.ko" ]]; then
  echo "virtio-gpu resolves to $resolved, not to OmacVM's driver in updates/" >&2; exit 1
fi
echo "OmacVM's virtio-gpu driver for $kernel"
rm -rf "$in"
