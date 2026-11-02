# OmacVM backlight module for silverblue-vm

The Mac's display brightness as the backlight `omacvm` (0-100): the desktops' own brightness slider, keys and popup
work in the VM. The module only stores the value; `/usr/libexec/omacvm-mac-levels` (`files/vm`) keeps it equal to the
Mac's through OmacVM Bridge. GPL-2.0-only.

Built by `files/scripts/build-omacvm-modules.sh` in the stage "omacvm-modules" of `recipes/vm.yml`.
