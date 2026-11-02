#!/usr/bin/env bash
# silverblue-vm: OmacVM's GPU parts from the stage "omacvm-gpu" into the image. The driver only when it was built
# for this image's kernel (in updates/, which depmod puts ahead of the stock one); else the stock driver stays.
set -euo pipefail
in=/tmp/omacvm-gpu
kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
install -Dm755 "$in/omacvm_drv_video.so" /usr/lib64/dri/omacvm_drv_video.so
install -Dm755 "$in/libEGL_omacvm.so.0" /usr/lib64/libEGL_omacvm.so.0
if [[ -f $in/virtio-gpu.ko && $(cat "$in/kernel") == "$kernel" ]]; then
  install -Dm644 "$in/virtio-gpu.ko" "/usr/lib/modules/$kernel/updates/virtio-gpu.ko"
  depmod -a "$kernel"
  echo "OmacVM's virtio-gpu driver for $kernel"
else
  # Without the patched driver the option is unknown to the stock one.
  rm -f /usr/lib/modprobe.d/90-omacvm-virtio-gpu.conf
  echo "No OmacVM virtio-gpu driver for $kernel (see the stage omacvm-gpu): the stock driver stays"
fi
rm -rf "$in"
