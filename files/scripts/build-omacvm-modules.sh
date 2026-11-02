#!/usr/bin/env bash
# Build stage "omacvm-modules" (silverblue-vm): OmacVM's small kernel modules from /src (files/src/omacvm-battery,
# files/src/omacvm-backlight) for the base image's kernel, into /out (*.ko, kernel). A module that does not build is
# left out; the image then lacks only that one.
set -euo pipefail

kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
dnf -y install "kernel-devel-$kernel" gcc make kmod elfutils-libelf-devel
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
