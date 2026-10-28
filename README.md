# bazzite-hadi

The OS images for two machines, built with [BlueBuild](https://blue-build.org):

- `ghcr.io/hadiskeini/bazzite-hadi` — pc-bazzite: Bazzite GNOME (testing) with Fedora branding, trimmed, and the
  machine's system config built in. x86_64.
- `ghcr.io/hadiskeini/silverblue-vm` — vm-silverblue: Fedora Silverblue 45 in OmacVM.app on a MacBook, with the
  VM's system config and the Mac camera module built in. aarch64, built on GitHub's arm64 runner.

- `recipes/recipe.yml` — pc-bazzite: base image, build stages, module order
- `recipes/shared.yml` — preferences valid on any Bazzite machine
- `recipes/pc.yml` — facts about pc-bazzite (hardware, kernel arguments, masks)
- `recipes/vm.yml` — vm-silverblue: base image, build stages, module order
- `recipes/vm-host.yml` — facts about vm-silverblue (Mesa for Venus, the camera module, OmacVM udev rules)
- `files/` — files copied into the images, sources for the build stages, build scripts
- `.github/workflows/image.yml` — one image: check inputs, build, test the ostree import, move `latest`;
  `build.yml` runs it once per image. `.github/fingerprint/<image>.sh` lists each image's inputs, so a change
  to one image does not rebuild the other.

Home config, Flatpaks and Homebrew stay in the `home` repo.

## Switch to it

```
sudo bootc switch ghcr.io/hadiskeini/bazzite-hadi:latest   # pc-bazzite
sudo bootc switch ghcr.io/hadiskeini/silverblue-vm:latest  # vm-silverblue
```

Only `bootc` applies the kernel arguments in `/usr/lib/bootc/kargs.d`. After the reboot, check `/proc/cmdline`.
