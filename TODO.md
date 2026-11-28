# TODO

What is owed, as opposed to what is known. `DESIGN.md` is the record of
what was measured; an item answered there, with its measurement, comes off
this list.

Only what can be done with what is here -- this desktop, the Pi 4 and
schenker's Linux -- is listed. What needs hardware the lab does not have and
will not get (an HDR panel, a Mac, a Windows laptop, FreeBSD on a machine, a
tablet, a second monitor of one model, a GPU that can be plugged) is not a
to-do: it is recorded as untested in DESIGN *Compatibility*, *Never run on
the hardware*.

## Reports drafted, not sent

Each is written with its reproduction and wants the account that files it.

- **Mesa, two X11 software-WSI reports**: `docs/mesa-x11-sw.md`.
- **Mesa display WSI**: `docs/mesa-wsi-display.md` (the plane kept for a
  connector given another CRTC, and the formats leak that fails a switching
  `asan` round), and the comment for Mesa #15248 beside it.
- **Mesa and drm/nouveau, runtime suspend garbling tiled images in VRAM**:
  `docs/mesa-nvk-pascal.md` -- the comment on #16729, the drm/nouveau
  report, the comment on #16535.
- **NVIDIA, the X11 stale swapchain**: `docs/nvidia-x11-stale-swapchain.md`,
  with `tools/probe/wsidrag.c` and an `nvidia-bug-report.log`.
- **weston, a held update applied with xdg_surface state after it**:
  `docs/weston-held-update-xdg-state.md`, after `tools/probe/heldgeom.c` has
  been run on 16.0.2.
- **wine**: `VK_PROCESSKEY` twice (`docs/wine-processkey-twice.md`), the
  parked client surface (`docs/wine-parked-surface.md`, with
  `tools/probe/parkedchild.c`), a wheel step lost on Wayland
  (`docs/wine-wheel-merge.md`), and the grown part of a window taking no
  pointer under its Wayland driver with more than one output.
- **Intel, the swapchain stall**: `docs/intel-swapchain-stall.md`.

## Defects found by reading, not yet fixed

- **Optimisations measured and not taken:** the window image's depth is drawn
  and cleared for frames that never test it; Win32's `seed_held_keys()` asks
  about 230 scancodes per focus gain; Cocoa runs `update_scale()` twice for one
  backing change and `screens_named()` is O(n^2) per forced publish; KMS
  re-enumerates the CRTCs five times per mode set; X11 asks a whole property
  before checking its limit, re-reads `_NET_WM_STATE` inside dispatch, retries
  a refused pointer grab on every motion while a capture is suspended, and pays
  three round trips for the default cursor before the opening waits on a
  scaled desktop.
- **The first present's own time under wine on Wayland** is logged nowhere:
  one round in eight says "no frame at the new size" under load (the first
  present at a new size takes 110 to 175 ms at 4K against `size_await()`'s
  250).

## Checks that can be run here and have not been

- **Wayland's wheel rule** (`ptr_axis_discrete()`: a notch's count less the
  smooth parts already delivered, so a smooth-only wheel scrolls and a hi-res
  one is not counted twice): the testbed's `EV scroll` echo against a hi-res
  and a notched wheel, on GNOME and weston -- whole notches in, whole notches
  out.
- **A move to a monitor of another scale**, which this desk has (DP-1 and
  DVI-D-1): the window must rebuild at the scale of the output it lands on, the
  one thing a same-scale pair cannot show.
- **Wine 11.0's rounds today**, against the 10.0 ones that passed: on
  `win64-vulkan` and `win64-d3d12`, a copy that reaches no other program and a
  paste that never arrives; on `win64-d3d12`, the second window's close button
  leaves two windows; under the Wayland driver, a move between monitors that
  stays on `DISPLAY2`, then a press on the titlebar that reaches nothing and a
  lost window. The same failures stand at `5735b47`, before the 1.0 work, so
  they are wine's or the harness's reckoning from it. Run again at `7f03aca`
  (2026-11-28, the `wine` on `PATH` here being 11.0): the same, the close
  button's failure falling on one of the two Direct3D 12 and Vulkan rounds
  and then the other, so a flake; a Win32 program setting `CF_UNICODETEXT`
  reaches `xclip` under 11.0, so the copy and paste failures are the
  testbed's path under it or the harness's. `FNL_DRIVE_WINE=staging` and
  `devel` (both 11.22) draw 1 to 2 fps in the window and fail the
  minimized, text-input and Escape checks, and Direct3D 12 there finds no
  window: no round of theirs says more until that is read. The round's
  two-window section now walks the raw mouse registration (a wheel over each
  window, a capture in each in turn, the second hidden under the first's hold
  and shown again): clean on `x11-vulkan` and on both wine rounds, with one
  wine fact it had to learn -- the click that captures the shown-again second
  window hands the focus to the first for a moment, and the captured motion
  made then arrives nowhere, so the section waits for the focus and asks it
  back.
- **The Pi's `tsan` round** fails on lavapipe's own race (the
  `_mesa_hash_table_copy` one `tsan.supp` covers), which the ARM library has
  no symbols for, so the existing key cannot match: a symbol source for it,
  the distribution's debug info or a Mesa built for ARM with its own.
- **The copy path's 33 ms hiccup on schenker's eDP with the GTX 1050 Ti**:
  three or four runs in ten miss every vblank (p99 33.3 ms), the rest draw
  every frame in 16.7. Not the GPU clock, the ranking, a fixed hold, the
  controller's decision or the copy's serialisation (each measured); the bad
  runs have in common a whole-run GPU time of 1.82 ms against 2.9 to 3.7. It
  wants a trace of a bad run itself (`X_LATELOG` under the harness), or a
  comparison with another program on that GPU.
- **A second lit output costs a KMS open about 200 ms** (343 to 368 ms warm
  against 126 to 150 with HDMI off), all of it in the open: `open_output()`
  reads every connector and `crtcs_save()` every lit CRTC. Which outputs a KMS
  window lights is a policy question (DESIGN *KMS: no window server*).

## After 1.0: what the 1.0 plan left out

Each is additive in a 1.x and is listed as a non-goal in the header's
*stability* block (docs/api-1.0-plan.md):

- window icon (B3); cursor names beside the key pair (B12);
  `FNL_CURSOR_NONE` and capture without a click (B11);
- minimum and maximum size, window flags (always-on-top, opacity, attention),
  the work area, a runtime move (B9, B10, B13, B4);
- a per-pixel motion texture for TAAU, and with it FSR 2, which would also
  want compute shaders in the three renderers (the tree has none) and the
  passes ported to C11 -- a multi-session subsystem, Vulkan first (B16);
- Direct3D 12 above feature level 11_0 and Metal capability requests, as
  Vulkan's `FnlVulkanSetup` is (B25, B26);
- gamepad and touch; multi-card KMS;
- a log sink and level per host (B8).
