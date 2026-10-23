# bazzite-hadi

The OS image for pc-bazzite: Bazzite GNOME (testing) with Fedora branding, trimmed, and the machine's system
config built in. Built with [BlueBuild](https://blue-build.org).

- `recipes/recipe.yml` — base image, build stages, module order
- `recipes/shared.yml` — preferences valid on any Bazzite machine
- `recipes/pc.yml` — facts about pc-bazzite (hardware, kernel arguments, masks)
- `files/` — files copied into the image, sources for the build stages, build scripts

Home config, Flatpaks and Homebrew stay in `bazzite-system` and `dotfiles`.

## Switch to it

```
sudo bootc switch ghcr.io/hadiskeini/bazzite-hadi:latest
```

Only `bootc` applies the kernel arguments in `/usr/lib/bootc/kargs.d`. After the reboot, check `/proc/cmdline`.
