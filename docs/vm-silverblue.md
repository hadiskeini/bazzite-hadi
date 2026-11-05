# vm-silverblue: Fedora Silverblue in OmacVM.app

The image `silverblue-vm` is Fedora Silverblue (aarch64) with niri for a VM in
[OmacVM.app](https://github.com/gillesgoetsch/OmacVM) on an Apple Silicon Mac. It has OmacVM's hardware
integration without Omarchy: GPU parts (`files/src/omacvm-gpu`), the battery and backlight modules, and the
guest agents in `files/vm`. Home config is in `~/home` (host `vm-silverblue`); the Mac's files are in
`~/home/hosts/vm-silverblue/mac/`. Check the VM with `omacvm-status`.

## Set up a new VM

1. On the Mac, make the VM folder from Fedora's Silverblue disk image (`*.raw.xz`):
   `~/home/hosts/vm-silverblue/mac/make-vm-folder.sh Fedora-Silverblue-Disk-….aarch64.raw.xz`.
2. Start OmacVM.app and complete GNOME's first setup. The app opens the first VM folder by name;
   another VM: `open -a OmacVM --args --vm NAME`.
3. In the VM, as root:
   - Grow the root partition (the disk image's root is 7.5 GB): `echo ', +' | sfdisk --no-reread -N 3 /dev/nvme0n1`,
     `partx -u /dev/nvme0n1`, `resize2fs /dev/nvme0n1p3`.
   - Remove the disk image's first-boot stamp (else systemd resets every service to presets at each boot):
     `rm -f /boot/ignition.firstboot`.
   - `rpm-ostree rebase ostree-unverified-registry:ghcr.io/hadiskeini/silverblue-vm:latest`, then reboot.
   - Copy the Mac's Touch ID key to `/etc/omacvm/touchid-key` (`install -m600`).
4. In the VM folder on the Mac: `venus-ready` (the image's Mesa is new enough), and in `features`
   `scroll-momentum=on` and `bridge=on`. Shut the VM down and start it again: the app reads `features` only at
   start.
5. Home: `~/home` (`bootstrap.sh`, then `sys`).

## What the image has from OmacVM

| Part | Files | Notes |
|---|---|---|
| The Mac's settings at start | `omacvm-app-host` (system) | SMBIOS strings to `/run/omacvm/host.env`: Ghostty's GPU switch, FullPanel, Vulkan windows. |
| Trackpad gestures | `omacvm-gestures` (system) | A three-finger tap is a middle click (`OMACVM_TAP3_TIME`, `OMACVM_TAP3_MOVE`); keep tap-to-click off. |
| Mouse side buttons, Caps Lock | `omacvm-side-buttons` (user) | Needs `mac/side-buttons.lua` and `mac/caps-lock.lua` in Hammerspoon. `niri-at-pointer` (`~/home`) uses its "ping" and "click". |
| Mac camera, Mac USB webcam | `omacvm-camera`, `omacvm-usbcam` (user), v4l2loopback | `/dev/video42` "Mac Camera", `/dev/video43` "USB Camera" (needs `mac/webcam.lua`). |
| Sound delay | `omacvm-audio-latency` (user, and the app through the guest agent) | The app calls `/usr/local/bin/omacvm-audio-latency`: a tmpfiles link to `/usr/libexec/qemu-ga/fsfreeze-hook.d/`, where Fedora's policy gives the label the agent may run. The freeze hook runs only `/etc/qemu-ga/fsfreeze-hook.d`. |
| Mac folder at `~/Mac` | `omacvm-mac-folder` (system) | virtio-9p, when the app shares a folder. |
| Touch ID for sudo, sudo -i, polkit | `omacvm-touchid`, `omacvm-touchid-note`, PAM lines (`files/scripts/install-omacvm-touchid.sh`), polkit rule and drop-in | The key is the only part not in the image. The password works either way. |
| Clipboard, Mac volume and brightness, battery | `omacvm-clipboard`, `omacvm-bridge-events`, `omacvm-mac-levels`, `omacvm-battery` | |
| niri helpers | `omacvm-display-hello`, `omacvm-niri-scale` | Started from niri's host config in `~/home`. |

GNOME's integration (the `omacvm-integration@local` extension, `omacvm-displays`) is not in the image: this VM
uses niri only.

## Rules

- Never run upstream's guest setup on this VM: `omacvm apply`, `omacvm enable/disable`, `omacvm update`, or the
  app's "Update VM". They run the Omarchy installer over SSH.
- After an app update, write the app's version into the VM folder's `omacvm-version`, else the app offers
  "Update VM" again.
- After the app enlarges the disk, grow partition 3 by hand (step 3 above) and delete the VM folder's
  `disk-jobs`: the app's own step works only for Omarchy.
- Do not set the app's `macPointer`: the pointer becomes invisible.
- One display at a time (default): the VM stays in one window, the only one with the app's 120 Hz path. With the
  lid closed it moves to the external display, and `omacvm-niri-scale` sets the scale stored for that size in
  `~/.local/state/omacvm/display-scales.json`.
- Full screen on several Mac displays: bringing the app to the front shows only the main window's display; on the
  other display swipe to its VM Space (or Ctrl+arrow there). Hammerspoon cannot do it: `hs.spaces.gotoSpace`
  fails for full-screen Spaces on a second display, and the VM grabs Ctrl+arrow.
- Volume, brightness and keyboard light: OmacVM Bridge on the Mac takes those keys while the VM is in front and
  sets the Mac's own; the VM shows the level in Dux Shell's OSD. Keep the VM's own volume at 100 %. Set up once:
  - Mac: `~/Applications/OmacVM.app/Contents/Resources/omacvm/src/bridge/mac/install.sh --prebuilt
    ~/Applications/OmacVM.app/Contents/Helpers/OmacVMBridge.app`, and `bridge=on` in the VM folder's `features`.
  - VM: copy the Mac's `~/Library/Application Support/omacvm-bridge/token` to `~/.config/omacvm-bridge/token` (0600).
  - niri: switch on the plugin "Mac volume and brightness" in Dux Shell's Settings -> Plugins.
- Caps Lock as Super: macOS sends Caps Lock to the VM only as a toggle, so `mac/caps-lock.lua` makes it F17 with
  `hidutil` while OmacVM is in front, and `omacvm-side-buttons` turns F17 back into a held Caps Lock.
- Clicks on the screen edge in full screen: `mac/screen-edges.lua` moves a press on the outermost point one point
  inward. There macOS starts to resize the VM window, and the VM gets no events until the release.
- A three-finger click cannot be told apart: while OmacVM is in front, macOS gives Hammerspoon no touch events.
- Mac USB webcam: `mac/webcam.lua` runs `mac/webcam-server.py` (port 5800, the VM's network only; macOS asks once
  for the camera for Hammerspoon). `omacvm-usbcam` connects only while an app reads the camera. Default camera:
  `OMACVM_WEBCAM`.
- No Omarchy start animation: `mac/local.omacvm.boot-splash.plist` in `~/Library/LaunchAgents` sets
  `OMACVM_BOOT_SPLASH=0` for the app at login (then quit and start OmacVM.app once).
- Full screen including the notch (FullPanel, experimental) starts only when the VM folder has `notch-mode` =
  `fullpanel`, `fullpanel-ready`, and `omanotch=on` in `features`; else the app says why in `logs/qemu.log`.

## Known issues

- Ghostty needs OpenGL 4.3, and virgl gives 4.1. With the app's switch "Ghostty on the Mac's GPU" (3.2.18 or
  later), `libEGL_omacvm` starts Ghostty, and only Ghostty, on Zink with OpenGL 4.3; with the switch off, on
  llvmpipe. Not seen here yet (since 2026-11-05): when the Mac ends Ghostty's Vulkan context (a refused command,
  or the apps' graphics memory is full), that Ghostty freezes, and no new Ghostty window opens while it runs. The
  VM folder's `logs/gpu-memory` on the Mac names the lost context. On Omarchy the app then runs
  `/usr/local/bin/omacvm-desktop-recover venus ghostty`, which writes `/run/omacvm/gl-zink-off`; this image does
  not have that script until a freeze happens.
  - When it happens: `pkill -x ghostty`; to keep Ghostty on the CPU until the next boot,
    `sudo touch /run/omacvm/gl-zink-off`.
  - When it happens again: add the `venus` case of upstream's `src/app/guest/omacvm-desktop-recover`, linked to
    `/usr/local/bin` like the sound delay.
- GTK 4 apps that play video through OpenGL on virgl lose their GL context when the video goes away
  (`gdk_gl_context_make_current() failed`); the window stops drawing.

## Upstream

The agents in `files/vm` came from silverblue-omacvm (github.com/hadiskeini/silverblue-omacvm, archived) at
commit `8f76930`. Its copies were from OmacVM at commit `51083a5` (`omacvm-app-host`: `6810c07`); local changes
since then: `omacvm-gestures` (three-finger tap), `omacvm-side-buttons` (Caps Lock, "ping" and "click"). In the
image the paths changed from `/usr/local/bin` and `/usr/local/libexec/omacvm` to `/usr/libexec`. Licences:
`/usr/share/licenses/omacvm-guest/`.
