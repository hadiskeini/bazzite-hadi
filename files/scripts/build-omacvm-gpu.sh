#!/usr/bin/env bash
# Build stage "omacvm-gpu" (silverblue-vm): OmacVM's GPU parts from /src (files/src/omacvm-gpu) for the base image's
# kernel and Mesa, into /out. The stage runs on the same base as the image, so `rpm -q kernel` is the kernel that boots.
#   /out/virtio-gpu.ko         the virtio-gpu driver with OmacVM's patches
#   /out/omacvm_drv_video.so   VA-API shim in front of Mesa's virtio_gpu driver
#   /out/libEGL_omacvm.so.0    glvnd EGL vendor library in front of Mesa's
# The build fails when the patched driver does not fit or build for the kernel, so :latest stays on the last image
# with it. The damage and HDR patches stay optional.
set -euo pipefail

kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
version=${kernel%%-*}
[[ $version == *.*.0 ]] && version=${version%.0}   # tag v7.2, not v7.2.0
# The mirrors can lag a new kernel by a day or more; Koji has every build. kernel-devel-matched comes along:
# akmods requires it, and the mirrors' older one pins an older kernel-devel.
install_kernel_devel() {
  dnf -y install "kernel-devel-$kernel" "$@" && return
  local rest=${kernel#*-}
  local koji="https://kojipkgs.fedoraproject.org/packages/kernel/${kernel%%-*}/${rest%.*}/${rest##*.}"
  dnf -y install "$koji/kernel-devel-$kernel.rpm" "$koji/kernel-devel-matched-$kernel.rpm" "$@"
}
install_kernel_devel gcc make patch kmod elfutils-libelf-devel libva-devel libglvnd-devel
mkdir -p /out

# The shims. Mesa's VA driver is in /usr/lib64/dri on Fedora, not in Arch's /usr/lib/dri.
sed 's|"/usr/lib/dri/virtio_gpu_drv_video.so"|"/usr/lib64/dri/virtio_gpu_drv_video.so"|' /src/omacvm_drv_video.c > /tmp/omacvm_drv_video.c
grep -q '"/usr/lib64/dri/virtio_gpu_drv_video.so"' /tmp/omacvm_drv_video.c
gcc -shared -fPIC -O2 -o /out/omacvm_drv_video.so /tmp/omacvm_drv_video.c -ldl
gcc -shared -fPIC -O2 -fvisibility=hidden -Wl,-soname,libEGL_omacvm.so.0 -o /out/libEGL_omacvm.so.0 /src/omacvm_egl.c -ldl

# The driver: the stable release's sources of drivers/gpu/drm/virtio, OmacVM's patches on them, built out of tree.
work=/tmp/virtio-gpu
src=$work/drivers/gpu/drm/virtio
base=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/gpu/drm/virtio
mkdir -p "$src"
for f in Makefile virtgpu_debugfs.c virtgpu_display.c virtgpu_drv.c virtgpu_drv.h virtgpu_fence.c virtgpu_gem.c \
         virtgpu_ioctl.c virtgpu_kms.c virtgpu_object.c virtgpu_plane.c virtgpu_prime.c virtgpu_submit.c \
         virtgpu_trace.h virtgpu_trace_points.c virtgpu_vq.c virtgpu_vram.c; do
  curl --fail --silent --show-error --location --retry 3 -o "$src/$f" "$base/$f?h=v$version"
done
# No fuzz: a hunk must fit exactly, so none lands in the wrong place of a newer kernel.
fits() { patch --dry-run -s -d "$work" -p1 -f -F 0 -i "/src/$1" > /dev/null; }
apply() { patch -s -d "$work" -p1 -f -F 0 -i "/src/$1"; }
if ! fits linux-virtio-gpu-deep-color.patch; then
  echo "omacvm-gpu: OmacVM's patches do not fit Linux $version" >&2; exit 1
fi
apply linux-virtio-gpu-deep-color.patch    # the base the other patches apply on; deep_color stays off
if ! fits linux-virtio-gpu-cursor-plane.patch; then
  echo "omacvm-gpu: the cursor-plane patch does not fit Linux $version" >&2; exit 1
fi
apply linux-virtio-gpu-cursor-plane.patch
# Linux 7.4 has the damage fix itself; the patch replaces the line older kernels have.
if grep -q 'new_plane_state->ignore_damage_clips = true;' "$src/virtgpu_plane.c"; then
  if fits linux-virtio-gpu-damage-clips.patch; then apply linux-virtio-gpu-damage-clips.patch
  else echo "omacvm-gpu: the damage fix does not fit Linux $version: left out"; fi
fi
if fits linux-virtio-gpu-hdr-metadata.patch; then apply linux-virtio-gpu-hdr-metadata.patch
else echo "omacvm-gpu: the HDR metadata patch does not fit Linux $version: left out"; fi
# Out of tree, the trace header sits next to the sources.
sed -i 's|^#define TRACE_INCLUDE_PATH .*|#define TRACE_INCLUDE_PATH .|' "$src/virtgpu_trace.h"
echo 'CFLAGS_virtgpu_trace_points.o := -I$(src)' >> "$src/Makefile"
if ! make -s -j"$(nproc)" -C "/usr/src/kernels/$kernel" M="$src" CONFIG_DRM_VIRTIO_GPU=m modules; then
  echo "omacvm-gpu: the patched driver does not build on Linux $version" >&2; exit 1
fi
[[ $(modinfo -F vermagic "$src/virtio-gpu.ko") == "$kernel "* ]]
modinfo -F parm "$src/virtio-gpu.ko" | grep -q '^cursor_plane:'
cp "$src/virtio-gpu.ko" /out/
echo "$kernel" > /out/kernel
ls -l /out
