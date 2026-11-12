# Quirks

Workarounds that exist only because of a gap somewhere else. Check each one now and then against its
"Check" column; remove it when that gap is closed. Paths without a host are in this repository.

## vm-silverblue (OmacVM.app on the MacBook)

| Quirk | Where | Why | Check |
|---|---|---|---|
| Linux 7.4's damage fix in OmacVM's patched virtio-gpu driver | `linux-virtio-gpu-damage-clips.patch` | Before 7.4 every frame went to the Mac whole. | Goes by itself: the stage skips it on a kernel that has the fix (Fedora on 7.4). |
| VA-API shim in front of Mesa's virtio_gpu driver, `LIBVA_DRIVER_NAME=omacvm` | `omacvm_drv_video.c`, `files/vm/usr/lib/environment.d/90-omacvm-video.conf` | Video decoding on the Mac's media engine. Mesa's driver offers I420, which Firefox cannot show, and plane sizes Chromium rejects. Mesa's path changed from Arch's `/usr/lib/dri` to Fedora's `/usr/lib64/dri`. | Mesa's virgl VA driver offers NV12 only and real plane sizes. |
| glvnd EGL library `libEGL_omacvm` | `omacvm_egl.c`, `files/vm/usr/share/glvnd/egl_vendor.d/10_omacvm.json` | Mesa's virgl does not pass the cropped size of a decoded picture to the Mac (1080p shows 8 extra rows). It also picks Ghostty's driver: Ghostty needs OpenGL 4.3 and virgl gives 4.1, so Zink on Venus with OpenGL 4.3 when OmacVM's switch "Ghostty on the Mac's GPU" is on (`OMACVM_GLZINK=1` in `/run/omacvm/host.env`), else llvmpipe. A Ghostty on Zink freezes when the Mac ends its Vulkan context: `docs/vm-silverblue.md`, Known issues. | Mesa's virgl passes the crop, and virgl offers OpenGL 4.3. |
| drirc: GL renderer named "OmacVM-GPU" for Chrome, Brave, Electron | `files/vm/usr/share/drirc.d/50-omacvm-chrome.conf`; for the Brave Flatpak: `flatpak override --show com.brave.Browser` (`force_gl_renderer`) | Skia turns instanced drawing off when the renderer names virgl: canvas paths and clips on the CPU. | Skia (GrGLCaps.cpp) no longer singles out virgl. |
| `MESA_VK_WSI_DEBUG=nowlts` (or `sw`) | `files/vm/usr/lib/systemd/user-environment-generators/60-omacvm-vulkan-present` | The VM's frame clock runs a few ms late; with commit timing, Vulkan windows with FIFO lost frames (45-50 on 60 Hz). | OmacVM.app drops it from its own guest files. |
| Mesa from updates-testing | `files/scripts/vm-mesa.sh` | Venus needs Mesa 26.2.4 or newer. | Goes by itself once stable Fedora has it; then remove the script. |
| Flatpak apps draw with OpenGL (`GSK_RENDERER=gl`) | VM: `~/.local/share/flatpak/overrides/global` | Flathub's Mesa has no Venus driver for aarch64 (no `virtio_icd`), so Vulkan in a Flatpak would be lavapipe on the CPU. OpenGL goes through virgl, where a new app's shader compiles stall every compositor's frames for 100-460 ms (Mission Center, Bazaar). | `org.freedesktop.Platform.GL.default` has `virtio_icd.aarch64.json`. |
| Chromium and Electron apps draw with OpenGL | none (no setting helps) | Chromium has no Vulkan output on Wayland ("--ozone-platform=wayland is not compatible with Vulkan"). Same virgl stalls as above at app start. | Chromium supports Vulkan (Graphite/Dawn) on Wayland. |
| Camera and side-button units not in the greeter (`ConditionUser=!gdm-greeter`) | `files/vm/usr/lib/systemd/user/omacvm-{camera,usbcam,side-buttons}.service` | GDM 49 runs its login screen as the user gdm-greeter (uid above 999, not `@system`): its camera service took the device first, the side buttons crash-looped. | Systemd user units no longer start for GDM's greeter by default. |
