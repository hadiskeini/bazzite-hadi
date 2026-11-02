#!/usr/bin/env bash
# Build stage "omacvm-battery" (silverblue-vm): OmacVM's battery module from /src (files/src/omacvm-battery) for the
# base image's kernel, into /out (omacvm_battery.ko, kernel). A module that does not build leaves the image without it.
set -euo pipefail

kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
dnf -y install "kernel-devel-$kernel" gcc make kmod elfutils-libelf-devel
mkdir -p /out
if ! make -s -j"$(nproc)" -C /src KDIR="/usr/src/kernels/$kernel" modules; then
  echo "omacvm-battery: the module does not build on $kernel: the image has no Mac battery"; exit 0
fi
[[ $(modinfo -F vermagic /src/omacvm_battery.ko) == "$kernel "* ]]
cp /src/omacvm_battery.ko /out/
echo "$kernel" > /out/kernel
ls -l /out
