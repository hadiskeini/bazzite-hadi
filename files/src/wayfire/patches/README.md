# Wayfire stack patches

`build.sh` applies `patches/<repo>/*.patch` in name order on top of upstream main. A patch that no longer
applies fails the stack build, and the image keeps the last good stack (`wayfire-stack:current`). That is
the first sign that upstream changed the code; then check whether upstream fixed the problem.

Each patch starts with a header: the problem, the fix, the upstream status, and when and how to remove it.
`git apply` skips the text before the first `diff --git` line.

| Patch | Problem | Upstream | Remove when |
|---|---|---|---|
| `wayfire/0001-direct-scanout-at-fractional-scale.patch` | No direct scanout at scale 1.5 | [#3253](https://github.com/WayfireWM/wayfire/issues/3253), maintainer agrees | Fix on main |
| `wayfire/0002-keep-above-state-set-before-map.patch` | `on created` always_on_top is lost at map (PiP rules) | Not reported | Upstream keeps the state at map |
| `wayfire/0003-fractional-xwayland-cursor-scale.patch` | X cursors scaled by an integer (1.5 → 1): too large or blurry | Not reported | Upstream uses a fractional scale |
| `wayfire/0004-srgb-in-8-bit-aux-buffers.patch` | Moved/transformed windows change tint (Vulkan, linear values in 8-bit buffers) | Not reported | Upstream picks the encoding by precision |
| `wayfire/0005-sdr-output-gamma-2.2.patch` | Composited windows have crushed shadows; scanned-out games look muted | Not reported | Upstream encodes SDR outputs with gamma 2.2 |

To remove a patch: delete the file, remove its row here, and remove what depends on it (named in its header).
