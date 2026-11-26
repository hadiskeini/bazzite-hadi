#!/usr/bin/env bash
# silverblue-vm: RPM Fusion's VA-API drivers, for H.264 and HEVC on the Mac's media engine (Fedora's Mesa builds
# without them). The package needs the image's exact Mesa version; when RPM Fusion lags, the image keeps Fedora's
# drivers (VP9 only) instead of failing. Its ld.so.conf.d entry makes its libgallium, a full build, the system's one.
set -euo pipefail

mesa=$(rpm -q --queryformat '%{VERSION}-%{RELEASE}\n' mesa-dri-drivers)
dnf -y install "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
if dnf -y install --setopt=install_weak_deps=False "mesa-va-drivers-freeworld-$mesa"; then
  test -e /usr/lib64/dri-freeworld/virtio_gpu_drv_video.so
else
  echo "vm-va-freeworld: RPM Fusion has no mesa-va-drivers-freeworld-$mesa yet: Fedora's VA-API drivers stay"
fi
dnf -y remove rpmfusion-free-release
