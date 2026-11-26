# TODO

What is owed, as opposed to what is known. `DESIGN.md` is the record of
what was measured; an item answered there, with its measurement, comes off
this list.

## FreeBSD, MSVC and the UCRT

Written on 2026-11-25, the day the targets were added. The FreeBSD and
MSVC trees are compiled in CI, and since 2026-11-26 both are compiled on
this desktop too -- cross, as the Mac's are (DESIGN *Cross-compiling for
FreeBSD*, *Cross-compiling the MSVC tree*); neither is run here, and the
UCRT one is driven on this desktop under wine.

- **The bake's filter.** Ubuntu's FreeType is built with
  `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` and filters LCD coverage with the
  ClearType taps; FreeBSD's package and vcpkg's build are not, and render
  harmony instead, which needs no filter. `mkfont` accepts both now, but
  text baked the harmony way has not been looked at on a screen.

### FreeBSD

- **The console.** KMS takes DRM master and saves and restores the CRTCs;
  on FreeBSD `vt(4)` owns the device (`vt_drmfb`), and whether master, a
  mode set and the restore behave as they do on Linux is a hardware
  question. `EVIOCGRAB` on evdev is not the vt keyboard path's grab: a
  key may still reach the console shell under the window.
- **The picture.** Whether Mesa on FreeBSD offers
  `VK_EXT_acquire_drm_display` (else the copy path), and whether
  drm-kmod's atomic client capability is there (else the legacy ioctls).
- **Hotplug.** There is no kobject uevent group and no inotify; outputs
  and `/dev/input` are rescanned every `KMS_RESCAN_S` seconds. A monitor
  or keyboard that comes or goes is seen that late, and the GPU hotplug
  that netlink carries on Linux is not heard at all.
- **Device access.** `/dev/dri/*` and `/dev/input/event*` want a devfs
  rule or the `video`/operator groups; `README.md` *On a bare display*
  says so. Whether `EVIOCSCLOCKID` takes there is unmeasured (the call is
  unchecked and a refusal leaves the default clock).
- **Figures.** `parts.c`'s thread list and `metrics.c`'s RSS read
  `/proc`; on FreeBSD the first is empty and the second is `-1` (the
  panel shows unknown). `rank.c` has the `sysctl` path for its name.
- **Keymap names.** There is no `/etc/default/keyboard` or
  `/etc/vconsole.conf`, so the console layout defaults to US unless
  `FNL_KMS_LAYOUT` names one; `kbdmap`/`rc.conf` names are not read.
- **The harness** has no FreeBSD environment: `bare_driver.TREES`,
  `DISTROS` and the `uname` gate know Linux and Windows only. A round
  there is by hand.

### win64-msvc

- **The archive.** `fenestal::static` keeps its internals global: MSVC has
  no relocatable link and no objcopy. The DLL's exports are still the API
  list alone. A consumer defining a library-internal name could meet a
  duplicate at the link, and `fenestal-dev`'s five-name check is guarded
  out there (`tools/drive/lifecycle.c`).
- **Warnings.** The tree is built at `/W3` and not held to `/WX`; its
  warnings have not been gone over, unlike gcc's and clang's.
- **The win64 analyser tree** needs a gcc-based UCRT compiler; CI's
  llvm-mingw is clang and has no `-fanalyzer`, so the checkers job skips
  that tree. MSYS2's UCRT64 gcc on Windows can run it.

## The MCP port

Run, with `tools/drive/mcp_check.py` and by hand, on schenker's KDE Wayland,
KMS (release, asan, tsan), Xorg and weston, on Windows 11 with Vulkan and
Direct3D 12 and on macOS with Metal and Vulkan (2026-11-26); DESIGN *Driven
from inside*. Owed:

- **FreeBSD, `linux-arm64` and the MSVC tree** compile and have never run
  it: the Pi 4's KMS (`bare_run.py` there), FreeBSD's `vt(4)` console and a
  native cl.exe build are the first places the socket, `fenestal-mcp` and
  `pointer_warp` meet a system the check has not.
- **Under a window server the pointer cannot be moved**
  (`backend_noop_pointer_warp`): the program is told of a position the cursor
  does not show. X11's `xcb_warp_pointer`, Win32's `SetCursorPos` and Cocoa's
  `CGWarpMouseCursorPosition` could, at the cost of moving a hand's cursor
  on a desktop somebody is using; left out until an agent on a desktop
  asks for it.
