# OmacVM battery module for silverblue-vm

From OmacVM.app 3.2.18 (commit 61c2de6f), `src/battery/guest/module/`, which takes it from try-omarchy. The module
is GPL-2.0-only; OmacVM's other files are MIT (`LICENSE.omacvm`).

`omacvm-battery.c` shows the Mac's battery as `BAT0` and `ADP0`, so UPower and every desktop's own battery indicator
see it. The agent `/usr/libexec/omacvm-battery` (`files/vm`) feeds it from the port `org.omacvm.battery`.

Built by `files/scripts/build-omacvm-modules.sh` in the stage "omacvm-modules" of `recipes/vm.yml`.
