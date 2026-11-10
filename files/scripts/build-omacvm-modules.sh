#!/usr/bin/env bash
# Build stage "omacvm-modules" (silverblue-vm): OmacVM's small kernel modules from /src (files/src/omacvm-battery,
# files/src/omacvm-backlight) for the base image's kernel, into /out (*.ko, kernel). A module that does not build is
# left out; the image then lacks only that one.
set -euo pipefail

kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
# The mirrors can lag a new kernel by a day or more; Koji has every build. kernel-devel-matched comes along:
# akmods requires it, and the mirrors' older one pins an older kernel-devel.
install_kernel_devel() {
  dnf -y install "kernel-devel-$kernel" "$@" && return
  local rest=${kernel#*-}
  local koji="https://kojipkgs.fedoraproject.org/packages/kernel/${kernel%%-*}/${rest%.*}/${rest##*.}"
  dnf -y install "$koji/kernel-devel-$kernel.rpm" "$koji/kernel-devel-matched-$kernel.rpm" "$@"
}
install_kernel_devel gcc make kmod elfutils-libelf-devel
mkdir -p /out
for src in /src/*/; do
  if ! make -s -j"$(nproc)" -C "/usr/src/kernels/$kernel" M="${src%/}" modules; then
    echo "omacvm-modules: ${src} does not build on $kernel: left out"; continue
  fi
  for ko in "$src"*.ko; do
    [[ $(modinfo -F vermagic "$ko") == "$kernel "* ]]
    cp "$ko" /out/
  done
done
echo "$kernel" > /out/kernel
ls -l /out