- **A display that is off or locked** sends a window no frame callbacks
  (KDE's idle blank, found 2026-11-26): `capture` and `events` then time out,
  and `state.shown` is still true. The library has no way to ask a
  compositor whether it is drawing.
- **The Windows agent wedges after a `native_run.py` that was killed**: the
  next run says "the program's output stopped without its exit code" until
  the `fenestal-agent` task is ended and run again (`schtasks /end`, `/run`,
  then ~30 s), and the first run after a boot does too. The relay's port
  and the agent's one-run-at-a-time are the places to look.

## HDR

Written on 2026-11-25; every tree of the four targets was built the same
day -- linux, win64 (release, analyzer and gc), macos (release and gc) and
macos-arm64 (release and gc) -- warning-clean, their checks passing.
Direct3D 12 presents scRGB where the display is in an advanced colour
mode, Metal the screen's extended range, Vulkan 10-bit PQ where the surface
offers it, and KMS 10-bit PQ through its copy path where the connector's
EDID names it. DESIGN *HDR: three spaces, one linear target* has the model;
the testbed's `--colour=` sets it. Nothing has been seen on an HDR panel.

Owed, in this order:

- **A host with an HDR display**: schenker's Mac panel is the one screen
  here known to have extended range, and no desktop monitor in the lab is.
- **What the float target costs**, on this desk and on the Pi: under HDR the
  scene, the resolve and the fetch all move from 8-bit to `RGBA16F`, so
  `bench.py --sizes` and `pace.py` against an SDR row of the same scene.
- **The harness**: a colour check (the program's `colour:` line against
  `FNL_COLOR`), a capture on an HDR output to read the ramp's last quarter
  against white, and an environment per backend that offers it. A pixel
  comparison has to know the space it is comparing in.
- **PQ on Linux**: whether Mesa's Wayland WSI lists `HDR10_ST2084_EXT` where
  the compositor's colour management is up, and that the SDR fallback holds
  where it is not.
- **KMS on HDR hardware**: the EDID probe, the connector's
  `HDR_OUTPUT_METADATA` and `Colorspace`, and the `XRGB2101010` scanout are
  written, and no panel in the lab carries the HDR static metadata block, so
  they have compiled and never been taken. What wants seeing: a connector
  that refuses `Colorspace` or the metadata, a driver whose dumb buffers will
  not take 10 bits, and the infoframe as a TV reads it.
- **Windows mid-run**: the user's HDR switch in Settings is not followed until
  the device is rebuilt. Whether a flip-model swapchain may change its colour
  space under a live window, and what `WM_DISPLAYCHANGE` should do, is
  untested.
- **MLAA in linear light**: its edge test was tuned on encoded values under
  SDR and now sees linear ones under HDR; whether its threshold wants the
  encode before it is unmeasured.
- **The veil**: declined under HDR, the snapshot being 8-bit, so a move fades
  through black; an encoded readback or an `RGBA16F` snapshot restores it.
- **A demo in HDR**: none asks for it -- `want_color` is 0 in every demo, so
  they present SDR and are unchanged -- and one that did would want its
  `CubePush` to carry `FnlFrame::color`'s two figures and its shader to
  multiply by `sdr_scale`. A demo is the program people copy, so this is the
  one that should show it.

## The 175% rung

Added on 2026-11-25: 210 in the baked ladder, the standard 175% that was
missing between 200 and the two scales above it. The bake has not been run
since, so DESIGN *The blob*'s figures are the six-rung bake's.

Owed: one release build, which re-bakes every atlas, to re-measure the blob's
coded size and the decode time (DESIGN *Three tables decide the cost*), and to
see `verifyfont` pass with the seventh -- the lender's test every cross tree
borrows.

## Temporal upscaling, measured

> **Built on 2026-11-22** (`a42c7d8`), the first compile of any of it: every
> tree is clean under gcc and under clang, the analyzer and the three
> sanitizers among them. The compile found what reading had not -- a C-style
> cast the shader compiler rejects in `blit.frag`, a Vulkan enum name that
> does not exist on the depth barrier, and a status line grown past its
> buffer -- all fixed in that commit, which also gave the vector its first
> callers.

Built on 2026-11-21 in all three trees -- `build/linux` (Vulkan),
`build/win64` (Direct3D 12 and Vulkan) and `build/macos` (Metal) -- warning
clean, the one shader through SPIR-V, DXIL and MSL.

The same day, eight environments drew resolved frames in
`tools/drive/bench.py` with `--bench-antialias=taau --bench-scene-motion=1`,
every record `complete`: this desk's `x11-vulkan` (59.5 fps, p99 25.4 ms),
`win64-vulkan` under wine (59.4), schenker's Windows
`win64-vulkan-native@nvidia` (60.0), its CachyOS `x11-vulkan-bare` (60.0),
`wayland-vulkan-bare` (59.8), `kms-vulkan-bare:nvidia` (58.9) and
`:nouveau` (29.9), and `macos-metal-native` (60.0). What the resolve costs,
motion on and on this desk: a direct frame 0.190 ms of GPU, TAAU 0.583, MLAA
0.691 -- +0.393 on a direct frame and 0.108 cheaper than the filter; the CPU
side +0.049 ms, the re-record the jitter forces. A captured frame
(`FNL_BACKEND=x11`, 2560x1508) is a drawn picture rather than a blank one.

Owed, in this order:

- ~~**Does Direct3D 12 draw it**~~ -- **answered for the drawing (2026-11-22)**.
  The environment the entry called shut is not: `win64-d3d12` under wine runs
  at `df55495` as it ran at `6b1788c` -- 98 checks, none failed, the overlay's
  picture among them -- and `tools/drive/bench.py win64-d3d12 --
  --bench-antialias=taau --bench-scene-motion=1` writes a `complete` record
  (59.997 fps, p99 18.45 ms, 1200 frames), with a frame captured through
  `import -window` showing the star drawn and the titlebar's TAAU box ticked.
  The silence that entry recorded was not reproduced, and the whole-tree
  review's wine picture failure is gone with it.

  **Still owed: the debug layer's opinion** of the resolve's
  `RENDER_TARGET -> COPY_SOURCE -> PIXEL_SHADER_RESOURCE` round trip, and of a
  one-texture pipeline binding a two-descriptor table. Neither machine that can
  be reached here has the layer: vkd3d-proton has none (`D3D12_NO_LAYER`,
  `environment.py`), and `win10`'s own Windows says `the Direct3D 12 debug
  layer is not installed` -- its Graphics Tools are not a feature of the
  guests' image (2026-11-22, `win64-d3d12-vm@win10`, `gc`, which drew 307 TAAU
  frames on WARP with a `complete` record, so Windows itself draws the mode
  too). The layer is on aspire's Windows at least: it raised its
  own exception in DESIGN *A loss waits for the queue under Direct3D 12*, so
  `win64-d3d12-native@aspire` on a `gc` tree is the way in, and both laptops
  were unreachable on 2026-11-22.
- ~~**The Vulkan barriers, under the validation layer**~~ -- **measured
  clean (2026-11-22)**. `asan` with
  `FNL_VK_VALIDATION=1`, `VK_LAYER_VALIDATE_SYNC=1` and
  `VK_LAYER_VALIDATE_BEST_PRACTICES=1` -- the layer probe proves mapped in the
  program's own `/proc/<pid>/maps` -- and `--bench-antialias=taau
  --bench-scene-motion=1`: `x11-vulkan` drew 2395 frames in 40 s, the
  record `complete`, and the log holds no layer line. The barriers around
  `vkCmdCopyImage`, the copy's write-after-write on the history image among
  them, are therefore silent under the checks that would name them. Probed
  by hand and not by a round: the round has no TAAU section, so what a
  checker round repeats is still owed --
  a press on the titlebar's TAAU box (that box's x, and the echo's
  `antialias` line as the proof the press landed) is what it wants.
  **The depth barrier is now covered too (2026-11-22, `a42c7d8`).**
  `record_taau_pass()`'s third barrier, which returns the scene depth from
  `DEPTH_STENCIL_READ_ONLY_OPTIMAL` to the attachment layout for the next
  frame, was added in `5e000d7` and compiled for the first time only at
  `a42c7d8` -- where it named a Vulkan enum that does not exist
  (`VK_ACCESS_DEPTH_ATTACHMENT_WRITE_BIT`; the barrier wants
  `_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT`). With the tree compiling, the same
  method cleared it: the `asan` and `gc` trees on `x11-vulkan` under
  `FNL_VK_VALIDATION=1` and `VK_LAYER_VALIDATE_SYNC=1`, drawing
  `--bench-antialias=taau` -- the only mode that makes the depth resident, so
  the barrier and the `TRANSFER` transitions around
  `scene_depth_make_resident()` both run every frame -- the records `complete`,
  the layer proven mapped in the program's `/proc/<pid>/maps` and not merely
  asked for, and no layer line in either log. Probed by hand, as the item
  above is: what a checker *round* still owes is a TAAU section, a press on
  the titlebar's TAAU box.
- ~~**What it costs**~~ -- **measured (2026-11-22)**. By the table's own
  method (DESIGN *Antialiasing*), a pinned scene against a direct frame of the
  same pin, uncapped on `MAILBOX`, motion off, `frame.gpu_ms`'s p50 over eight
  seconds. The pinned case wanted a render scale the testbed could not be put
  in from the command line, so `--bench-scene-scale` was added to the
  benchmark (`fnl_set_scene_size_px()` at area/scale): the resolve is +0.656 ms
  of GPU at native 2560x1440 on the RX 590, and **+0.095 ms pinned at
  1280x720, +0.030 at 640x360** -- cheapest where the mode is worth having, as
  the design expects. On lavapipe a hundredfold larger, a software cost. The
  figures are in DESIGN *Antialiasing*. **The fill on its own is measured too**
  (2026-11-22): a second identical `vkCmdCopyImage` added to the pass (both
  images already in TRANSFER layouts, so it changes nothing) and the delta
  against the one-copy build taken -- **0.097 ms of GPU at 2560x1440, 0.033 at
  1280x720**, two runs each agreeing. So of the resolve's +0.656 ms at native,
  ~0.10 is the history fill and ~0.55 the full-screen pass; at half, ~0.03 of
  the +0.095 is the fill and ~0.065 the pass. The fill is a real slice but the
  resolve dominates.
- ~~**What the scene's fetch costs now**~~ -- **measured (2026-11-22)**. The
  same method as the resolve's table: the cubic fetch against the bilinear one
  it replaced, the shader the only difference, a 2560x1440 area on the RX 590.
  The fetch runs per output pixel, so its cost is flat: **+0.26 ms of GPU**
  whether the scene is native, pinned at half or a quarter. And the design's
  claim that it "pays what it paid before" at 1:1 is right about the result
  and wrong about the cost -- at native size a direct frame never fetches
  (0.165 ms either way), but a TAAU frame goes 0.576 to 0.829. So the sharper
  stretch is +0.26 ms even where it changes no pixel. That +0.26 is the
  cubic's own increment -- sixteen taps against the bilinear fetch's one, both
  at the same 2560 output, so the resolution is common to both and cancels.
  What is still owed is how much a *cheaper* cubic reclaims: the same sixteen
  taps collapse to four with the separability the loop does not use (a bilinear
  row pass then a bilinear column pass), and that against the sixteen is the
  figure a reader weighing the cubical against a two-pass separable one wants.
  A frame read back to confirm the result is identical at 1:1 is still owed
  too.
- **What the depth comparison costs** -- **measured, and it is nothing
  (2026-11-22)**. `TAAU_DEPTH_REJECT` at nought leaves the fetch and the alpha
  write in place and turns off only the compare: three interleaved runs each at
  1280x720 read 0.512 ms at 0.02 and 0.514 at nought, a difference inside the
  spread and in the wrong direction. Lavapipe agrees under a spread wider than
  any signal. So the compare is free; the depth fetch and write are not, and
  live inside the resolve's +0.095 ms. **Whether it earns its keep is still
  owed**, and is a picture question, not a clock one: `TAAU_DEPTH_REJECT` at
  0.02 is a guess, and what it should be wants the moving cube of
  `demos/motion` -- where the clamp, not the accumulation, decides -- read back
  off the window at two or three values against the threshold at nought.
- **What a motion vector looks like** -- **judged on the demo (2026-11-22),
  the testbed still owed**. `demos/motion` under `F2` (drift) + `F3` (TAAU) +
  `F4` (scene pinned to half the window), toggling `F3` while the cube panned:
  **crisper with TAAU on**, the cube's silhouette and face edges holding
  through the pan rather than smearing -- the reprojection gathers the moving
  content and the clamp lets it through. The echo proves the conditions (the
  pin held at 960x600 across each `antialias` toggle, so the TAAU change was
  not a re-layout). This is the case the design is built for.
  **The testbed's star now applies the jitter too (2026-11-22).** No program
  in the tree was applying `FnlFrame::jitter_x`, so TAAU on the star gathered
  identical samples -- a false test. `demos/motion` was taught it first; the
  testbed's four scene files (`scene_vulkan.c`,
  `scene_d3d12.c`, `scene_metal.m`) now shift their projection by it the same
  way, in the two vertex shaders' shared `ScenePush` (grown 16 to 32 bytes,
  `vec2 jitter` after `size`), verified through the HLSL and MSL
  translations and clean in every tree. So the star testbed now draws a real
  TAAU picture. What is still owed is the *comparison* the item names: the
  star at 60 and at 30 sizes a second, against MSAA and MLAA, a frame read
  back off the window -- the demo's half-scale comparison answered the mode's
  worth, not the star's against the other two.
- ~~**What it looks like**~~ -- **measured on the star, against MSAA and MLAA
  (2026-11-22)**. The testbed's star, `--freeze` (the scene at a held pose while
  the library clock runs, so TAAU still gathers its jitter), motion on, each
  mode set and its picture read back through the portal -- so all three frame
  the identical scene: FSAA and TAAU differ by a mean 1.9/255, FSAA and MLAA
  1.2/255, the difference at the edges alone. Over the star disc, **TAAU has
  the fewest hard edge steps (3699 against FSAA's 5972 and MLAA's 6266) and the
  most blended ones (6.0% against 4.5 and 4.6)** -- the smoothest edges, its
  thin rotating wedges cleaner than MLAA's, which show stair-stepping at the
  tips. FSAA and MLAA sit close, MLAA the weaker of the two on the fine
  features, as the filter's four-pixel minimum span predicts. One pose at
  native size on the RX 590. **The 60-against-30 question is answered too**
  (2026-11-22): the testbed's new `--fps=N` holds the present rate (verified,
  30 reads 30 against 60's 60), so at 30 the star advances twice as far per
  frame -- twice the displacement the clamp sees. TAAU stays clean at 30 as at
  60, no smearing or ghosting from the larger inter-frame jump (hard edge steps
  2769 against 2411, blended 6.3% to 5.9%, the small rise the bigger step
  predicts), so the accumulation handles a halved rate. The two frames are at
  different rotations -- real motion, not frozen -- so it is a
  quality-holds-up judgment and not a pixel diff. The moving cube of
  `demos/motion`
  was judged too -- crisper with TAAU at half scale, edges holding through the
  pan (its own entry above).
- ~~**Whether a resting window keeps it**~~ -- **yes (2026-11-22)**. The
  testbed, TAAU on, `--freeze` (one pose) and `--frame-stats-log`: motion on to
  converge, then motion off; the stats prove the rest (frames climbing 408-438-
  469 while moving, then held at 500-501-502 -- the ~1-2 a half second are
  `rest_owed()`'s figure refreshes, not the scene). The rested image is the
  converged one, sharp and intact -- star, checkerboard, bars all present, not
  blanked, not degraded. Two rested frames 1.5 s apart differ by a mean
  0.69/255 (nonzero only at edge pixels), so it holds still rather than
  gathering on; the residual is the figures-refresh redraw running the scene
  through the accumulation again, which is `rest_owed()`'s doing and not the
  scene moving. Nothing in the result asks for a change: the intent -- a still
  scene keeps what it converged on -- holds on a screen.
- **The titlebar control**: the fourth checkbox (TAAU) is 58 points of run,
  measured in `deco_min_window()` on 2026-11-21 -- the floor 556 to 614, and
  683 with the GPU box in it; that 683 is `environment.py`'s `FLOOR_W`, and
  `stress.py`'s floor check agrees with it. The three widths the output box
  gives way at were derived from the pre-TAAU measurement (798, 792, 790).
  **Captured (2026-11-22)** with the testbed's new `--width=PT` (the presentable
  width in points, verified: 800 reads 1600 px at scale 2, 792 1584, 790 1580):
  the DP-1 output box is **present at 800 and at 792, gone at 790**, the GPU box
  taking its place -- the derived boundary holds. At 700 the bar is intact and
  readable, every control fitting with no overlap or truncation. So the
  give-way widths are measured rather than derived now. **The narrowest bar is
  checked too (2026-11-22)**: with the GPU box in the run the enforced floor is
  683 points, not the 614 without it (`deco_min_window()`), and it holds on
  screen -- `--width=670` and `650` both clamp to 683, and at 683 the bar shows
  FSAA, MLAA, TAAU, Fs Upsample, GPU 1, the size box and all three window
  buttons, no overlap or truncation and no control lost. The 614 figure is the
  window's floor with no GPU box, which a renderer that has listed its GPUs
  never has.

## Vendor temporal upscaling (MetalFX, FSR 2)

Two vendor upscalers are open enough to build on -- MetalFX a system framework
and FSR 2 under MIT -- with very different costs, and neither is written. The
caller-side seam already lets a program bring its own: every
renderer hands out the scene's command buffer and layout
(`fnl_vulkan_scene_cmd()` and its D3D12 and Metal twins), and
`fnl_set_scene_motion_px()` plus the depth are the inputs. So what follows is
about the library drawing one itself, not about whether a caller could.

- **MetalFX -- a scoped implementation, not yet written.** It fits the Metal
  renderer: `MTLFXTemporalScaler` is a system framework (no vendored binary,
  no new link line) and takes colour, depth, a motion texture, an output and a
  jitter offset -- the last two and the depth the library already has
  (`ds->jitter_x/y`, `ds->motion_x/y`, the scene depth). Two things block a
  claim that it works, and both are the reason it is a TODO and not code:
  1. **A motion texture, not a vector.** The scaler wants a per-pixel
     two-component motion image; the library produces one uniform vector. A
     MetalFX path can only synthesise a constant-motion texture from
     `ds->motion_x/y`, which is faithful for a pure camera pan -- the case a
     single vector describes, and what the API is documented for -- and wrong
     for content that moves independently. Anything better wants a per-pixel
     motion seam the library has not got, which is the same gap FSR 2 has.
  2. **It cannot be verified here.** MetalFX's temporal scaler needs a
     supported device and macOS 14 or later (it gates on
     `supportedInputContentMinScaleForDevice:`). The one Mac in this lab is an
     Intel one reached over ssh, and no round runs MetalFX. So the work must be
     **built for real and then seen drawing on a Mac before it is called
     done** -- writing it cross-compiled and unrun would be a claim with no
     evidence. The shape: gate on `MTLFXSpatialScalerDescriptor` or
     `MTLFXTemporalScalerDescriptor` at the device, fall back to `taau.frag`
     where unavailable or refused, and add the constant-motion texture the
     scaler reads.
- **FSR 2 -- a planned subsystem, not an integration.** FSR 2 is MIT and so
  may be reimplemented, but the tree has **no compute shaders at all**
  (`shaders/` is `.vert`/`.frag`; no `vkCmdDispatch` in
  any renderer) and its reference is C++, against this tree's C11. A real FSR 2
  means, in order: (a) a compute stage in the three renderers and the build's
  shader translation for SPIR-V/DXIL/MSL; (b) the passes ported to
  C11 and compute shaders -- reactive mask, locks, history, auto-exposure,
  Lanczos; (c) the same per-pixel motion texture MetalFX wants, since FSR 2
  reprojects by a vector field too. It is multi-session, and starts with the
  compute foundation, Vulkan first.
- **DLSS and XeSS are not implementable here** and are not on this list:
  their upscalers are trained neural models shipped as opaque vendor binaries,
  so "supporting" them is loading a vendor DLL, which the link invariant ("a
  consumer links `fenestal` plus the system's own libraries, and nothing
  else") and the no-vendored-binaries rule both refuse. A caller who has the
  SDK loads it through its own code, on the seam above.
- **The Metal depth the resolve samples is now stored (2026-11-22).** Found
  while scoping MetalFX: the Metal scene pass opened with a `DontCare` depth
  store, so the TAAU resolve read undefined depth for its rejection. Fixed
  (`MTLStoreActionStore` when FNL_AA_TAAU is in force), built for the macOS
  tree; not yet run, since no round drives TAAU on the Mac -- it wants a frame
  on the Mac with `FNL_AA_TAAU` and the depth rejection on, read back.

## Windows laptops

Each needs a hand on the machine or hardware the two laptops do not have.
`tools/drive/native-windows.md` is the strategy.

- **A slower second monitor on Intel**: a window there is paced by the
  panel's 60 Hz (DESIGN *Measured on Windows itself*). Are the extra frames
  dropped or shown late?
- **The hybrid copy**: an external output on the NVIDIA device, which
  neither laptop has had.
- **Sleep with the lid, a second monitor, or longer.** A minute's suspend is
  done on all four devices. Open too: why only the second laptop's NVIDIA
  run shows the sleep as a gap in the frame clock.
- **Battery and battery saver**, where Windows may coarsen a throttled
  process's timers.
- **A panel above 60 Hz, and variable refresh.**
- **Intel's swapchain stall** is prevented, not explained; the report is
  drafted in `docs/intel-swapchain-stall.md`, its last section what it owes.

## Win32 and macOS, after the 2026-11-25 review

Written by reading; both changes are in, and each wants the round that
proves it.

- **A two-window wine round** for the raw mouse registration, which is the
  host's now (`BackendHostState::raw_owned`, `raw_point()`/`raw_give()`,
  `raw_wanted()`): a second window's open no longer re-points the process's
  one registration away from the first window's capture, the registration
  follows the window in front for the wheel, and the program gets its own
  back when no window wants it (DESIGN *A second window*, *Wine on the bare
  machine*). The round opens two windows under wine, captures in each in
  turn, wheels over both, and closes one while the other holds the pointer.
- **A Mac round** for Cocoa's per-entry autorelease pools: every
  request-drain entry holds one now, and `wake_cb()`'s drain loop a pool a
  window, so a round that reads the program's stderr should show no
  `_NSAutoreleaseNoPool` from a title set every frame or an input-latency
  run, and the resident set should stay flat across them.

## After the second 2026-11-25 review

Found by reading, each proved at its site; these are the ones left, and what
each wants.

- **D3D12: a live `fnl_set_vsync()` does not remake its own swapchain.** The
  tearing flag is fixed at creation (`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`),
  and `d3d_window_images()` only writes the interval when the size did not
  change, so "off" cannot tear and "on" keeps paying the flag -- where the
  Vulkan renderer's DXGI bridge recomputes it. Wants a Windows round, as the
  flag costs 53 ms against 36 on the GTX 960M.
- **D3D12: the Direct3D 11 bridge's back textures start in COMMON** while the
  frame's first barrier names PRESENT as their before-state (a debug-layer
  error, once per lifetime and per resize): track each one's state as the
  targets' is. **And a window-depth allocation failure is raised as device
  loss** in both D3D12 and Metal, which rebuilds into the same allocation
  for ever; the scene target's own path refuses the frame instead.
- **TAAU's history is two presents old**: the copy writes slot S's history
  and the pass in slot S reads it on the next drawn frame, and the motion
  vector is documented as the frame before's. Scale by the presents since
  the history was written (a per-slot present index), or key the history to
  the image. Shared by the three renderers; wants a panning measurement.
- **Win32: the IME is not re-placed when the window moves**, so a candidate
  list in screen coordinates stays where the window was until the next key
  (Cocoa invalidates on move and resize now). `WM_MOVE` re-placement while
  a composition is up wants a composition-active flag.
- **The testbed's Vulkan scene keeps its entry points and `g_loaded` at file
  scope**, alone among the scenes, and
  the VRAM/driver caches beside them: a second window's `load_vulkan()`
  rewrites them under the first's render thread (TSAN-visible under
  `--second-window`). Move them into the per-window `Scene`.
- **KMS: a completion given up on at a lend can be taken for the next
  flip's**, reporting that frame with the older stamp (the renew clears the
  stale count on purpose -- keeping it would eat a later completion for the
  window's life). A submit token in the event's `user_data` -- a slot and a
  sequence rather than the window's address -- would tell the two apart; it
  wants the bare machines.
- **Wayland, minor and latent:** `wl_window_close` leaves
  `hs->pointer_inside` true for a second window closed under the pointer,
  and `kb_leave` clears `kb_focused` for a surface that is not
  `kb_window`; no present reader misbehaves today.
- **Optimizations measured but not taken:** the window image's
  depth is drawn and cleared for frames that never test it; Win32's
  `seed_held_keys()` asks ~230 scancodes per focus gain; Cocoa runs
  `update_scale()` twice for one backing change and `screens_named()` is
  O(n²) per forced publish; KMS re-enumerates the CRTCs five times per mode
  set; CI's macOS job builds each slice and then the universal tree builds
  them again; X11 asks a whole property before checking its limit, re-reads
  `_NET_WM_STATE` inside dispatch, retries a refused pointer grab on every
  motion while a capture is suspended, and pays three round trips for the
  default cursor before the opening waits on a scaled desktop.

## After the third 2026-11-25 review

Found by reading, each verified at its site; the fixes are in, and these are
the two left, with what each wants.

- **Wayland: once a discrete notch has been seen, a hi-res wheel's smooth
  parts are dropped for the rest of the process.** `ptr_axis()` returns on a
  smooth WHEEL axis with no discrete once `wheel_discrete_seen` is set -- a
  host-wide latch never cleared. The guard is there on purpose: a hi-res
  notch's parts arrive as smooth axis events before the discrete count that
  repeats them (the state header says so), and counting both would scroll
  the notch twice. But a smooth-only wheel, or a compositor that stops
  sending axis_discrete, then scrolls nothing after the first discrete, and
  a compositor that sends an axis before its coupled discrete would count
  the first notch twice. Counting in `ptr_frame()` -- the discrete where the
  frame has one, the smooth travel otherwise -- removes the latch but not
  the repetition, unless the smooth notches already delivered are subtracted
  when their discrete arrives. Wants a hi-res wheel on GNOME and on weston,
  the testbed's `EV scroll` echo counted against the physical notches,
  before either rule is written down.
- **An event overflow drops one-shot answers with the rest.**
  `queue_push()`'s `ev_lost` gate holds every event until the consumer has
  drained the ring, so a clipboard answer posted while the ring is full is
  lost, and `FNL_EVENT_LOST` cannot say it was that one. Deliberate --
  nothing after a gap may overtake `FNL_EVENT_LOST`, or a release would
  land before the loss that explains it. A LOST marker pushed at the
  overflow point, with what follows queued after it, keeps the order and
  the answer; that is a change to the ring's invariant, so it wants a
  design pass.

## The output box under a window server

Written by reading (DESIGN *The titlebar controls*); its stress check was
added on 2026-11-05 and passes on X11 and wine (the `output-pick`
capability, two checks each), and the give-way was measured the same day:
the box is drawn at 734 points and gone at 732, and a caller's run yields
to the title long before it. Owed:

- **Does each move land?** Measured on 2026-11-05, every backend that
  places a window: X11 under mutter and with no manager (schenker's stick,
  NVIDIA), maximized by the box and fullscreen by the testbed's F7, each
  to the other monitor and back with the state kept; Win32 on Windows,
  maximized and fullscreen -- the maximized one by its placement and then
  a restore (`win32_set_output()`), which it did not before: the placement
  moved only the restored rectangle, and the window stayed maximized where
  it was until a restore; Win32 under wine, both renderers, fullscreen and
  a maximized move that landed; Wayland, fullscreen; the Mac, a zoomed
  window (Built-in Display to PL4071UH and back). The fullscreen moves
  want a way in with no bar to click, which the testbed's F7 -- the next
  output -- is. Still owed: a monitor of another scale, where both of
  schenker's are 125%; the move must be checked to rebuild at the scale of
  the output it lands on, which is the one thing a same-scale pair cannot
  show. The cursor (now the surface scale's size) and the 175% text rung are
  in. Under wine a maximized window on a monitor with no
  taskbar (DISPLAY2) is drawn without its titlebar, with or without a
  move: wine's maximize and not the library's, so the box could not be
  clicked there and the back was not driven.

## The metrics panel at rest, after its change

Written by reading (DESIGN *The metrics panel*); the at-rest cost, the
minute at rest, the sample after the present on amdgpu and on NVIDIA, the
per-frame figure under wine, the figures' frames left out of the means
and the panel's GPU row as a sampled share were measured on 2026-11-05 and
recorded there. The figures' frames: with `--frame-times` and the
testbed's `--frame-stats-log` on X11, the panel's rest frames drew and were
counted -- frames and rows both 124 to 127, GPU 35621 to 36830 us, 403 us
a frame -- while the figures held at the last drawn frame's (CPU 0.24,
GPU 0.39 ms); the drained rows lag the totals by the newest frame or two,
and a rest frame's GPU time reaches the totals after the sample's
quarter-second hold, so its drained row says -1. The GPU row: on
schenker's Intel Vulkan at 3840x2117 and 30 fps, 18.9 % against the
product's 18.9 %; at rest with the panel shown, 0.0 % where the product
stands at the last moving frames' 18.9 %.

## The whole-tree review of 2026-11-02

Each change was written by reading. The release trees were built and
round on 2026-11-04 and 05 -- the bare machines, both laptops' Windows
and the Mac -- the macOS cross tree covers the `.m` files'
`@autoreleasepool` returns, and the clang tree (`-Wthread-safety`) built
clean and passed a round on the thinkpad on 2026-11-05. The checker
trees were built and round on the same day, on the devices and not this
desk: asan and gc passed on the thinkpad and the Pi, analyzer on the
thinkpad and a laptop, the Windows analyzer and gc on the laptops and
the macOS gc on the Mac; the Pi's tsan is the one failure, on the
driver's own race (TODO *The Raspberry Pi 4*, whose debug info Arch
Linux ARM does not publish). The desk and the guests were round on
2026-11-05: x11-vulkan 106 checks with only the known sync-value line
below, win64-vulkan 89, win64-d3d12 93 and
wayland-vulkan 74, all clean, and the guests' win64-vulkan-vm and
win64-d3d12-vm 91 each on win10 and win11, all clean.

- **Win32's mouse capture** (`on_button()`, `WM_CAPTURECHANGED`): a press
  in the content dragged off the window and released there, in a guest
  and on a laptop; a drag of the titlebar and of a border still moving
  and sizing; Alt+F4 closing and Alt+letter silent.
- **X11's wake after an off-pump flush** (`x11_flush()`,
  `x11_ack_release()`): a border drag under a manager with
  `_NET_WM_SYNC_REQUEST`, its last release answered at once.
- **X11's paste stamped with the server's time** (`FNL_STAMP`,
  `x11_clip_property()`): a paste from Firefox, Qt and xclip owners; a
  large one by INCR; one given up at the deadline, then another; and a
  copy made while a paste is on its way answering it with the copy.
- **KMS's per-group key types** (`key_cut()`): the option that redefines a key
  (`compose:ralt`, which is `compose(ralt)` -- the right Alt made a
  two-level Multi_key with a type of its own) leaving the other keys' group
  levels is now checked by `verifykeymap` (`files: compose:ralt`): the right
  Alt types nothing and is no group key, and every other key keeps the third
  level its layout gave it, in both groups and in the table, with its
  selector. What is still owed is the **machine** half -- a two-layout
  console (`FNL_KMS_LAYOUT=us,de`, or `--layout='de,us compose:ralt'`) typed
  on the thinkpad, which is a round and not a check. The plain
  `grp:alt_shift_toggle` console was seen there (2026-11-05), 17 sequences
  with the third levels among them and the group key switching Y both ways;
  the `compose:ralt` spelling there is owed, blocked on a hand at that
  console.
- **X11's output move from fullscreen or maximized** (`x11_set_output()`,
  `BackendState::norm_x`): an X11 window on this desk, where GNOME
  manages it, restored where it was, placed on the other monitor.
- **Wayland**: copy then paste in one turn (`clip_sync`), on GNOME and
  weston; the frame callbacks a renew displaces destroyed once and only
  once (`present_drop()`, `frame_done()`), under the checkers.
- **The titlebar's open list under a custom bar or in fullscreen**
  (`ctl_modal()`, `ctl_draw_popup()`): a press where its hidden rows
  would be lands on the window, the list is shut when the bar comes back,
  and a checkbox pressed with a list open only puts it away.
- **KMS's captured pointer from a tablet or touchscreen**
  (`pointer_to()`): no creep across a stroke, and no jump at a new
  touch. Owed: blocked on hardware -- no machine in the lab has a tablet
  or a touchscreen on a KMS console; the arithmetic (8 px/mm) is checked
  with a uinput pad on the three laptops, but a uinput pad is one pointer
  and cannot show a stroke or a new touch.
- **macOS's output name with two monitors of one model**
  (`publish_output()`): `fnl_output_name()` handed to
  `fnl_request_output()` keeps the window where it is. The Mac's two are
  the built-in panel and the PL4071UH, so a pair of one model is still
  owed.
- **Direct3D 12**: an adapter named by `FNL_GPU` let go at the device's
  end (`choose_adapter()`). The Direct3D 11 present refused part-way
  (`d3d11-present-refused`) was run on both laptops' GPUs on 2026-11-03
  (DESIGN's table). The guests draw on WARP, which the composition
  measure skips, so the fault is armed there and never reached.
- **The Vulkan bail path** (`frame_bail()`): a recording forced to fail,
  the swapchain made again and the window drawing on, with no
  validation message.
- **The swapchain's present wait from the waiter thread**: the spec
  makes `vkWaitForPresentKHR`'s swapchain externally synchronised, and
  `shown_main()` waits beside the render thread's acquires and presents.
  Asked of the validation layer's thread checks, and of drivers beyond
  Mesa's, whether that holds or the wait wants the lock in slices.
- **`fnl_d3d12.h` compiled alone** (`FNL_CHECK_D3D12` in
  `tools/headercheck.c`): the win64 tree's first build, as C11 with
  `COBJMACROS` and as C++11 to C++20, against mingw's `<d3d12.h>`.
- ~~**X11's sync value kept for its frame, run out at a drag's first press**~~
  (`SYNC_SHOWN_WAIT`, `x11_present_event()`): `x11-vulkan`'s stress round
  says so once in release and in `gc`, `pace.py --border` never (DESIGN *X11:
  xcb, not Xlib*). Logged (`FNL_X11_PRESENT_LOG=1`): the in-order match is
  about five frames behind once swapchains have been made again, frames
  queued that are never answered -- Mesa's, presented to a swapchain retired
  before it sent them. Matched by serial now (DESIGN): `x11-vulkan`
  here clean, and `x11-vulkan-bare` on the thinkpad, aspire and the Pi
  (2026-11-02). The shown stamps read back through sizes (`pace: shown`):
  right on `x11-vulkan`, and on the bare machines' Xorg (the thinkpad,
  aspire, the Pi). The stamp before its latch on aspire was
  NVIDIA's out-of-date present shown anyway (DESIGN), counted now; the
  rounds after it clean (2026-11-02: `x11-vulkan-bare` on
  aspire, 96 checks; `x11-vulkan` 106 and `wayland-vulkan` 74 here).
  Wayland's take-back answers the same where the compositor has spoken
  first (DESIGN); its rounds clean (2026-11-02: `wayland-vulkan-bare` on
  aspire, 66 checks; `wayland-vulkan` here, 74).
  It recurs in logs from 2026-11-02 through 11-06 after the serial matching
  above: that fix was real, and the bound was the other half.

  **Closed (2026-11-21)** by measurement rather than by a theory. The wait was
  two periods at 60 Hz -- 34 ms, Vulkan's bound for the same wait -- and
  `FNL_X11_PRESENT_LOG=1` over 145 completions on this desk puts the latency at
  16.4 ms median, 17.5 at p90 and **49.6 at its worst**, with 2.8% past 34 ms.
  One value that runs out logs the line the round calls a failure and turns the
  shown-wait off until the next answer, so a round long enough to present a few
  hundred frames was near-certain to trip it. `SYNC_SHOWN_WAIT` is 0.100 now:
  above every completion measured, and still a bound on what the manager waits
  for. `x11-vulkan` passes its round, 111 checks.
- **Input held for `bench.py --hands` and pace's border row**, which
  drive input as a round does without the round's hold
  (`input_hold.py`).
- **The pass after it, the same day** (CHANGELOG 0.9.0's first lines), built on
  the linux and win64 release trees, the macOS release tree and the linux clang
  tree, warning-free with every in-build check passing, and no round run: X11's
  pointer grab let go with the focus and taken back, under mutter and with no
  manager, a capture switched away from and back; Wayland's unaccelerated
  relative motion, on GNOME and weston; the clipboard's CRLF at Windows' edge,
  both ways; Win32's candidate list at the caret, with a real input method;
  Cocoa's composition dropped at a focus loss; `fnl_vulkan_scene_rect()` in
  `examples/scene_vulkan.c`, direct and offscreen; a calibration ending on a GPU
  named by name and by index; two programs calibrating at once over one ranking
  file. (The fault sites DESIGN's table had as *Unrun* were run on
  2026-11-03, each firing as written, the rounds otherwise clean.)
- **The review's rounds of 2026-11-03, at `ebc3ba8`**, half done. Every
  tree built warning-free from nothing, fifteen with the ARM four, and
  release rounds ran clean everywhere but this desk: the bare machines'
  X11, weston and KMS, Vulkan (aspire, the thinkpad, the Pi:
  eighteen, aspire's KMS on nouveau and nvidia), the Mac's three (91
  checks each), and schenker and aspire under Windows (Vulkan and
  Direct3D 12 on both GPUs: 107 and 106 checks). `gc` with
  `FNL_FAULT=d3d12-composed,d3d11-present-refused` on both laptops' both
  GPUs fired as DESIGN's table writes it, once a device, the rounds
  otherwise clean. The checker rounds were run on 2026-11-06 at
  `ca042a7`: `wayland-vulkan` under asan, tsan and gc with the validation
  layer, 75 checks each and none failed (Wayland's frame callbacks), and
  `x11-vulkan` under the same three, 109 checks each, none failed but the
  known sync-value line in gc (the present wait). tsan found radeonsi's
  threaded-context command stream handed between the render thread and
  the driver's own (`amdgpu_cs_flush()`, `amdgpu_do_add_buffer()`,
  `si_begin_new_gfx_cs()`) when the GPU box moved the window onto another
  device; each is suppressed in `tsan.supp` with its frames named. Owed still:
  - what no round does: KMS with two layouts (`FNL_KMS_LAYOUT=us,de`)
    and `compose:ralt`; a tablet or touchscreen on KMS; two monitors of
    one model on the Mac; a real input method on Windows;
  - DWM composing a window by itself: the measure found the window at
    rest composed with no fault on schenker's HD 630 and GTX 1050 Ti and
    aspire's HD 530, and only aspire's GTX 960M needed `d3d12-composed`.
    Against DESIGN *Measured on Windows itself*: is that the windowed
    case it expects, or a window that should be on independent flip?

## The dissolve, after its review

Fixed by reading (DESIGN *The veil*) and not yet measured.

- **NVIDIA's own swapchain holds the second acquire some 200 ms** on
  aspire's GTX 960M, which does not drive the panel, so a move's first
  frame reaches the screen a quarter second late there (DESIGN *The
  veil*, 2026-10-31); through DXGI, the default, it does not. Owed: why,
  on that swapchain alone. A hidden move shows the same on both GPUs'
  own swapchains: the restore gets the first frame when the second
  acquire returns, 0.21 to 0.23 s after it, on the HD 530 as well
  (DESIGN *The veil*, 2026-11-01).

## Linux

- **A dip every 21 s on this desktop's console at 4K**: 44 to 48 frames
  a second for about 3.5 s, then 60 again, the RX 590 on DP-1 at
  3840x2160. Not the library's: `vkcube --wsi display`, with none of it,
  dips the same (4.5, 26 and 47 s, Mesa's overlay layer logging); the
  kernel's connector polling off
  (`drm_kms_helper.poll=N`) changes nothing, and no DRM uevent comes with
  it. What it is wants hands: another monitor on DP-1, the dark DVI-D-1
  unplugged, amdgpu's DC debug. Seen as "45 frames a second without
  vsync" before: the display path has no tearing mode there, so vsync off
  runs FIFO at 60 between the dips. Since the period estimate holds there
  (DESIGN *Late latching*) the dip costs frames of 17.8 to 18.5 ms, not
  21 to 25.
- **The thinkpad's resets**: three, each under lavapipe on four threads,
  nothing in the journal; heat, most likely. Its fan is at its fastest on
  `auto` -- with `fan_control=1` every fixed level held it slower -- so
  what is untried is less heat: a round with `LP_NUM_THREADS=2`, or the
  fan cleaned. Nine checker rounds back to back at two threads (2026-10-15,
  every server, `asan`, `tsan` and `gc`) ran without one and ended at
  55 C; then a night of it (2026-10-17 18:49 to 07:00, `LP_NUM_THREADS=2`,
  X11, weston and KMS rounds at `release`, `asan` and `tsan` in turn): 599
  rounds, 66 or 67 of each, none failed, one boot throughout, 41 to 55 C.
  Two threads keep it up, and the machine's line in `bare-hosts` now
  says `LP_NUM_THREADS=2`, which every round, `pace.py` and `bench.py` run
  there takes (`bare_driver.machine_env()`). The sweep of 2026-10-23
  (`docs/benchmarks.md`) ran its twelve rounds there at four threads, one
  boot, without a reset. Owed still: four threads with the fan cleaned.
- **The X11 auto-repeat filter** for a server that refuses detectable
  auto-repeat, and the Mode_switch columns for one without XKB, both
  UNVERIFIED in `x11_keys.c`. No server from X.Org's tree runs without
  XKB: Xorg 21.1.16 has no option for it, and Xvfb started with
  `-extension XKEYBOARD` says the extension "can not be disabled" and
  lists it anyway. Owed to a server of another lineage, if one turns up.
- **Two Mesa reports**, drafted with reproductions in
  `docs/mesa-x11-sw.md`; the tracker searched (2026-10-15, neither there)
  and both still in main. Owed: sending, from the account that files them.
- **NVIDIA's own driver on the laptops' NVIDIA GPUs**, which on Linux
  have only been NVK's: its WSI on X11 and Wayland through PRIME offload
  (both laptops' outputs are the Intel card's) and the automatic choice
  with `device_passed_over()` not matching. Pascal and Maxwell are the
  legacy 580 branch's (`nvidia-580xx-dkms` and `-utils`, AUR, closed
  kernel module).
  - *schenker* boots it from its own persistent stick, the NVIDIA entry
    the default and the NVK one a choice in the menu (installed by hand
    from the 2026-10-17 archive, the key's entries and modprobe file
    repeated). Xorg and weston rounds there pass on the GTX 1050 Ti;
    KMS cannot use it (DESIGN *KMS: no window server*, *NVIDIA's own
    driver takes neither path*). Xorg rounds lose the window in the
    eight drag-resizes, 7 of 13 ("swept the screen and found no
    window"): the render loop rebuilds for good, every acquire on each
    new swapchain `VK_ERROR_OUT_OF_DATE_KHR` (21 rebuilds in 0.4 s,
    traced in gdb), though the X server, `currentExtent` and the
    swapchain all say one size (1392x860) and the window holds still.
    No frame, so no events, and the round sees nothing. NVK there and
    weston on this driver do not. `tools/probe/wsidrag.c` (the
    library's rebuild, Vulkan and xcb alone) gets stuck so, 3 and 4
    times in 20 rounds, once it rebuilds at each ConfigureNotify's size
    as the library does; taking `currentExtent` and rebuilding only when
    told out of date, 60 rounds never did. Once stuck, nothing on the
    Vulkan side frees it (the device idle, no `oldSwapchain`, a new
    surface, a new X connection, half a second with nothing alive), nor
    a configure at the size it has; the window a pixel wider and
    straight back does, at the first rebuild, 4 of 4. Intel's GPU on
    the same probe: none in 20 rounds. The library now does that after
    a run of eight (DESIGN *X11: xcb, not Xlib*, `stale-swapchain`),
    and with it 8 of 8 Xorg rounds there passed (68 checks), the nudge
    taken in 3 (2026-10-19). Under `asan` with the validation layer the
    fault site is clean here (`stale-swapchain%10`, 12 nudges, the
    window at its own size after each), but GNOME on XWayland carried
    out 1 of about 10 pairs, where the same pair sent by another client
    took every time: why is open, and matters only if the freeze is
    ever seen under a manager. With nothing named the library chooses
    the GTX 1050 Ti itself, and Xorg and weston rounds pass on it
    (2026-10-19, the nudge taken twice in the Xorg one); so do `asan`
    and `gc` on both; `tsan` cannot run on it (DESIGN *NVIDIA's own
    driver is checked by ASan and not by TSan*). `pace.py` against the
    NVK rows of 2026-10-18: weston keeps every rate at 60 fps for less
    CPU than Intel's; Xorg, which fell away as the requests quickened,
    now holds each size until its frame is shown (DESIGN *X11: xcb, not
    Xlib*, 2026-10-29: 51, 60 and 60 fps at 45, 60 and 250 sizes a
    second). (weston's draw row, 40 fps there, was the hold: DESIGN
    *Late latching*.) Owed: the
    report to NVIDIA, drafted in `docs/nvidia-x11-stale-swapchain.md`,
    sent with the probe and an `nvidia-bug-report.log`, after `wsidrag` on
    580.196.07 (nothing upstream matched, 2026-10-31). The
    `nvidia-suspend` units left enabled on the NVK entry stand aside
    (2026-10-19, `systemctl suspend`: the unit done in 62 ms, then the
    kernel's entry); but schenker does not come back from a suspend on
    that entry, deep by `rtcwake -m mem` or `s2idle` by systemd, the
    program drawing on NVK: it answers ping and nothing else, the screen
    black and keys ignored, the journal ending at the entry. Deep hung so
    before NVIDIA's driver was on the stick (DESIGN *A suspend with a
    GPU, on aspire*). Nor from one on the NVIDIA entry, so not nouveau's:
    deep by `systemctl suspend`, the program drawing on this driver,
    `nvidia-suspend.service` done in 330 ms (weston's seat given up),
    then the kernel's entry and nothing after; and so again with weston
    up and nothing on either GPU. The kernel's `pm_test`, no server up:
    `freezer` comes back, `devices` hangs as the sleeps did -- a device's
    suspend or resume, before the firmware is asked anything. Open:
    which, by `pm_trace`. It says nothing
    about nouveau's atlas: this driver never suspends a GPU before
    Turing ("Runtime D3 status: Disabled by default").
  - *The key* is built (2026-10-18, the module for its kernel, 7.2.11);
    both laptops booted its nouveau entry on 2026-10-25 (`fenestal-boot
    once fnl-<kernel>-plain`, then `power.py boot <name> stick`), and
    the KMS rounds on each driver passed there on both, from nouveau
    with NVIDIA's module not loaded and its files put back for the boot;
    what NVIDIA's userspace costs there is measured (DESIGN *Another
    kernel driver, with the window open*). The other bare rounds on that
    entry, drawn on NVK (`FNL_GPU=nvidia`, 2026-10-31): Xorg, weston, and
    wine on Xorg, weston and cage, on both laptops, 91, 62, 73, 54 and 48
    checks, none failed. Owed: the sleep with the GPU included.
  - *Moving between the two with the window open* (DESIGN *Another
    kernel driver, with the window open*): the kernel's half measured on
    schenker, the helper and `bare_driver.py --driver` run there both
    ways. KMS's alone; under a server the boot entry. The library's move
    measured on KMS on schenker and aspire, four moves each, 1.3 to 1.8 s
    and 2.1 s (2026-10-23), and a move refused by a holder seen to go
    back; a KMS round on each driver passed there; both fault sites
    forced; the move taken from the titlebar's GPU box by the round's
    hands, there and back, in the KMS rounds on a driver. Owed: what a
    suspend does with
    NVIDIA's module loaded and the GPU on nouveau (NVIDIA's sleep units
    write to a module with no GPU); and a move drawn across on an AMD
    GPU (RADV is among the drivers handed to the loader, on no machine
    here) or on a machine whose display card is the NVIDIA GPU, where
    the list is empty and says why. Where a move to nouveau's pause goes
    is measured (DESIGN *Drawn across the move*): NVK's instance,
    enumeration and device, none of it the library's. nouveau's bus
    faults on aspire are quieted, by the helper's loads and by the keys'
    nouveau entries alike. The KMS rounds on a driver, with the box's
    move, under the checker trees: schenker's Vulkan rounds on both
    drivers, under `asan` and `gc`, 97 checks each and none failed
    (2026-10-31, `b4571d7`); aspire's Vulkan rounds on both drivers
    under `asan`, 97 checks, none failed, the same day, and under `gc`
    likewise; and `tsan` on
    nouveau passes on both, the drivers preloaded and Mesa's threaded
    context off (DESIGN, 2026-10-31). Owed: a Vulkan round there under
    `tsan` loses F11 now and then and then the window (three times on
    2026-10-31, on schenker and aspire, each passing when run again, no
    TSan report in any): NVK on the copy path draws 14 to 30 fps under
    TSan, and the round's fullscreen waits may be what is short.
  - *The harness* knows the driver (`FACTS`, `discrete_sleeps()` by
    NVIDIA's "Runtime D3 status", the closed ICD in `DEBUGINFO`, `__NV_`
    forwarded, the ICD kept loaded under ASan and `lsan.supp` keyed on its
    two libraries, a `tsan` round on it skipped with the reason). ASan
    needed no `protect_shadow_gap=0`.

## The bare machine, without Xorg

- **A report to weston**, drafted in
  `docs/weston-held-update-xdg-state.md`: a content update held for a
  FIFO barrier is applied with xdg_surface state sent after it (DESIGN
  *Wayland*; worked around here), reproduced alone by
  `tools/probe/heldgeom.c`; main read and the tracker searched
  (2026-10-17, and again 2026-10-31: still there, the same in 16.0.2,
  not reported). Owed: `heldgeom` on 16.0.2, the current release; then
  sending it.
- **The kernel's messages did not go to VT 12, and looked like ours**
  (2026-11-24): `SETUP_ROOT`'s `fenestal-logcons.service` ran
  `setlogcons 12`, which answers 0 whether or not VT 12 exists; the
  kernel honours the redirect only where the target vc is allocated
  (`vt.c`'s `vt_console_print()`), and a machine set up with ttys 1-6
  has no VT 12. schenker's CachyOS printed an i915 `*ERROR*` over a drawn
  frame while the unit reported itself active -- measured there, and the
  redirect shown to work once `/dev/tty12` had been opened, the
  allocation outliving the opener. Fixed in `SETUP_ROOT`: the unit opens
  the tty before `setlogcons`. Owed: `--setup` on a machine whose unit is
  already installed, then a hand at the VT, and the existing units' next
  boot; and the harness does not read the kernel log at all, so a
  message that reaches a VT a round is not on is still nowhere it looks.
  Known, and not fenestal's: `i915 0000:00:02.0: [drm] *ERROR* Atomic
  update failure on pipe A` is the driver's own missed-deadline report
  (`intel_crtc.c`'s `intel_pipe_update_end()`), logged when the update
  straddles a vblank. schenker's i915 sent it twice in four days
  (2026-11-24 10:16 and 10:18, 112 and 135 us in the section), both
  around a burst of three nouveau/nvidia driver rebinds, and neither
  inside a fenestal round; the commit still lands and the frame is
  drawn. Upstream's known signature for PSR, gamma and stale-scanline
  reads under load; nothing owed.
- **A window lost to the harness on weston after an edge resize**
  (2026-10-31): wine's Direct3D 12 round on weston on schenker's nouveau
  entry, 13 checks in, the program drawing at the size the drag left; run
  again on that boot it passed, and weston's own Vulkan round beside it.
  Owed: `tools/probe/heldpresent.sh` (built here and run on the machine's
  weston by `bare_run.py --weston`) the next time it happens, before the
  round is run again. The NVK losses of 2026-10-16 were the harness's
  sweep (DESIGN *The startup inventory*).
- **Weston's move between monitors stalls under the harness** (2026-11-26):
  black in `fenestal-console`, weston 14.0.2 on DP-1 3840x2160 and DVI-D-1
  1920x1200. Both Linux weston rounds find, centre and run their window
  (66 checks), but `cross_monitors()` fails its three: the drag carries the
  window part-way across the boundary and further drags do not advance it,
  so the program keeps reporting `DP-1 ... + DVI-D-1 ...`. The drag is the
  harness's open-loop one (`wayland_driver.Driver.drag()`, the uinput
  hand's units against weston's shell); the same section passes on the
  thinkpad's weston. Owed: what weston 14 does with the motion once the
  window's edge has crossed the output, and either a weston-specific move
  or the section declared for a bare weston, with the measurement.
- **nouveau's runtime suspend garbles tiled images in VRAM** (DESIGN
  *The startup inventory*): reproduced alone by `tools/probe/nvkatlas.c`
  (`NVKATLAS_IDLE=12`, half the texels wrong; never with runtime PM `on`).
  The library no longer picks NVK on a pre-Turing GPU by itself, and
  keeps the atlas in system memory where it is chosen. The same bug on
  Kepler was reported to Mesa on 2026-10-30 (#16729), with a trace of the
  suspend keeping the VRAM page kind. Owed: the comment on #16729, the
  report to drm/nouveau and the comment on #16535, drafted in
  `docs/mesa-nvk-pascal.md`, posted; and whether a Turing or later GPU
  under NVK, which runtime-suspends too, keeps its images.
- **The KMS backend by hand**: whether eight pixels a millimetre feels
  right under a finger. The arithmetic is checked with a uinput pad on all
  three laptops (DESIGN *Input is evdev*); the feel is not. Owed: blocked on
  a hand -- a finger on the panel, which no round can stand in for; a
  second monitor or a multi-mode output would also let the fit be measured
  rather than argued (below).
- **KMS modes and outputs** (DESIGN *Its size is a mode, and its output one
  of the card's*): driven on both paths, and an output switch on the display
  path. Mesa's acquire timeout that was a lost surface is fixed in 26.2.6
  (e102ea9f; `ACQUIRE_TIMEOUT_DISPLAY_NS` stays for an older Mesa). Owed:
  the report of its two other display-WSI bugs, drafted in
  `docs/mesa-wsi-display.md`, sent -- the plane kept for a connector given
  another CRTC (worked round, DPMS off) and the formats leak that fails a
  switching `asan` round on the display path, here and on the Pi; the
  comment for Mesa #15248 beside them, cut to what the issue's comments
  do not already say (NVK's display queries fail on a connector of
  nouveau without `nouveau.atomic=1`); and outputs on a second card, not
  listed.
- **KMS atomic commits** (DESIGN *The commits are atomic where the card has
  them*): built (2026-11-23) -- `kms_atomic_enable()` at open sets
  `DRM_CLIENT_CAP_ATOMIC` and reads each CRTC's primary plane, and the mode
  set, page flip, output unbind and console return each go by
  `DRM_IOCTL_MODE_ATOMIC` where the driver takes it, the legacy `SETCRTC`
  and `PAGE_FLIP` kept as the fallback. **Run on schenker's CachyOS**
  (Intel HD 630, eDP-1 + HDMI-A-1), 2026-11-23, `release`: the single window
  on the display path (anv, Y-tiled) and on the copy path (llvmpipe and the
  NVIDIA proprietary surfaceless path), `KMS: atomic commits on card2` in the
  log, no refusal; and two windows on the two outputs, both by the copy path,
  the two flips serialised by a commit lock (two render threads committing on
  the one node took `EBUSY` for a modeset colliding with a flip, and an Intel
  primary plane shared by two CRTCs was found and fixed). `clang` and `asan`
  build clean; an `asan` two-window round there is clean of the library's
  leaks -- the one report is 642 bytes in `libdbus` registered by NVIDIA's
  proprietary ICD at exit, the same single-window, which the harness
  suppresses. `DRM_MODE_ATOMIC_TEST_ONLY` before a mode set is in: a mode or
  plane the driver will not take is refused before the screen changes. Owed,
  in order, all of them **blocked on a driver path no round here reaches** --
  schenker's opened card is Intel (i915), and the two other cards the lab
  has (black's amdgpu, aspire/schenker's nouveau) are a machine and a kernel
  module away: the display path's own mode change and console return on
  `black`'s amdgpu, where `remove_fb` fails, and on nouveau with and without
  `nouveau.atomic=1`; and the **cursor plane** as a commit of its own
  (below).
- **The cursor plane's commit** (DESIGN *The pointer is drawn on the cursor
  plane*): in since 2026-11-23. The open finds each CRTC's cursor plane by
  `drm_plane_type.CURSOR`, one it may show and no other CRTC has taken, and
  each shape and its place go as one `DRM_IOCTL_MODE_ATOMIC` naming the
  plane's `CRTC_ID`, `FB_ID`, `SRC_*` and `CRTC_*` -- the corner the legacy
  ioctl was given, so `DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT` is not asked
  for. A glyph's dumb buffer is wrapped in a framebuffer on its first show
  (`ADDFB2`, ARGB8888) and both go at close, and a CRTC with no cursor plane
  or a refused commit keeps the legacy `DRM_IOCTL_MODE_CURSOR` for that
  window's life, one line saying which ran (`KMS: the cursor plane on CRTC N
  by atomic commits`). A refusal is taken three in a row before the legacy
  ioctl is the window's for good, since one can be the render thread's mode
  set switching the CRTC off between the lit check and the commit. A Debug
  tree's `FNL_FAULT=kms-cursor-legacy` forces the fallback (DESIGN's fault
  table). Built clean under `clang` and `-fanalyzer`; run by a two-window
  round on schenker's CachyOS (Intel HD 630 on eDP-1 and the NVIDIA GTX
  1050 Ti) on 2026-11-23, `release`, `--capture`: the line is `KMS: the
  cursor plane on CRTC 61 by atomic commits`, every drawn-pointer check
  passes with the window moved between the two outputs, and the round's new
  pointer crossing passes -- 185 checks, none failed, and the two-window
  section eight. The forced fallback was driven on 2026-11-24, the same
  machine, `gc`, `--capture`, `FNL_FAULT=kms-cursor-legacy`: the legacy
  line at the third refusal, every drawn-pointer check passing on the
  legacy ioctl, the two-window section eight with none failed and the round
  185 with none. Owed: a CRTC with no cursor plane, unseen -- it would say
  the legacy line instead, which the round allows as a notice. Blocked on
  hardware: no card in the lab has a CRTC that offers no cursor plane, and
  the path can only be reached where one does.
- **The unplug section with a force that reaches the program and two
  outputs**: no machine has both today -- `black`'s kernel sends no uevent
  for a forced status (DESIGN *KMS*), and the laptops have one output each.
  A second monitor on a laptop, or a cable pulled by hand on `black`.
  schenker has the two outputs and was driven there on 2026-10-18 under
  another kernel; its i915 now sends none for eDP-1 and one write of five
  for HDMI-A-1 (DESIGN's measurement of 2026-11-24), and the section skips
  there again. Owed besides: the probe reads only the write's own moment
  now, so a kernel whose event is late is skipped rather than driven.
  The same reaches the away grace's other half (DESIGN *A monitor that
  leaves is away first*), driven only with monitors that came back: a
  dark output pulled leaving the list after 5 s, the window's own pulled
  moving it after 2.5 s, and a move owed to an output that does not come
  back dropped with its line. Both are blocked on a machine with two
  outputs and a kernel that sends a uevent for a forced status; none in
  the lab has both (schenker has the outputs, its i915 sends none for
  eDP-1 and one write of five for HDMI-A-1).

## The Raspberry Pi 4

Built (`./build.sh linux-arm64`, DESIGN *Cross-compiling for ARM Linux*)
and driven on KMS, release, by a round. The Pi is `pi4` (Arch Linux ARM,
`linux-rpi`), in `bare-hosts` at its wired address (`pi4`), with no
monitor yet.

With no monitor it runs on HDMI-A-1 forced on by the kernel's line (DESIGN
*Cross-compiling for ARM Linux*; the line before is the Pi's
`/boot/cmdline.txt.bak-2026-10-24`).

- **With the monitor**: the forced line kept or taken off, and `pace.py`'s
  columns for the Pi read against a sink.

- **The checkers on the Pi**: asan, tsan and gc built here for ARM and
  round there on 2026-11-05 -- asan (`x11-vulkan-bare`) 87 checks and gc
  (`kms-vulkan-bare`) 59, none failed; tsan (`wayland-vulkan-bare`)
  failed on one lavapipe race. Both its stacks are the driver's
  rasteriser threads (`llvmpipe-2` and `-3`, a memcpy at
  `libvulkan_lvp.so+0x16c994`, the block one of them allocated), the
  race that `race:_mesa_hash_table_copy` in `tsan.supp` covers. The
  function is named by its code, not its symbols: the ARM library has
  none, and Arch Linux ARM's debuginfod has none for any of the Pi's
  Vulkan files (HTTP 404 for all of them, `bare_driver.py --setup`;
  no `debuginfod.archlinuxarm.org` exists). The 196-byte function at
  `libvulkan_lvp.so+0x16c8e0` copies a hash table's 0x48-byte header
  with four 16-byte moves, allocates its entries with `0x18` the
  entry's size and the table's count, and the faulting call at
  `+0x16c994` memcpys the inline entries (`0x1c8` bytes) through the
  GOT -- instruction for instruction what `_mesa_hash_table_copy`
  (`src/util/hash_table.c:234-235`) does in the same Mesa 26.2.6
  built for x86_64, whose debug info names it and both source lines.
  Owed: a symbol source for the ARM library -- the distro's dbg, or a
  Mesa built for ARM with its debug info -- so the existing key
  matches; until then the round fails on the driver's own race, not
  this project's.

## Wine 11 on this desktop

WineHQ's builds are installed beside the distribution's 10.0
(2026-11-01), from `dl.winehq.org`'s `resolute` suite, each under its own
directory in `/opt` and none on the path: `wine-stable` 11.0,
`wine-devel` 11.21 and `wine-staging` 11.21. `wine` is still 10.0 and
runs every wine round here unless `FNL_DRIVE_WINE` names another, each in
a prefix of its own and written to the ledger (tools/drive/README.md
*Another wine than the distribution's*); the three prefixes are made.
One round of each under 11.0 and 11.21 is DESIGN *Wine 11 on this
desktop*. Owed:

- **11.21's keys twice**, `VK_PROCESSKEY` and then the key, once an input
  method has been at them (`tools/probe/keytwice.c` and `.py`): wine's
  bug 60449, which has the cause with a Japanese layout on X11 alone. A
  comment adding Wayland, a German layout and the regression since 11.6
  and 11.7 is drafted in `docs/wine-processkey-twice.md` (master and the
  tracker read 2026-11-01, not sent). Owed: sending it. Until a wine
  without it, no 11.21 round gets past its first toggle; the rest of both
  of its rounds is owed.
- **11.0's X11 clipboard**, which crosses to no other program: why, and
  whether 11.21 mended it or only does not meet it.
- **11.0's Wayland driver after a move between monitors**: an output size
  that is no monitor's, a move back that stays on `DISPLAY2`, and then a
  press on the titlebar's controls that reached nothing and a window the
  round lost -- whether the harness's reckoning from a point the echo
  confirmed, or wine's geometry. The rest of that round is owed: the
  clipboard and the desk's layout under 11.0's Wayland driver.
- **`wine-staging`** 11.21, not yet run.

## Wine on Wayland

- **A later wine.** 10.0's Wayland driver has no clipboard, never says a
  window changed monitor, and types only the letters through the desk's
  layout. 11.0's says the monitor changed, with the wrong size; the
  clipboard and the layout under 11.0 and 11.21 are *Wine 11 on this
  desktop*'s owed rounds. Wine's own vkd3d runs DXIL from 11.0
  (`FNL_DRIVE_VKD3D=wine`).
- **The corner after a corner resize, with more than one output.** Under
  10.0 and 11.0 stable (measured 2026-11-26) the grown part of the window
  takes no pointer input: motion stops at the old surface's edge and a
  press there is never delivered, so the round cannot drag its corner
  again. With one output the same drags pass. The round leaves them out
  where `REGROW_UNHELD` holds (tools/drive/environment.py). Owed: the
  rounds under 11.21 and 11.22, which the declaration is lifted for, and
  a report to wine; the declaration comes off when its own round passes
  without it.

## Wine on the bare machine

- **The GPU box's pick under wine on thinkpad's lavapipe** does not
  always register: `win64-d3d12-bare@thinkpad` failed so three times on
  2026-10-23 (one, two and six checks, the six when the harness then lost
  the window) and passed when run again each time. Three rounds back to
  back on 2026-10-31, lavapipe at two threads (`bare-hosts`), passed
  whole, 71 checks each, the pick registering every time. Owed: the
  click's echo read at the time of the next failure, whether the list
  opened at all, before the round's patience is stretched for it.

Each of these is wine's (11.20), measured and declared or worked around in
the harness (DESIGN *Wine on the bare machine*); a later wine is owed a
round without the declaration.

- **The X focus with no window manager.** Wine sets it on a window it
  keeps itself only when a child of that window has the Win32 focus, so a
  top-level window never has it, and wine then clips no pointer for it:
  the round gives it the focus.
- **Fullscreen back to maximized on weston**, which wine's Wayland driver
  loses to weston's answer to its first request of two:
  `MAXIMIZED_LOST_IN_FULLSCREEN`.
- **The layout on Wayland**, weston's and cage's alike: under German, Y is
  y and the o-umlaut key a u-umlaut.
- **Wine on KMS** itself, with no compositor between: cage stands in.
- **Wine's parked client surface** itself: wine 11.20 keeps a destroyed
  swapchain's surface attached and showing until the window is destroyed
  (!11351), and its Wayland driver stacks a new child's subsurface under
  it. A report drafted in `docs/wine-parked-surface.md` (master read and
  the tracker searched 2026-10-31, not reported), with
  `tools/probe/parkedchild.c` as its reproducer. Owed: the probe built and
  run on weston and cage, and on Windows; then sending it.
- **A wheel step lost on Wayland**, one pair in forty: the server merges
  a step into the WM_MOUSEWHEEL the window's thread holds unaccepted,
  and frees both; the library reads the wheel from raw input under wine
  and loses none (DESIGN *Wine on the bare machine*). A report to wine,
  drafted in `docs/wine-wheel-merge.md` with a one-line fix tested
  against `tools/probe/wheelcount.c`; master and the tracker read
  (2026-10-21, still there, not reported). Owed: sending it.

## macOS

Cocoa beside Win32, Wayland, X11 and KMS, and Metal beside Vulkan and
Direct3D 12, on schenker's macOS stick (DESIGN *macOS*): the `macos` tree,
cross-compiled here (DESIGN *Cross-compiling for macOS*).
Written: the backend (`src/backend/cocoa/`), the Metal renderer
(`src/renderer/metal/`), Vulkan's bridge on MoltenVK (`surface_cocoa.c`,
with the portability extensions), `fnl_metal.h`, the testbed's and a
snippet's scenes, the toolchain, `build.sh`'s `macos` target, the checks'
tables, and the harness's environments for the Mac (`mac_driver.py` and its
agent; `macos-metal-native` and `macos-vulkan-native`,
and two `pace.py` columns). Owed, in this order:

1. **What the design assumes and has not seen**, each answered in DESIGN
   *macOS* with what was seen: the ISO swap, on the stick's own keyboard
   (type 43) and a USB one; a covered window's drawables; and the
   capture's deltas, which are AppKit's accelerated ones where Win32's
   are raw. A second monitor is seen: since 2026-11-06 the stick drives
   the built-in panel and the external iiyama PL4071UH from power-up;
   both survive a display sleep, and an external plugged in after login
   shows beside the panel (DESIGN *macOS*; tools/drive/README.md
   *Driving a window* says what the stick and the firmware do). A
   maximized window moved between them and back with its state kept
   (2026-11-05, read from the window's reports; the built-in was dark
   then). Run since with both panels lit: the round moved the window to
   PL4071UH and back under Metal and Vulkan, 95 checks each and none
   failed (2026-11-06).
2. **What the harness has and has not run** (`mac_driver.py`,
   `mac_agent.m`): "dead-layout" through macOS's input sources runs there
   and passes (2026-10-31, U.S. International - PC selected from the ssh
   session, the window typing through it, every round on the Mac); a
   picture is refused while a window not the program's, the Dock's or the
   menu bar's is in front of the window (`over`), which is what the
   Screen Recording grant's dialog asking to be confirmed again would be.
   A reset (`tccutil reset ScreenCapture`, 2026-11-06) clears the decision
   and `screen` reads 0, but an ssh session's capture is then denied
   outright -- `screencapture` says "could not create image from display",
   no dialog -- so the re-confirmation is macOS's own timer and cannot be
   forced that way. Owed still: that dialog seen, its owner and layer, and
   whether `screen` reads 0 while it is up.

- **The bench's three repeats are the teardown's test**: a place runs
  three lifetimes in one process, closing one window and opening the
  next, which is what found the view outliving its App (DESIGN *macOS*,
  2026-11-05). A harness change that gives a place one run a start loses
  the only automatic test of it.

## macOS on Apple's processors, and the app bundles

Written 2026-11-04 and built; seen on 2026-11-05: CI's x86-64 bundle
opened on schenker (`open -n`, and run direct from `Contents/MacOS`),
`codesign --verify --deep --strict` passing, MoltenVK loaded from
`Contents/Frameworks` under Vulkan (dyld's own line), and Vulkan and Metal
each drawing a complete record at 60 fps from the bundle; and
`./build.sh macos-arm64` built here, an arm64 Mach-O, its first on this
desktop. CI builds both trees and joins them with `lipo` into the
ad-hoc signed bundles, MoltenVK in one only where its program has a
Vulkan scene (`.github/stage.sh`). Owed, in this order:

1. **The arm64 half run anywhere**: no Mac here has Apple's processor.

MoltenVK is staged only where a scene is Vulkan's: the testbed and
every demo but hello carry `scene_vulkan.c`, fenestal-dev has none, and
CI's zip of 2026-11-05 lists exactly those six bundles with it, at 43.9
MB against 50.5. A bundle without it runs on the Mac -- `hello.app`
opened under Metal, its log saying `Vulkan lists 0 GPUs here`,
and named Vulkan it refuses with the loader's own line.

## CI

`.github/workflows/ci.yml`: each target's release tree on a push to `main`
and on a pull request -- Linux, Windows and ARM Linux in an Ubuntu 26.04
container, macOS x86-64 and arm64 on a `macos-26` runner -- and ruff. A
push of a `v*` tag builds the same trees and attaches their archives to a
GitHub Release, named for the tag. A Monday run, or one asked for by
hand, builds the checker trees too -- clang, asan, tsan, analyzer and gc
on Linux, asan, tsan and gc on ARM, analyzer and gc on Windows, gc on
both Macs -- and runs none of them. The run's artifacts are the commented
source zip and one archive per target, uploaded as they are (`.tar.xz` for
Linux, `.zip` for Windows and macOS); the Linux job hands the Mac its font
and headers through the cache. Owed:

- **A push that started no run** (`1bd1c42`, the push said it failed and
  had landed): started by hand then; not seen since.

## The ABI baseline

- **The Direct3D 12 baselines were dumped under wine**, by a mingw build of
  `tools/abidump` (the layout is the same on both 64-bit ABIs), and the
  Metal lines in every baseline natively on this desktop. The native trees'
  own dumps check them, so the Windows runner's and the Mac's release builds
  are the first to say so for theirs; a difference fails that tree's check.

## Direct3D 12

A second renderer beside Vulkan: D3D12 on Win32, in every Windows tree
beside Vulkan and chosen at `fnl_init()` (DESIGN *One build per operating
system*): `win64` draws with Direct3D 12 and Vulkan. Direct3D 12 is
Windows' own and is never linked beyond the system's libraries.

In this order, each step leaving every round passing. Done: the Vulkan
renderer's state behind `App::renderer_state`, with no file of `src/core/`
excused by `check-layering.py`; `on_record`'s contract, which `fenestal.h`
states for every renderer (DESIGN *Recording*); and one shader source,
translated by `spirv-cross` and checked at build time (DESIGN *One shader
source*).

1. **D3D12 on Win32** is written (`src/renderer/d3d12/`, the `win64-d3d12`
   environment, `fnl_d3d12.h`, a scene for the testbed, and a snippet):
   DXGI flip discard, the frame-latency object waited on before each frame,
   sync interval 1 or 0 with `ALLOW_TEARING`, and DXIL from `dxc` (DESIGN
   *One shader source*). It records afresh every frame (DESIGN *Recording*).
   It passes the stress round under wine on vkd3d-proton, on X11 and on
   Wayland (release and `gc`), in both guests on WARP, and on both laptops
   on each GPU; the `analyzer` tree builds clean. The frame-latency wait is
   skipped on the frame that answers a size (under wine on Wayland the two
   waits held each other to their bounds on every maximize) and under sync
   interval 0 (on Windows it held vsync-off to the refresh rate: 8 to 22% of
   a core flat out, 17 to 63% once skipped); see `frame.c`.
2. **The harness**: four environments (`win64-d3d12`, `-wl`, `-vm`,
   `-native`), wine's two on vkd3d-proton (`tools/vkd3d_proton.py`).
   Flat out (`pace.py --no-vsync`, twice): under wine on X11 5,854 fps
   at 200% of a core where Vulkan reaches 7,168 at 111%, and on Wayland
   6,704 at 202% against 7,950 at 101% -- vkd3d-proton's and DXVK's
   threads. On the laptops' Windows Direct3D 12 costs less CPU a frame
   than Vulkan on every GPU (DESIGN *Vulkan through DXGI*, 2026-10-31).
   Under wine on Wayland the first present at a new size takes 110 to
   175 ms at 4K, against size_await()'s 250: one round in eight said "no
   frame at the new size", the one run while both guests drew on this
   machine's CPU. Load alone does it (2026-10-31): idle, three rounds
   without the warning; with six of eight threads busy, one of three said
   it three times, the presents around it at 55 to 66 ms. Owed: the
   first present's own time, which the library logs nowhere.

Questions, each to be answered with a measurement:

- **Direct3D 12 draws fewer frames through a drag** than Vulkan under
  wine, counted over the press (2026-10-31, DESIGN *X11*,
  `docs/benchmarks.md`): 35 to 37 on X11 and 18 to 23 on Wayland, where
  wine's Vulkan draws 53 to 58; its intervals up to 55 ms with 37
  missing. Under wine there is no sync counter and `size_await()` holds
  each size. Owed: the `--frame-times` lines' `other` under wine, to
  name the gap.
- **Resizing under wine** shows the new size before its frame: wine's X11
  window has no `_NET_WM_SYNC_REQUEST` (DESIGN *X11*). Recorded now
  (`tools/drive/screencast.py`): black margins. A later wine, or wine's
  Wayland driver, is where it closes.
- **NVIDIA's MAILBOX on Windows**, on a GPU's own swapchain -- no longer
  the default, which goes through DXGI and tears with vsync off (620 fps
  on schenker's GTX 1050 Ti, 871 on aspire's 960M; DESIGN *Vulkan
  through DXGI*), but what `FNL_VK_DXGI=0` and the GPU box's plain entry
  give -- holds the 1050 Ti to two frames a refresh (120 fps; IMMEDIATE
  490, Direct3D 12 900) where the 960M on the same driver drew 683
  (DESIGN *Measured on Windows itself*). aspire's MAILBOX frame is 1.45
  ms with the acquire at 0.01 where schenker's waits up to 7.7 ms, and
  not because of the display's scale: aspire at 125%, schenker's
  setting, still drew 569 (690 at its own 100%), and not because of
  NVIDIA's present-method setting: unset on both, and set either way on
  schenker it changed nothing (DESIGN, 2026-11-01). What is left is the
  hardware, Pascal against Maxwell; owed only if another Pascal card
  (or a later driver) is to hand.
- **D3D12 under wine** goes through vkd3d-proton, since wine 10.0's own
  vkd3d runs no DXIL (DESIGN *One shader source*), and in the guests
  through WARP; which of the Win32 rules (`size_await()`, the reaper) are
  Vulkan's and which Windows', measured again under DXGI. A later wine
  (vkd3d 1.16 or newer) would let its own vkd3d be tried.

## Moving to another GPU

Written (DESIGN *Moving to another GPU*): the list, `fnl_request_gpu()`,
`FNL_EVENT_GPU` and the titlebar's GPU box, built and driven. Owed:

- **What has not yet moved** (Vulkan here, under wine, on both laptops'
  Windows and on their bare X11, weston and KMS, and Direct3D 12 on both
  laptops have, DESIGN *Moving to another GPU*): Metal where a Mac has two
  GPUs.
- **Another renderer on a Mac with two GPUs**, as Metal's own moves are
  owed there: Metal's and MoltenVK's lists on one GPU have been crossed,
  not on two.
- **A move entry for each renderer, and the ranking** (DESIGN *Another
  kernel driver, with the window open*, *Ranked by measured frame
  rate*): on schenker's KMS, a calibration of eight entries kept; a
  start that names nothing opened on the first, with
  no move after; and `kms-vulkan-bare@schenker:nvidia`, opened there,
  through the driver cycles and the walk from every row to every other
  in the ranked list (`gpu_every_pair()`), 56 switches, 10 of them to
  another driver, every one landed in one process with the scene moving
  -- and again on 2026-10-26 at b5c2a34, 97 checks and none failed, the
  log clean; and with HDMI plugged in, `@schenker:nvidia` and, booted on
  nouveau, `@schenker:nouveau`, which opened on the first entry by
  moving the GPU to nvidia, 105 checks each, none failed; and a round
  pinned to HDMI that ran the GPU sections again on the panel
  (`gpu_on_other_outputs()`), 164 checks, none failed, the walk on HDMI
  moving the driver 12 times in its ranked order -- booted on nouveau
  and again booted on NVIDIA's driver, alike.
  aspire's KMS ranking, six entries since 2026-10-28, took its two
  kernel-driver moves on 2026-10-31 (16 s, a start with
  `FNL_GPU_DRIVER=switch`): eight entries, Intel's Vulkan first at 397
  fps, the GTX 960M's 396 on nvidia and 130 on nouveau; and
  `kms-vulkan-bare@aspire:nvidia` opened on the first and walked the 56
  switches, 97 checks, none failed; and under Windows on both laptops,
  where no driver moves (DESIGN *Ranked by measured frame rate*,
  2026-10-31).
- **The calibration, entry by entry and display by display** (DESIGN
  *Ranked by measured frame rate*): on schenker's KMS, the eight entries
  measured on eDP-1 at the first start (31 s), and on HDMI-A-1 once the
  window was moved there (33 s, the 4K output reordering the list); a
  start with HDMI unplugged and one with it back measured nothing, every
  figure kept. With the window open,
  HDMI pulled and pushed measured nothing; pulled while the window was
  measured on it, the calibration ended with 3 of 8 kept and the window
  on the panel; pushed back and the window moved there, the 5 missing
  measured (21 s). A move between displays put the window on the
  display's first entry by itself, nothing measured. A calibration
  ended by its display pulled (HDMI at entry 7 of 8, 6 kept) put the
  window on the panel's first entry, the GPU moved back to nvidia. The
  time left, reckoned per group, within a second of what it came to
  (33 s at the start, 29 at 3.7 s, 11 at 22.5 s); keys, clicks and
  motion held back by hand, nothing posted during the calibration, a
  release included, and F2 posted after it. Bare X11 and bare weston on
  schenker, GNOME's X11 and Wayland here, and aspire's and thinkpad's
  KMS, Xorg and weston (DESIGN *Ranked by measured frame rate*). Owed: a
  monitor unplugged mid-calibration under a window server.
- **A GPU plugged in or out with the window open** (DESIGN *Ranked by
  measured frame rate*, *A GPU plugged in or out*): built on KMS, never
  run -- no GPU here can be plugged. Owed with a dock or an external GPU:
  the uevents' shape for one (a card node added, and removed); the
  rebuild that lists again and its 1.5 s wait for udev; the new entries
  measured; the GPU in use pulled. And a way to hear it on the window
  servers' backends, which hear none yet.
- **Work for the second GPU** (`docs/multi-gpu-offload.md`, an
  assessment, nothing built): on schenker and aspire the other GPU idles
  while every NVIDIA frame crosses to Intel's card. The testbed's
  heaviest scene leaves the HD 530 a third busy at 60 Hz and the 960M
  drawing 152 fps (the doc, 2026-10-31): no pressure to relieve with it.
  Owed, for a caller's heavier scene: KMS's hand-over by dma-buf and
  NVIDIA's copy engine in place of the CPU copy, whether NVIDIA's 580
  branch has the extensions it needs, and the scene split at the scene
  target.

## Input latency

Measured by kind of input on every backend (DESIGN *Input latency*,
2026-10-31): weston, Xorg, KMS, GNOME's Wayland and XWayland, wine and
the Mac, with the harness's hands. Owed:

- **Win32**: measured on schenker under Windows with the raw input
  thread, and under wine on the thinkpad; a key typed into another
  program, and a program with raw input of its own, seen on aspire
  (DESIGN, 2026-10-31). Owed on Windows a hand at a real keyboard and
  mouse beside the agent's `SendInput`.
- **Cocoa**: measured on schenker's macOS with the agent's posted events
  (DESIGN). Owed: a hand at a real keyboard and mouse, the agent's events
  being posted above the HID drivers, and where half its motion comes
  unstamped.

## The benchmark mode

`docs/benchmark-mode.md` is the plan; its phases are built and run on
every system (DESIGN *Benchmarking*). Owed:

- **GNOME and a GPU that does not drive the panel** (DESIGN *On Wayland
  a move presents into a subsurface of its own*): weston and cage both
  show the own surface missing a refresh in one or two hundred and the
  subsurface missing none 4 to 11 ms later, which is why the window
  keeps opening on its own surface. Owed: the same two runs on GNOME
  (mutter), which wants a machine whose drawing GPU does not drive its
  panel and GNOME on it; none here has both.

- **The KMS places whose panel lists one mode cannot be fitted**: the
  laptops' and the thinkpad's KMS window is their output, and each
  output has one mode, so `--sizes` leaves the area unchanged there and
  the fit has one point (2026-11-05). Owed: a second monitor on one of
  them, or a machine whose output lists more, so the copy path and the
  display path are fittable there as the Pi's KMS is.

## An overlay image

The overlay draws text, rectangles, strokes and boxes, and no picture. An app
with a logo, an icon or cover art draws it from strokes -- the first program
built on this library drew its whole titlebar that way -- and the only image
path in the tree is `FnlRendererVtbl::overlay_cover`, which blits the last
frame over the whole window for a move. Nothing about it generalizes: it is
one full-window quad with the scene's own image, and `App::ovl_img` is the
swapchain image index rather than a picture.

Built, on all three renderers (2026-11-20):

1. **The object and its pixels.** `fnl_image_create(app, w, h, format,
   pixels)`, `fnl_image_destroy()`, `fnl_image_supported()` and
   `fnl_overlay_image(app, x, y, w, h, img)`. One format -- RGBA8, not
   premultiplied, which is what the glyph atlas and the cover already use --
   and the enum appended to as more are wanted. The caller's pixels are
   copied, so it may free them on return.
2. **A shader pair**: `shaders/image.vert` and `shaders/image.frag` beside
   rect/shape/text, translated by spirv-cross at build time as those are.
   The vertex stage places the quad itself, from a push constant, so an image
   needs neither a vertex buffer nor the overlay's dynamic viewport.
3. **Lifetime and device loss**: the library keeps the CPU copy, so a device
   rebuild (`on_gpu_created`) uploads it again. Each renderer destroys its
   images' objects in its own device teardown (`vk_images_device_gone()`,
   `d3d_destroy_overlay()`, `mt_destroy_overlay()`)
   and leaves `FnlImage::gpu` null; the next draw uploads. That is what a
   swap to another renderer rests on too: the renderer that owned an object
   is the one that nulls it, so the next renderer's `image_free()` is never
   handed one that is not its. What is left at `fnl_shutdown()` is freed
   there.
4. **The three pipelines**: `src/renderer/vulkan/overlay.c` (an image, its
   memory, a view and a set from a freeable pool, one shared sampler),
   `src/renderer/d3d12/overlay.c`
   (a texture, an upload list and an SRV from a heap of the images' own) and
   `src/renderer/metal/overlay.m` (a texture, held as an owned `void *`).

Known, not owed:

- The ordering is call order for rects and strokes -- the core flushes both
  before an image -- and text stays one draw at the end of the frame, so a
  glyph appended before an image still lands on top of it, as it already did
  for a rectangle.

## The review of 2026-11-20, what it left

Found and fixed: an image's push constants left standing over the overlay's
(Vulkan); a texture released with the other frame slot's list still naming
it (Direct3D 12), and the same hole on Vulkan's picture, whose view and
descriptor set now wait as Direct3D 12's drain; a timestamp pair surviving
a swapchain rebuild; ENTER and LEAVE in surface points on Wayland; the LED
bit test by bytes on KMS; an `NSCursor` set per motion on Cocoa; `GETCRTC`
per cursor report on KMS; and X11's blocking `Xft.dpi` read per notification.

Open, each needing a decision or a measurement:

- ~~**The GPU ranking's per-frame re-derivation**~~ -- **closed (2026-11-21)**:
  `app_rank_tick()` derives the display only when `App::display_gen` moved,
  and every publish that can change the display bumps it -- the size, the
  output, the output's own mode, the refresh rate, the outputs' list. The
  case this entry named is the one that made a stamp necessary rather than a
  comparison of published fields: a mode switch that keeps the size moves
  the refresh rate alone, and `app_publish_refresh()` bumps the stamp for it.
- ~~**Wayland's held-key guard**~~ -- **built (2026-11-21)** anyway, since the
  guard costs nothing where nothing repeats: `kb_key()` sends a press of a key
  the caller already holds down the repeat path -- `app_key_repeat()` and the
  text, never the binding -- on the held set `kb_enter()` already seeds from
  the compositor's array, which is X11's guard with the set the protocol
  hands over. Whether a compositor repeats is now a thing that cannot be
  heard wrongly rather than one to be waited for.
- ~~**Win32's clipboard `Sleep()`**~~ -- **closed (2026-11-21)**: the waits
  are `pump_sent()`, `size_await()`'s idiom, so the thread dispatches what
  another window *sends* it and takes nothing off its queue. The re-entry
  the review asked to judge turns out not to need one: `WM_SIZE` is posted,
  which `QS_SENDMESSAGE` excludes, and `clipboard_open()` waits before the
  clipboard is open, so nothing is held across a handler.
- ~~**Per-frame figures that move slowly**~~ -- **settled (2026-11-21)**, two
  cached and one declined. The calibration panel's width is frozen in
  `plan()`, where the ids are set (`App::cal::panel_w`), so the draw no longer
  measures every entry again on every frame; and the metrics panel keeps its
  eight formatted figures keyed on each row's label and unit, regenerating
  one only when the value or its decimals move. The third is declined: the
  controls' `BACKEND(a)->outputs()` is `backend_published_outputs()` on X11
  and Wayland -- a mutex take and a copy of published state, no window-server
  round trip -- and `kms_outputs()` is a few atomics and its own array. Two
  of those and ten arithmetic recomputations are not worth a cache whose
  staleness would misplace or unclickable the window's only controls, which
  is the hazard this entry named itself.
- ~~**Startup, not frame**~~ -- **closed (2026-11-21)**: Vulkan enumerates
  the device's extension list once and keeps it on the renderer's state,
  where a device creation used to make about twenty round trips for ten
  names; Metal keeps each compiled MSL stage by the source it came from, so
  the overlay's fourteen asks are eleven compiles, emptied at the top of
  `mt_create_overlay()` so nothing outlives its device. Both were once per
  device and measured as no cost, so what moved is the work, not a figure.
- ~~**`win32_host()` per `WM_MOVE`**~~ -- **closed (2026-11-21)**: probed once
  in `win32_open()`, where wine's display driver has loaded, and kept on the
  backend state; before open it still probes, which is the status line's one
  call and nobody's frame. `under_wine()`'s loader lock and ntdll scan no
  longer run per step of a window drag.

What verifies any of the pictures is now in the tree.
`fnl_image_create()` had one caller, `examples/image.c`, whose
`snippet-image` is built into every tree and run by nothing, and the round's
`read-pixels` evidence was a row for motion and blackness -- neither reads
the overlay's composition. So the testbed draws a picture from its first
frame, white beside black, before its own panels and under the library's:
the metrics panel, the drop-down and the veil are flushed after `on_overlay`,
which is where a push constant left standing shows. It is bound to `I`, as
the snippet binds it, so that a destroy and a create land mid-round and not
only at exit, and `stress.py`'s picture check reads the middle row through it
three times -- drawn, destroyed, drawn again.

**Measured since, and what is left.** This desk's `x11-vulkan`
passes the three checks (7.0% of the middle row white, 2026-11-21), and wine
does at `df55495` (the paragraph below). The laptops' and the Mac's rounds
pass with the check in them (2026-11-23/24). The KMS half has run too: the
LED's bit test at open and the `GETCRTC` kept off the motion path are on
every KMS open and pointer move, and `kms-vulkan-bare` passes (2026-11-23/24,
release, `gc` and `asan`, 185 checks each on schenker; `pi4` 59 at
`039518b`); the picture check itself is skipped there, KMS having no
`read-pixels`. **Still owed: the guests**, whose last rounds in the ledger
are 2026-11-05, before the check existed.

**The wine environments' failure did not reproduce, and no diagnosis was
made.** Measured (2026-11-21, commit `1deaf19`, nine runs between them: six on
`win64-vulkan`, three on `win64-d3d12`): both failed `a picture in the overlay
draws` and `and it is made again after its destroy`, while `I takes it away`
passed on a picture that was never drawn. At `df55495` on 2026-11-22 both pass
the three checks with a row 7.2% white: `win64-d3d12` 98 checks and
`win64-vulkan` 94, none failed. Whatever it was, it went with the commits
between them, and the draw is drawn -- so what is owed is not a diagnosis but
a second look if it comes back. One thing to know while reading a wine round:
`size_await()`'s 250 ms line (`no frame at the new size in 250 ms`) fires
about once in a run's worth of wine rounds on this desk -- once in the
`win64-vulkan` round here, and in five on 2026-11-02 -- and it fails the
round's log check when it does.

## Three shapes a UI is made of

`fnl_overlay_rect_round()`, `fnl_overlay_circle()` and
`fnl_overlay_gradient()`, built on all three renderers (2026-11-21). They are
the shape stream's box form -- `ShapeInstance::shape_kind` says which of
stroke, fill, round or gradient an instance is, and `shaders/shape.frag`
reads it -- so all three blend, all three are one draw with the strokes, and
none needs a pipeline of its own.

Both decisions the shapes left open are made (2026-11-21):

- **A disc past the coordinate bound is refused, not clamped.** The centre
  is a position and is still clamped like every other point; the radius is
  not, because a disc of a clamped radius is silently a different picture
  from the one asked for. Past `OVERLAY_COORD_MAX` it draws nothing and
  says so once, on `fnl_image_create()`'s terms.
- **A gradient on a degenerate box is the first colour.** The shader tests
  the span's *size* rather than dividing by a floor, since an angle past 90
  degrees runs the axis backwards and a signed test would mistake that for
  degenerate; zero span is `t = 0` at every pixel, so the box is one colour
  instead of a hard edge down its middle.

Seen (2026-11-21): the four forms drawn on this desk's X11 with Vulkan,
from the testbed's shapes panel (`--shapes`, or O). The corners are round
and the middle flat, the disc's edge is clean, and the gradient runs from
the first colour at the left to the second at the right -- so the angle's
direction and the corner radius's clamp are as the header says.

The coverage maths is measured as well as seen. The disc's edge, read off
a capture at surface scale 1.25 and again at 1.50, is partial coverage
spread across the whole 0-to-1 range on both -- 265 and 322 boundary
pixels, median 0.73 and 0.76 -- which is an analytic edge and neither a
staircase (all 0 or all 1) nor a blur (clustered at one value). The feather
is 0.5 physical pixels at any scale, so this is the ramp the shader
documents rather than one that happened to look right.

## A second window

Designed in DESIGN *A second window*. The header and the core's sort are in --
`FnlWindowDesc`, `fnl_window_create()` and the `Host` split -- and the backend
seam is split too: `host_open()` makes the connection and the one pump,
`window_open()` makes a window on it, and the pump serves every window the host
lists. The per-window frame loop is built: `fnl_start()` starts every window's
render thread, `fnl_pump()` stops the windows whose close was asked for and
starts any the run created, and `fnl_stop()` joins them all. **The door opens**
on every backend that is split: X11, Win32 and Cocoa whole, and Wayland's and
KMS's host halves. A second window opens on all five backends; compiled and
run on X11 and Win32 (release, 2026-11-23), on KMS in a round of its own, on
Wayland (2026-11-24) and on the Mac (2026-11-24). The
app was always the handle -- 128 of the 144 `FNL_API` prototypes take one --
so the work is the process state that is still in the app, not a handle to add.

- **The host, and the split.** *Written*: `FnlWindowDesc` beside
  `FnlInitDesc`, `fnl_window_create()` in the header and the core, and `Host`
  in `app.h` holding the backend vtable, the signal-installed flag, the window
  registry and the one renderer; `App` carries its host and the registry's next
  link. `fnl_init()` is unchanged in behaviour and creates the host and its
  first window. The backend seam is split: `host_open`/`host_close` and
  `window_open`/`window_close` are separate entries, `pump`, `wakeup` and
  `check_error` are the host's. X11's state is in two halves -- the event loop
  the host's, the connections and window the window's; Win32's is the module,
  the window and presentation classes and the raw-input thread the host's, the
  window's HWND and its state the window's; Cocoa's is the one NSApplication,
  the wakeup pipe and the application delegate the host's, the window and its
  view the window's. The per-window frame loop is in -- `fnl_start`/`fnl_pump`/
  `fnl_stop` reach every window on the host, a window's render thread started
  with the run or the pump that follows its creation. The renderer seam does
  not split. The GPU ranking is the host's: `fnl_init()` reads it and measures,
  and a second window reads the same file and opens on its first entry
  (`App::rank_read_only`, `RANK_KEEP`), so it neither measures nor writes the
  file and does not aim again; a calibration is refused while two windows are
  open, since its moves between GPUs are, and one under way ends when a second
  opens. The log level stays a process static in `error.c`, as the log sink
  is -- one program, one level.
- **The process state the table in DESIGN names**, each row a site to move:
  the signal app, `this_thread` and `app_renderer_state_ours()` (done: the
  render thread's window is the thread-local `render_window`, and the accessor
  compares by identity), Win32's raw-input registration and the two pointer
  calls that are the desktop's (`ShowCursor`, `ClipCursor`), Cocoa's four
  file-scope variables (`iso_swap`, `pump_waiting` still file-scope and main-
  thread-only), and the Metal and Vulkan tables that are one a process
  today.
- **Wayland's second window is built** (2026-11-24; DESIGN *A second window*).
  The host owns the one `wl_display`, its registry and globals, the seat and
  its devices, the layout, the outputs and the event loop; a window owns its
  surface, xdg objects, viewports, frames and its own set of outputs. The
  host's listeners now take the `Host`, not a window, and the seat's events
  are routed to the window their surface names (the keyboard's enter, the
  pointer's enter, and the text input's), with the clipboard and the input
  method — the seat's — delivered to the focused window. A window is answered
  by the host's own open list until `fnl_window_create()` links it, since its
  keyboard enter arrives inside `window_open()`'s wait for its configure.
  Driven by hand on GNOME Shell 50.1 (release and asan): F10 shows and hides
  it four times over, focus moving between the two windows and each key
  echoed from the one that received it; clean under the Vulkan validation
  layer. **Owed: its round** — `two_window_wayland()` is in the harness
  (`stress.py`, `expected()` allows the testbed's per-window focus lines) and
  has not been run.
- **KMS's second window is built** (2026-11-23; DESIGN *On KMS a window is an
  output*). The host owns the card and its outputs, the event loop, the one
  input set and the layout; a window is a selection of one output and its own
  scanout and cursor plane, and a second window opens on a free output at its
  preferred mode. Input is routed by the pointer over the windows laid left to
  right (pointer-follows-focus; the focused window has the keyboard), and the
  output box lists the free outputs plus this window's. Since a second window
  shares the host's Vulkan device and Mesa's display WSI faults anv on a
  display surface beside a second swapchain, a host of more than one window
  draws by the copy path and a window already showing lets its display go.
  **Proven on schenker's CachyOS** (Intel HD 630, eDP-1 + HDMI-A-1), 2026-11-23,
  `release`: the single window on the display path unaffected; two windows on
  the two outputs (`--second-window`) drawing both, the cursor on the focused
  first window, at `--kms` with the Intel copy path and with `FNL_GPU=nvidia`
  (proprietary, its surfaceless copy path); SIGTERM/SIGINT now closes every
  window, not only the one that installed the handler. **Many monitors** are
  the same model, made N-ary: a third and a fourth window open on the outputs
  no other holds, tiled left to right for the pointer; `outputs_rescan()` now
  moves *every* window whose output went to one still held by none (or leaves
  it drawing nothing) and sets the mode again for every window whose output
  came back. Mirroring and joining are the caller's (N windows on N outputs,
  the program drawing the same or a shared canvas), not the model's. **The
  harness's two-window check on KMS is built and passes** on schenker's
  `:nouveau` (`kms-vulkan-bare`, release, `--capture`): both outputs lit by
  the program (HDMI-A-1 and eDP-1 named in the echo) and each drawing its own
  window, from per-output `kms_grab.py` captures (1920x1200 and 1920x1080);
  five checks, none failed. On `:nvidia` since (2026-11-23) the check also
  takes the pointer across: pushed left into the first window's edge and then
  right past its width, the seat goes to HDMI-A-1 and comes back to eDP-1
  with the pointer, and it reads the log's line for the cursor plane the
  pointer went on -- eight checks, none failed, the round 185 with none, a
  second round after. `:nouveau` the same day gave the same figures. Owed: proving more than two outputs -- this lab has two
  on schenker and no spare monitor, and forcing a connector's status on its
  i915 sent no uevent, so the N-ary hotplug path is reasoned and built but
  not driven; a display path that is not one window's -- a DRM lease per
  output, or a per-window device, closes Mesa's sharing for good; and
  **outputs on a second card**, which one-card-a-process (`SET_MASTER`) does
  not take -- a multi-card desktop wants a model of its own and is owed.
- **Cocoa's second window is built** (2026-11-24; DESIGN *A second window*).
  The host's half is one NSApplication, one wakeup pipe and one delegate
  asking the frontmost; each window has its own NSWindow, view, layer,
  delegate, press and render thread. The pump hands each event to the window
  AppKit names in it, a key-up monitor acts only on its own window's, a
  window placed with `x_pt`/`y_pt` opens there, Metal's shader cache is the
  device's, and one Vulkan instance is made at a time process-wide (MoltenVK
  refused concurrent first instances — measured). Driven on the Mac over ssh
  (`macos-metal-native`, release): the single-window round 98 checks and the
  two-window round 19, none failed — F10 show/hide, F12's views, the second
  window drawing, taking the pointer and giving it back, log clean.
  Found on the way: AppKit's own drag was handed a press it no longer owned
  (`performWindowDragWithEvent:` retaining a freed `NSEvent`, a
  `EXC_BAD_ACCESS` in `objc_retain`), now owned by the window until release.
- **The window position is wired** for X11, Win32 and Cocoa
  (`fnl_window_create()`'s `x_pt`/`y_pt`). Wayland (the compositor places)
  and KMS (the window is the output) ignore it, as their comments say.
- **What is measured where.** *Done on all five backends*: `stress.py` opens a
  second window (the testbed's `--second-window`, relaunched after the
  single-window round) and checks both windows open, are distinct and focused in
  turn, that the second window draws the scene (its clock on with F2, a row of
  its content read some frames apart moves), and that it takes the pointer on a
  content click and gives it on F9 while the first is open -- what no
  single-window round can say. The capture is driven on the second window alone:
  the two open nearly on top of each other under wine, and a covered window parks
  and the driver cannot find it to click. It passes on `x11-vulkan`,
  `win64-vulkan` and `win64-d3d12` (release, 2026-11-23), 12 checks a round, and
  on `macos-metal-native` (release, 2026-11-24, 19 checks). Wayland has a check
  of its own (`two_window_wayland()`): a compositor lists no windows and a
  client is told neither where its windows are nor which AppKit-shaped handle a
  key went to, so it reads the testbed's per-window focus lines, the show and
  hide lines, and the echo's area (`wayland-vulkan`, release, 2026-11-24, 9
  checks).
  `find_window()` picks between a process's several windows now -- `index=` into
  them, and `find_windows()` naming every one (an empty title) -- so the check
  sees both rather than whichever tree walk reached first. KMS has a check of
  its own (`two_window_kms()`): a window is an output, so each is captured by
  `kms_grab.py --output` from the scanout, and the pointer crosses from one
  output to the other with the seat (`kms-vulkan-bare@schenker`, release and
  `gc`, 2026-11-23). Still owed: the order `find_windows()` returns is the
  tree's, stable per round but not across a restack. The harness's own
  `_NET_WM_PID` note stands.
- **The scene is one per window.** Each renderer's `testbed/scene_*.c` keys its
  pipelines by the `FnlApp` the callbacks name, so a host's second window draws
  its own rather than overwriting the first's. `window_start()` enters
  each window's renderer before its `on_gpu_created` (`thread_enter`, a
  no-op for Vulkan and Direct3D 12). Metal's compiled-stage cache was one
  a process and is now the device's, since a second window compiles for its
  own device on its own render thread.
- **The testbed shows the three ways two windows can relate** (2026-11-24):
  F10 shows and hides the second window, F12 cycles **own** (the second draws
  the scene shader's other scene -- the demos' turning cube, content of its
  own entirely -- where it starts, so a second window is not a copy of the
  first), **mirror** (both draw the same pattern) and **join** (one canvas
  split across the two, the first the left half and the second the right, the
  star on the seam and the sweep crossing it). The second window's own close
  button hides it, as F10 does, rather than leaving it on screen stopped --
  `fnl_is_closing()` is how the loop learns of a close the window's stopped
  render thread may never drain. The library has no hand in the views: the
  view rides the pinned `ScenePush`
  as `canvas`/`origin`/`tint`/`time_bias`/`content`, read by
  `testbed_scene_view()` on each window's own render thread, so what a window
  draws is that window's app. The key arrives on the render thread and the
  window is opened and freed on the main thread, so the testbed writes its own
  `fnl_start`/`fnl_pump`/`fnl_stop` loop (`run()`) that spends the key between
  two turns. It also closed the harness gap `--second-window` left open:
  F10/F12 are driven by the two-window round, on every backend that opens one.
- **The DESIGN table's pointer row is closed**: Win32's `ShowCursor` count and
  `ClipCursor` confinement are now the host's -- a host count of windows that
  have confined the pointer, so the first hides it and the last gives it back,
  and a window releasing while another still holds it leaves that one's clip on
  the pointer rather than clearing it (which was the fight two captures had).
  The round exercises it: the second window takes the pointer on a content click
  and gives it on F9 with the first open beside it.
- **The two-window review's fixes** (2026-11-24): a failed
  `fnl_window_create()` no longer takes KMS's console and display path with
  it (the last-window test is the registry's, and the copy path the attempt
  forced is given back); `Host::window_count` is atomic, since render threads
  read it, and `Host::opening` refuses a GPU move while a window is being
  built; a GPU hotplug reaches every window (X11 and KMS, as Wayland already
  did); the signal handlers follow the host to a window that outlives the one
  that installed them; Win32's `WM_QUIT` ends every window's run; a second
  window reads the host's ranking and opens on its first entry, and cannot
  calibrate; Vulkan's `vkDeviceWaitIdle()` takes the host queue's lock, the
  pipeline cache's loaded figures and temporary file are the window's, and the
  GPU listers are serialised. Owed: a second Vulkan window lists no entry of
  its own renderer until a rebuild of its own, so after the window that chose
  the device closes it cannot yet be moved to another GPU of the renderer
  drawing, nor take the ranking's calibration over from the window that went;
  a survivor moved to another renderer leaves `Host::renderer` naming the
  first window's choice, so a window created afterwards is built on that one;
  a window created while the only window is moving to another GPU can be
  built on the renderer being rebuilt, since `opening` is checked before the
  move commits and not again -- a host-level moving flag, stored before
  `opening` is loaded on the move's side, is the shape of the fix; KMS's copy
  path is one-way, so closing the second window leaves the survivor on it
  though it could take the display again (the remake wants the closing
  window's unlink first); each Vulkan window keeps its own `VkPipelineCache`,
  so two windows write the one cache file in turn and the last save loses
  the other's newly compiled pipelines -- one pipeline cache for the host's
  device, saved once and reused by every window, is the shape of the fix (and
  stops both compiling the same pipelines); and Win32's size hold is the pump
  thread's, so a border drag on one window holds every window's input for up
  to `SIZE_AWAIT_MS`.
- **Order.** The header and DESIGN, the core split with `fnl_init` unchanged,
  the backend seam's split and the per-window frame loop are done, and the seam
  is split on all five backends: X11, Win32 and Cocoa whole; Wayland (the one
  display, globals, seat and event loop the host's) and KMS (the card, outputs,
  inputs and event loop the host's) with their host halves apart. A second
  window opens on all five. The window position is wired for X11, Win32 and
  Cocoa, which KMS has no use for and Wayland cannot honour. Both windows draw
  the scene, and the two-window round passes on X11 and Win32, drawn with
  Vulkan and Direct3D 12 (release, 2026-11-23), on KMS, which has a
  check of its own (release and `gc`, 2026-11-23), on Wayland
  (`two_window_wayland()`, release, 2026-11-24) and on the Mac
  (`macos-metal-native`, release, 2026-11-24).
