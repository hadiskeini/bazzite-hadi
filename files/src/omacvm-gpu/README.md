# OmacVM GPU parts for silverblue-vm

From OmacVM.app 3.2.18 (commit 61c2de6f), `src/app/guest/`, MIT licence (`LICENSE.omacvm`). The `linux-*.patch`
files change the Linux virtio-gpu driver (GPL-2.0); the module built from them is GPL-2.0.

- `linux-virtio-gpu-*.patch`: the virtio-gpu driver with the module option `cursor_plane` (a cursor plane for
  compositors without cursor hotspots, as niri and Hyprland) and Linux 7.4's damage fix (the Mac draws only what changed).
- `omacvm_drv_video.c`: VA-API driver shim in front of Mesa's virtio_gpu driver (video decoding on the Mac).
- `omacvm_egl.c`: glvnd EGL vendor library in front of Mesa (picture size of decoded video; Ghostty's driver: Zink
  with OmacVM's switch "Ghostty on the Mac's GPU", else llvmpipe).

Built by `files/scripts/build-omacvm-gpu.sh` in the stage "omacvm-gpu" of `recipes/vm.yml`.
