#!/usr/bin/env bash
# silverblue-vm: RPM Fusion's builds with H.264 and HEVC, which Fedora leaves out (patents).
#   mesa-va-drivers-freeworld  VA-API decode and encode on the Mac's media engine (Fedora's: VP9 only)
#   libavcodec-freeworld       FFmpeg's native H.264/HEVC decoders: Firefox's VA-API path needs them to parse the stream
# Each needs the exact version of Fedora's package. When RPM Fusion lags, Fedora's stays instead of failing the build.
# Their ld.so.conf.d entries put both ahead of Fedora's copies; the freeworld libgallium is a full build.
set -euo pipefail

dnf -y install "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
take() {
  local pkg=$1 base=$2 files=$3 ver
  ver=$(rpm -q --queryformat '%{VERSION}-%{RELEASE}\n' "$base")
  if dnf -y install --setopt=install_weak_deps=False "$pkg-$ver"; then
    compgen -G "$files" > /dev/null
  else
    echo "vm-freeworld: RPM Fusion has no $pkg-$ver yet: Fedora's $base stays"
  fi
}
take mesa-va-drivers-freeworld mesa-dri-drivers '/usr/lib64/dri-freeworld/virtio_gpu_drv_video.so'
take libavcodec-freeworld libavcodec-free '/usr/lib64/ffmpeg/libavcodec.so.*'
dnf -y remove rpmfusion-free-release
