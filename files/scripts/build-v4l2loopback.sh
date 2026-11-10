#!/usr/bin/env bash
# Build stage "v4l2loopback" (silverblue-vm): builds RPM Fusion's v4l2loopback module for the kernel of the base image
# and leaves the kmod package and its userspace package in /out. The stage runs on the same base as the image, so
# `rpm -q kernel` is the kernel that boots. Universal Blue's prebuilt kmods do not exist for aarch64.
set -euo pipefail

kernel=$(rpm -q kernel --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | tail -n1)
dnf -y install "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
# The mirrors can lag a new kernel by a day or more; Koji has every build. kernel-devel-matched comes along:
# akmods requires it, and the mirrors' older one pins an older kernel-devel.
install_kernel_devel() {
  dnf -y install "kernel-devel-$kernel" "$@" && return
  local rest=${kernel#*-}
  local koji="https://kojipkgs.fedoraproject.org/packages/kernel/${kernel%%-*}/${rest%.*}/${rest##*.}"
  dnf -y install "$koji/kernel-devel-$kernel.rpm" "$koji/kernel-devel-matched-$kernel.rpm" "$@"
}
install_kernel_devel akmods
# Its %post builds for the running kernel, which in CI is the runner's; the build below names the image's kernel.
dnf -y install --setopt=tsflags=noscripts akmod-v4l2loopback
akmods --force --kernels "$kernel" --kmod v4l2loopback
modinfo -k "$kernel" v4l2loopback > /dev/null || { cat /var/cache/akmods/v4l2loopback/*.failed.log; exit 1; }

mkdir -p /out
cp /var/cache/akmods/v4l2loopback/kmod-v4l2loopback-*.rpm /out/
dnf download --destdir /out v4l2loopback
ls -l /out
