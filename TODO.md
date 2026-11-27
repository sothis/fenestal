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

- **Direct3D 12: a live `fnl_set_vsync()` does not remake its swapchain.**
  The tearing flag is fixed at creation (`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`)
  and `d3d_window_images()` only writes the interval when the size did not
  change, so "off" cannot tear and "on" keeps paying the flag -- where the
  Vulkan renderer's DXGI bridge recomputes it. The titlebar's vsync checkbox
  now makes this reachable.
- **Direct3D 12: the Direct3D 11 bridge's back textures start in COMMON**
  while the frame's first barrier names PRESENT as their before-state (a
  debug-layer error, once per lifetime and per resize): track each one's
  state as the targets' is. **And a window-depth allocation failure is raised
  as device loss** in Direct3D 12 and Metal, which rebuilds into the same
  allocation for ever; the scene target's own path refuses the frame instead.
- **TAAU's history is two presents old**: the copy writes slot S's history and
  the pass in slot S reads it on the next drawn frame, and the motion vector
  is documented as the frame before's. Scale by the presents since the
  history was written (a per-slot present index), or key the history to the
  image. Shared by the three renderers; the bench cannot show it, a panning
  picture can.
- **Win32: the IME is not re-placed when the window moves**, so a candidate
  list in screen coordinates stays where the window was until the next key.
  `WM_MOVE` re-placement while a composition is up wants a composition-active
  flag.
- **The testbed's Vulkan scene keeps its entry points and `g_loaded` at file
  scope**, alone among the scenes, and the VRAM/driver caches beside them: a
  second window's `load_vulkan()` rewrites them under the first's render
  thread (TSAN-visible under `--second-window`). Move them into the
  per-window `Scene`.
- **KMS: a completion given up on at a lend can be taken for the next flip's**,
  reporting that frame with the older stamp (the renew clears the stale count
  on purpose -- keeping it would eat a later completion for the window's
  life). A submit token in the event's `user_data` -- a slot and a sequence
  rather than the window's address -- would tell the two apart.
- **Wayland, minor and latent:** `wl_window_close` leaves `hs->pointer_inside`
  true for a second window closed under the pointer, and `kb_leave` clears
  `kb_focused` for a surface that is not `kb_window`.
- **Wayland: once a discrete notch has been seen, a hi-res wheel's smooth
  parts are dropped for the rest of the process.** `ptr_axis()` returns on a
  smooth WHEEL axis with no discrete once `wheel_discrete_seen` is set, a
  host-wide latch never cleared. It is there on purpose -- a hi-res notch's
  parts arrive as smooth axis events before the discrete count that repeats
  them, and counting both would scroll twice -- but a smooth-only wheel, or a
  compositor that stops sending `axis_discrete`, then scrolls nothing. Counting
  in `ptr_frame()` removes the latch but not the repetition unless the smooth
  notches already delivered are subtracted when their discrete arrives; the
  testbed's `EV scroll` echo counted against the notches, on GNOME and weston,
  before either rule is written down.
- **An event overflow drops one-shot answers with the rest.** `queue_push()`'s
  `ev_lost` gate holds every event until the consumer has drained the ring, so
  a clipboard answer posted while the ring is full is lost, and
  `FNL_EVENT_LOST` cannot say it was that one. Deliberate -- nothing after a
  gap may overtake `FNL_EVENT_LOST` -- but a LOST marker pushed at the overflow
  point with what follows queued after it keeps the order and the answer; that
  is a change to the ring's invariant and wants a design pass.
- **A second window's leftovers:**
  - a second Vulkan window lists no entry of its own renderer until a rebuild
    of its own, so after the window that chose the device closes it cannot yet
    be moved to another GPU of the renderer drawing, nor take the ranking's
    calibration over;
  - a survivor moved to another renderer leaves `Host::renderer` naming the
    first window's choice, so a window created afterwards is built on that one;
  - a window created while the only window is moving to another GPU can be
    built on the renderer being rebuilt, since `opening` is checked before the
    move commits and not again -- a host-level moving flag, stored before
    `opening` is loaded on the move's side, is the shape of the fix;
  - KMS's copy path is one-way, so closing the second window leaves the
    survivor on it though it could take the display again;
  - each Vulkan window keeps its own `VkPipelineCache`, so two windows write
    the one cache file in turn and the last save loses the other's newly
    compiled pipelines -- one cache for the host's device, saved once, is the
    shape of the fix;
  - Win32's size hold is the pump thread's, so a border drag on one window
    holds every window's input for up to `SIZE_AWAIT_MS`.
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

- **TAAU, three pictures:** whether `TAAU_DEPTH_REJECT` at 0.02 earns its keep
  (the moving cube of `demos/motion` read back at two or three values against
  nought); a frame read back to confirm the cubic fetch changes no pixel at
  1:1; and what the separable form of the cubic (four taps for sixteen)
  reclaims of its +0.26 ms. And a **stress section for TAAU**: a pick of the
  titlebar's antialiasing drop-down, the echo's `antialias` line as the proof.
- **Two windows under wine, for the raw mouse registration**, which is the
  host's now (`BackendHostState::raw_owned`): a second window's open no longer
  re-points the registration from the first window's capture, it follows the
  window in front for the wheel, and the program gets its own back when no
  window wants it. The round opens two windows, captures in each in turn,
  wheels over both and closes one while the other holds the pointer.
- **A move to a monitor of another scale**, which this desk has (DP-1 and
  DVI-D-1): the window must rebuild at the scale of the output it lands on, the
  one thing a same-scale pair cannot show.
- **Wine 11.0's rounds today**, against the 10.0 ones that passed: on
  `win64-vulkan` and `win64-d3d12`, a copy that reaches no other program and a
  paste that never arrives; on `win64-d3d12`, the second window's close button
  leaves two windows; under the Wayland driver, a move between monitors that
  stays on `DISPLAY2`, then a press on the titlebar that reaches nothing and a
  lost window. The same failures stand at `5735b47`, before the 1.0 work, so
  they are wine's or the harness's reckoning from it. `wine-staging` has not
  been run, and neither has 11.21 past its first toggle (the key twice).
- **The Vulkan bail path** (`frame_bail()`): a recording forced to fail, the
  swapchain made again and the window drawing on, with no validation message.
- **The swapchain's present wait from the waiter thread:** the specification
  makes `vkWaitForPresentKHR`'s swapchain externally synchronised, and
  `shown_main()` waits beside the render thread's acquires and presents. Asked
  of the validation layer's thread checks, whether that holds or the wait
  wants the lock in slices.
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
- **Direct3D 12 through a drag under wine** draws fewer frames than Vulkan
  (35 to 37 on X11 and 18 to 23 on Wayland against 53 to 58, intervals up to
  55 ms): the `--frame-times` lines' `other` under wine would name the gap.

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
