# vkpace

A Vulkan implicit layer that paces a game at its own `VK_AMD_anti_lag` input-stage call. Deadlock calls that
on its main thread right before it samples input, so a sleep there is the lowest-latency place to limit frames.
The game's `fps_max` must be 0: its own limiter sleeps in the same spot and would take over.

What it does per frame (all measured 2026-10-22, memory note `deadlock-antilag-hook`):

- **Absolute grid with catch-up** (`PACE_FPS`): frame *f* starts at `origin + f * T`. A late frame gets no sleep and
  the following frames sleep less until the grid is reached; more than `PACE_CATCHUP` periods late rebases the
  grid. Recovers the throughput a relative limiter loses on the engine's heavy 64 Hz tick frames (141 average
  instead of 136 at the same cap).
- **Driver anti-lag underneath** (`PACE_DRIVER=on`): the call is forwarded with the game's mode but `maxFPS` 0, so
  the Mesa 26.3 pacer does its GPU-bound just-in-time wait and no limiting. Held off for `PACE_DRIVER_COOLDOWN_MS`
  (2000) after a stall of more than three periods or a game mode change, because its timing history then
  produces seconds of stutter.
- **Flip logging** (`VK_EXT_present_timing`, enabled only when a log file or the scheduler needs it; `PRESENT_TIMING=1` forces it): the real first-pixel-out time per present from the
  compositor's presentation feedback, written as `F` lines. Steps aside when the PyroFling latency layer (Mora,
  `LATENCY_MEASUREMENT=1`) owns the swapchain's timing queue.
- **Flip-locked scheduling** (`PACE_JIT=1`): predicted light frames start up to
  `PACE_JIT_MAX_MS` later so they present just before their flip slot. Lowers average latency ~0.5 ms but makes
  consecutive frames' latency uneven (animation error); left off by decision.

The layer is always loaded and passive: without `PACE_FPS` every call is forwarded untouched and nothing is
queried or written. Defaults are the measured best: driver anti-lag on, 64 Hz tick predictor, scheduler off.
Launch options for Deadlock on pc-bazzite (the layer paces at the game's VK_AMD_anti_lag call, which RADV exposes
from Mesa 26.3; `PROTON_RADV=1` makes Proton-Wineland load its bundled RADV, which has it. `+fps_max 0` is the game's own limiter, which must be off):

```
PROTON_RADV=1 PACE_FPS=141 PROTON_ENABLE_WAYLAND=1 MANGOHUD=0 perf-hold %command% -nojoy +fps_max 0
```

`perf-hold` (dotfiles `hosts/pc-bazzite/bin/`) holds the performance power profile while the game runs: the main thread's core then stays near its
maximum clock, and tick frames overrun less often (`cpuab.py` measures it). The hold ends when the game exits.

Add `VKPACE_LOG=/path/to/file.txt` to record a session; without it the layer evaluates no log arguments, does no
formatting, no I/O, and skips the flip-timing driver queries, so the per-frame work is one mutex, a few clock reads
and the sleep. A
background writer thread does not run inside the Wine process, so writes are synchronous, every 100 lines.

Environment: `VKPACE_DISABLE=1` unload · `VKPACE_LOG` log file · `PACE_FPS` cap · `PACE_MODE=grid|period` · `PACE_CATCHUP`
(1.0) · `PACE_SPIN_US` (adaptive 50–400 µs unless set) · `PACE_DRIVER=on|off` · `PACE_DRIVER_COOLDOWN_MS` (2000) ·
`PACE_TICK_HZ` (64; 0 disables the predictor) · `PACE_JIT`, `PACE_JIT_MARGIN_MS` (0.5), `PACE_JIT_MAX_MS` (1.5),
`PACE_JIT_GPU_MS` (5.5 start value) · `PRESENT_TIMING=0` disables flip logging.

`pace_sim.c` is a game-loop simulator for chain tests without a game (build: `cc -O1 -I$(brew --prefix)/include
-o pace_sim pace_sim.c -l:libvulkan.so.1`). It needs Mesa 26.3 or later for a real anti-lag path (`VK_DRIVER_FILES` can point at Proton-Wineland's bundled RADV).
