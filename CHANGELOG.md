# Changelog

What each release changed for a program that uses Fenestal. `DESIGN.md` is
why; `fenestal.h` is the API, and its *stability* block says what is
promised: before 1.0, nothing between minor releases.

## 0.9.0

- Cocoa: a window closed and another opened in the same process no longer
  crashes on the exit event AppKit still owed the old window's tracking
  area; the area is removed before the window goes, and the view's
  pointer handlers no longer read an App the view has lost.
- X11 with Vulkan, where the device can present one to one from its
  corner (`VK_KHR_swapchain_maintenance1`, or the EXT name), a resize
  keeps the swapchain and at rest the images are the window's size
  again.
- Wayland with OpenGL: the EGL window is kept larger through a resize,
  as Vulkan's images are, and the viewport shows its top-left.
- Licensed: the code under MIT, the documentation under CC BY 4.0, and
  `demos/` and `examples/` under CC0, each source file tagged with its
  `SPDX-License-Identifier` (`LICENSE`); the material Fenestal carries and
  did not write is listed in `THIRD_PARTY_NOTICES.md`, which goes with
  every copy.
- The baked font is named: Fenestal Glyphs Sans and Fenestal Glyphs Mono,
  renamed modified versions of Ubuntu Sans and Ubuntu Sans Mono, as the
  text `fnl_font_notice()` returns now says.
- Wayland: a present the driver refuses after the compositor has said it
  was shown is counted as presented, as on X11, so its index is not drawn
  again.
- `fnl_frame_samples()`: a frame whose present the display refused is
  `presented` false under its own index, where it took the index of the
  frame before and said presented; `FnlFrameStats::presents` no longer
  counts it.
- X11 with Vulkan on NVIDIA's driver: a present the driver calls out of date
  and shows all the same is counted as presented, so the next frame no
  longer draws under its index and `fnl_frame_shown()` stamps it once.
- X11: a frame the server answers with a stamp from before its present --
  XWayland's flip for a window resting between frames -- is stamped at the
  first refresh after the present with vsync on, where it was not stamped,
  so `fnl_frame_shown()` has those frames too.
- X11 with Vulkan: the Present completions that say when a frame was shown
  are matched to their frames by the presenter's serial, so the shown stamps
  (`fnl_frame_shown()`) and the resize hold no longer drift some five frames
  behind once a swapchain has been made again.
- A renderer named in `FnlInitDesc::renderer` is the only one the GPU
  list offers, as the header says, even with `renderers` set beside it:
  `renderers` applies where `renderer` is `FNL_RENDERER_DEFAULT`.
- A GPU named by `gpu_name`, `gpu_index` or `FNL_GPU` stands through a
  GPU calibration: a name brings the window back to the entry it was on
  when the calibration began, and an index to that entry of the ranked
  list, where both used to land on the ranked first. Moved to another
  display, a window on a GPU named by name stays there.
- A calibration no longer writes the scene-motion setting: it runs the
  scene's clock beside it, so `fnl_scene_motion()` reads what the caller
  or F2 last set, and a change made during a calibration holds after it.
- `fnl_set_metrics_key()` ignores a value that is not an `FnlKey`,
  leaving the key as it was.
- KMS: a second GPU plugged in while the list waits for the first is
  waited for too, and one plugged in after the wait gets a list of its
  own; the second was taken up unwaited, before udev had readied it.
- The GPU ranking's file is written under a name of the process's own
  before it replaces the kept one, so two instances of a program no
  longer write over each other's half-written file.
- X11: a captured pointer's grab is lifted when the window loses the
  focus and taken again when it comes back, as Win32's clip and macOS's
  detachment are; the capture and the reticle stand meanwhile.
- Wayland: `FNL_EVENT_RELATIVE` carries the unaccelerated movement, as
  Win32's and KMS's do, where it carried the desktop's accelerated one;
  X11 and macOS stay accelerated (`FnlRelativeEvent`).
- Wayland: a keyboard that goes with the seat's capability posts
  `FNL_EVENT_FOCUS` with `focused` false, as its leave would have.
- Wayland and KMS: in text entry a held key `FnlKey` does not name -- a
  yen key -- types again as it repeats, as on X11, Win32 and macOS;
  modifiers and locks still do not.
- Wayland and KMS: a key name the keymap defines twice takes the later
  code, unless the later came in by augment (`|`), as libxkbcommon does.
- The overlay calls, `fnl_claim()` and `fnl_titlebar_claim()`, and the
  four scene getters (`fnl_vulkan_scene_cmd()`, `fnl_gl_scene_framebuffer()`,
  `fnl_d3d12_scene()`, `fnl_metal_scene()`) refuse a thread other than the
  one running the callback, as the header says, where a call from one
  while a frame was recorded went through. `fnl_shutdown()` and
  `fnl_run()` called from the first `on_gpu_created` or the last
  `on_gpu_lost` are refused with a line, and `fnl_init()`'s refusal of a
  NULL or undersized argument says why through `fnl_last_error()`.
- The clipboard's lines end in LF on every system: Windows' CRLF becomes
  LF in a paste, and an LF without its CR gains one in a copy there.
- Win32: an input method's candidate list is put beside the caret that
  `fnl_set_text_rect_pt()` names, as on Wayland and macOS, rather than
  wherever the system left it.
- macOS: a composition still up when the window loses focus is discarded,
  as on X11 and Wayland, and the input method is told where the marked
  text is (`markedRange`). `fnl_input_latency_ms()` is measured from key,
  mouse and wheel events alone, as on Windows.
- `fnl_vulkan_scene_rect()`: the rectangle the viewport and scissor are
  set to for `on_record`, as `fnl_d3d12_scene()`'s `rect` and
  `fnl_metal_scene()`'s are. A clear of the scene's own wants it:
  `vkCmdClearAttachments` takes no viewport, and drawn straight into the
  window the scene starts past the titlebar. `examples/scene_vulkan.c`
  clears that rectangle, where it cleared one shifted up by the titlebar.
- The frame read back for a move's dissolve, and the texture it is drawn
  from on the device after, are let go once the dissolve is over, where
  they were kept for the device's life: some 33 MB at 4K.
- OpenGL: a clip distance a caller's scene left enabled no longer clips
  the library's overlay.
- Vulkan on a Mac skips a frame the window server would not show, as
  Metal and OpenGL there do: a covered window no longer draws for nobody.
- Metal: `on_gpu_lost` follows the end of the lost device's frames in
  flight, as the header says, where a loss skipped the wait.
- The testbed's benchmark mode writes a record's fractions to nine
  significant digits, where it wrote six; exits with the worst status of
  every series it ran, and 1 where a record could not be written; refuses
  `--footprint-log` beside `--bench`; says what is wrong with a replayed
  record's `options`; and leaves a replayed option at its default unset,
  as the run it replays had it.
- Wayland: requests made while the display's socket is full -- a title,
  a size floor -- go out as soon as it has room, where they waited for
  the next event to arrive.
- X11: where the server will not intern the clipboard's stamp property,
  copy and paste go at `CurrentTime` with one line, where every paste ran
  to its deadline and answered empty.
- KMS: text entry turned off with a dead key's accent pending drops the
  accent, as X11 and Wayland do, so the next field does not start with it.
- Win32: under `fnl_titlebar_custom()` the bar's drag strip wears the
  caller's `fnl_set_cursor()` shape, as on the other backends, where
  Windows drew its arrow.
- A titlebar press held when the window loses the focus is let go, on
  every backend: whatever took the focus may take the release too, and
  the next motion started a move the button no longer held.
- `FNL_EVENT_RELATIVE` is held back during a GPU calibration, as the
  pointer's position is; a capture taken before one began goes on.
- `fenestal-pace` and `fenestal-lifecycle` are one program, `fenestal-dev`,
  with a mode each: `fenestal-dev pace 60 12` and `fenestal-dev lifecycle
  20` are what the two were run as before.
- The programs to start from are under `demos/` -- `hello`, `motion`,
  `motion2`, `texture`, `texture2` and `texture3`, each with its scenes,
  its shaders and a `CMakeLists.txt` of its own, built in every tree on
  the static and the shared library. They were the `fenestal-demo`
  repository, now retired. `fenestal-minimal` and `examples/minimal/` are
  gone, and `tools/drive/minimal.py` is folded into the stress round. The
  testbed is `testbed/`, and the API reference's snippets are
  `examples/*.c`. The testbed drains the event queue in `on_record` as
  well as `on_overlay`, as a program whose input moves its scene does.
- The titlebar's output box is on every backend, not only KMS: it names
  the monitor the window is on, follows it when the window is dragged or
  moved by the window server or by `fnl_request_output()`, and a pick
  moves the window there, keeping its size, place and state. Under a
  window server it gives way where the window is narrower than the
  controls with it (734 points), so the size floor stays 625. On Wayland
  it is dim: a client there places nothing. `fnl_output_count()`,
  `fnl_output_name_at()` and `fnl_request_output()` follow: they list a
  window server's monitors and move the window to one -- on Wayland only
  a fullscreen window, and on macOS none that is fullscreen.
- The GPU calibration takes about half as long: an entry is measured once
  its figure holds still, at least a second and at most the 3.5 s every
  entry took before; the likeliest to win is measured first, and one that
  settles under half of the best is not sampled. On this desk three
  entries took 4 to 5 s where they took 11, the same entry first.
- The GPU ranking measures a monitor once the window has stayed on it, on
  one output and unmoved, for a second: a window carried or stretched
  across monitors no longer calibrates the one it passes over, nor moves
  to another GPU mid-drag. And on Wayland a display is the followed
  output's name and its own mode: keyed by the compositor's bounds, which
  are another output's while a window straddles two, one monitor was
  several displays and a window resized across onto a second monitor
  calibrated twice. Wayland rankings kept so far are under the old names,
  and each monitor is measured once more.
- `fnl_antialias()` reports the mode the GPU drawing now can draw: after a
  move to a GPU without FSAA it read FSAA though none was drawn. The mode
  asked for is kept, so a move back to a GPU with FSAA draws it again, and
  each change posts `FNL_EVENT_ANTIALIAS`.
- The titlebar's `FSAA` and `MLAA` boxes show the antialiasing mode in
  force, whoever set it: a mode set with `fnl_set_antialias()` was drawn
  unticked, and a click on that box turned it on again rather than off.
- A metrics panel of the library's own, on every renderer: the frame
  rate, what a frame costs on the GPU and the CPU, the rate that allows,
  input latency, and this process's CPU, memory, GPU memory and share of
  the GPU, drawn over the caller's overlay at the presentable area's top
  right. `F5` shows and hides it, a binding like `F2`'s;
  `fnl_set_metrics_key()` moves it to another key or to none, and
  `fnl_set_metrics_panel()` shows it from code. A program that used `F5`
  for itself unbinds it. The testbed's own panel is now this one.
- The metrics panel shown no longer keeps the frame's figures measured
  at rest: a still window draws a frame for it only where its process
  figures move, at most twice a second rather than five times, and that
  frame is left out of `fnl_frame_gpu_ms()` and `fnl_frame_cpu_ms()`, as
  is any frame drawn at rest only to show them, so with the panel up they
  and `fnl_fps()` hold the last drawn frames' figures. Call
  `fnl_set_cost_sampling()` to keep them live. Its process figures are
  sampled after the present rather than ahead of the frame, so the
  sample (NVML's query is half a millisecond) no longer falls between the
  late latch's hold and the frame it was timed for. Its GPU row is the
  GPU's time on the window's frames over each half-second sample rather
  than the frame's GPU time by the frame rate, so a still window no
  longer reads as busy, and a CPU reading the system refuses no longer
  makes the next one read 99999.9 %.
- OpenGL on X11 keeps up with a drag of the border under a window
  manager: the frame at a new size is shown before the manager is told
  it is there, where it was told as the frame was handed over, and the
  next size then waited for the frame (48 to 49 fps through a drag on
  this desk, against 36 to 40).
- The `--frame-times` line (`FnlInitDesc::frame_times`) reports `other`
  after `rebuild`: the frame's time outside every part it names, so the
  parts and it add up to the frame.
- OpenGL on macOS costs less of a core: the present polls GL's fence
  where Apple's wait on it spun, some 4 ms of CPU a frame.
- On macOS a fullscreen asked for during AppKit's fullscreen animation
  is applied when the animation ends, where it was dropped before.
- OpenGL on macOS: the `macos` tree draws with OpenGL 4.1 through CGL,
  after Metal and Vulkan in the order (`FNL_RENDERER=gl`), its frames
  copied into the window's Metal layer; a program there links OpenGL
  and IOSurface beside AppKit, QuartzCore and Metal.
- On Windows a pointer capture gives a program's own raw mouse back when
  it ends: registered for the capture on the library's window, it was
  never handed back, and the program had no raw mouse for the rest of
  its life.
- Under Direct3D 12 a device loss waits for the frames in flight before
  `on_gpu_lost` and before the renderer's own objects go: a device only
  said lost still runs them, and its objects were freed under the queue
  (the debug layer raised it as an exception nothing caught).
- A GPU ranking file from before a GPU's ways were one entry no longer
  lists that time's rows of the second way (`vulkan-D3D11|...`) as ranks
  of their own: they were kept, printed and never opened on.
- The GPU ranking's figures kept for an output whose rate was not yet
  known (`@0` in the file) are that output's once its rate is known,
  and are no longer measured again under the rate's name.
- Under wine, Direct3D 12 lists only the adapters on which it builds its
  pipeline states, asked of each with one built at listing: under wine
  10.0's own vkd3d, which makes a device and runs no DXIL, it lists none,
  so the GPU box offers no Direct3D 12 entry that comes back and the
  ranking measures none. Windows is asked nothing more.
- `libfenestal.dll` from an optimised build exports the API again: every
  export of the release DLL pointed at address 0 and none of the code
  was in it. The programs and `fenestal::static` were not affected.
- OpenGL draws with a 4.1 core context where the device makes no 4.5,
  which was refused before. `fnl_gl_version()` says which, 45 or 41; under
  41 a scene draws with 4.1 core calls and GLSL 4.10 and maps its own
  depth into GL's range (`fnl_gl.h`; spirv-cross's `--fixup-clipspace`,
  in `FNL_GL41_FLAGS`). The examples' OpenGL scenes and the snippet are
  written in the 4.1 subset, which 4.5 runs too.
- `./build.sh dist` packs `build/fenestal.zip` and
  `build/fenestal-commented.zip` alone, with no build, from the working
  tree as it stands: untracked files in, deleted ones out, each file's
  mode its own (`tools/mkdist.py --worktree`).
- `FNL_VK_VALIDATION=1` turns on Khronos' validation layer in the
  library's Vulkan instance alone, where the loader's
  `VK_LOADER_LAYERS_ENABLE` puts it in every instance of the process,
  Zink's included; a loader without the layer makes the instance
  without it, with a warning.
- `FnlFrameSample::gpu_ms`: each drawn frame's GPU time by the
  renderer's timestamps, in what was the struct's padding, so it keeps
  its size. A row the GPU timed waits in the ring for its time -- eight
  frames or a quarter of a second at most -- and holds back the rows
  after it, so they still come in the order drawn; when the window's
  loop ends every row is handed over.
- `fnl_frame_shown()` on Win32, by DXGI's frame statistics, for Direct3D
  12 and for Vulkan presenting through DXGI; on Metal, by each drawable's
  presented time, and for Vulkan on MoltenVK by VK_GOOGLE_display_timing,
  which carries the same; and on KMS for Vulkan's own display path, by the
  driver's VK_EXT_present_timing where it has it (Mesa's Intel driver,
  26.2), its clock calibrated onto CLOCK_MONOTONIC.
- `fnl_output_refresh_mhz()` on Win32, from the display configuration's
  signal (60.020 Hz on both laptops' panels), and the frame loop paces on
  it there until it has measured the period, as on the other backends.
- Direct3D 12's swapchain allows tearing only when vsync is off: with
  vsync on the flag cost a refresh on a GTX 960M, 53 ms from latch to
  screen against 36.
- Direct3D 12 with vsync on keeps one frame queued, not two: the wait a
  frame skips is taken back by waiting for it. A refresh less from latch
  to screen on every GPU measured, 19.8 ms against 36.4 on a GTX 960M.
- Vulkan through DXGI on Win32 tears with vsync off where DXGI offers
  tearing, and no longer waits for the display: 871 fps on a GTX 960M,
  where it held 60.
- A window at rest has buffers its own size: half a second after its
  size last changed they are made so, once, where a drag had them larger.
  On Win32 that is what Windows flips onto a plane rather than composes,
  a refresh less: 20 ms from latch to screen against 37 on an HD 630. A
  fullscreen window on Win32 is no longer given buffers taller than
  itself.
- Vulkan on Win32 goes through DXGI by default wherever the GPU can share
  images with Direct3D 11, not only where a swapchain rebuild is slow:
  NVIDIA's own swapchains were 36 and 73 ms from latch to screen where
  DXGI is 20. `FNL_VK_DXGI=0` keeps the device's own. Never under wine,
  whose DXGI cannot take the shared images.
- OpenGL on KMS, a Debug build: NVIDIA's refusal of the output's buffer,
  which the library asks for and then copies each frame instead, is no
  longer logged as a GL error, and the copy's readback buffer is made in
  client memory, where NVIDIA's GL had moved it and warned.
- Win32: a window being resized keeps the display's rate where its
  frames are composed a refresh late: Vulkan through DXGI has three
  buffers, and Direct3D 12 queues two frames while the window is sized
  and one at rest. On an HD 530 and a GTX 960M a drag drew 30 to 47 fps
  and draws 55 to 60; from latch to screen at rest is unchanged.
- The GPU ranking ranks a GPU and leaves how its frames reach the window
  to the renderer: a Vulkan GPU listed both ways on Win32 is one entry
  measured once, and a program that ranks stays on the way Vulkan takes
  by itself rather than being moved onto the GPU's own swapchain. A rank
  file's figures for the two ways apart are dropped.
- The GPU list's entries name no way through another API where their
  renderer gives none: a Direct3D 12 entry read uncleared memory there,
  which `fnl_gpu_via_at()` returned and which made the ranking calibrate
  at every start of a Debug build.
- Direct3D 12 on Win32 measures a window at rest and, where DWM composes
  it, presents through a Direct3D 11 swapchain, as Vulkan does through
  DXGI: an HD 530, whose WDDM 2.1 driver composes a Direct3D 12 window,
  goes from 36.9 ms from latch to screen to 20.2. `FNL_D3D12_DXGI11=1` or
  `0` decides it.
- `fnl_frame_shown()` drops a stamp from before its frame was presented,
  which is no time it was shown: on X11 a present the server carried out
  at once (vsync off, Mesa's `vblank_mode=0`) completed with the vertical
  blank before it, and WARP in a Windows 11 guest reported frames shown
  minutes before they were drawn.
- Vulkan on Windows presents through a DXGI swapchain, its frames copied
  by Direct3D 11, where a swapchain of Vulkan's own is slow to resize --
  Intel's driver, 33 to 44 fps through a drag, 60 this way -- and a GPU
  that can share images with Direct3D 11 is listed a second time for it.
  `fnl_gpu_via_at()` and `fnl_renderer_via()` say "D3D11" for such an
  entry and window; the ranking measures the two entries apart.
  `FNL_VK_DXGI=1` or `0` decides it for a window.
- Wayland windows drawn with Vulkan keep their swapchain through a drag,
  its images larger than the window and its part shown through the
  viewport's source rectangle; Direct3D 12's buffers are made larger than
  the window the same way. A resize costs neither a new swapchain nor
  `ResizeBuffers` until the window outgrows them.
- On X11 a frame reaches the screen a refresh after its latch: the frame
  loop waits, ahead of the latch, until the X server says the frame
  before is on the screen (the Present extension), and lets one more wait
  where frames miss. 16.4 to 16.5 ms from latch to screen at the same
  rate, where Vulkan took 54 ms, OpenGL 21 and NVIDIA's own driver 36.
- `fnl_frame_shown()` on X11, from the Present extension's completions:
  when each frame the graphics API presented reached the screen, by the
  server's clock.
- `FnlEvent::input_age`: how long before the event's `time` the oldest
  input it carries happened, by the device's own stamp where the library
  shares its clock; 0 for what no hand did. In the padding after `type`,
  so `FnlEvent` keeps its size. The benchmark record's `input.to_shown_ms`
  is filled from it.
- On X11 with no window manager, the window takes one size at a time: a
  size asked for, by the caller or by a drag of the border, waits until a
  frame at the last one has been presented, or a quarter of a second, and
  the latest goes then. NVIDIA's own driver had most sizes of a fast drag
  out of date before they drew.
- A drag on X11 and on Windows draws at the display rate again. The frame
  that answers a size is still shown before the size is acked, but the
  wait is made on a thread of the renderer's rather than the render
  thread's: a drag of the window's corner on X11 drew 59 to 60 fps where
  it drew 45 to 48, and Direct3D 12 on Windows 49 to 56 where it drew 15
  to 19. How fast the window follows the drag is unchanged.
- A sample two refreshes long is no longer taken as the display period
  where the output reports its refresh rate; one could pace a whole drag
  at half the rate.
- On Windows, the cursor comes back where the reticle is when a capture
  ends, as it does on the other backends, rather than where the hidden
  cursor had drifted -- which could be the titlebar or a resize edge.
- A display period learnt from a swap that did not block at first is
  learnt again once it does: OpenGL on Xorg (Mesa, Intel) had stood on
  the first half second's 1.06 ms for the window's life, spaced by the
  loop's own clock and never late-latched; it is now 16.6 ms and paced by
  the swap. A line says so when it happens.
- `fnl_frame_shown()`: when each frame reached the screen, by the
  display's own stamp, joined to the frame by `FnlFrame::index` -- KMS's
  page flips (the copy path and OpenGL's) and Wayland's
  `wp_presentation`, which the Wayland backend now binds.
- `fnl_frame_samples()`: each drawn frame's phases (`FnlFrameSample`),
  the last 1024 kept while `fnl_set_frame_stats()` is on, drained by the
  caller with a stride so a shorter struct reads its prefix.
- `fnl_output_refresh_mhz()`: the refresh rate of the output the window
  is on, as the window server says it, 0 where it says nothing (Win32).
- `fnl_set_frame_stats()` and `fnl_frame_stats()`: the frame loop's time
  as running totals in `FnlFrameStats` -- pace, fence, acquire, record,
  submit, present, rebuild and draw, the GPU's time, idle frames, display
  cycles and misses -- for a caller to read twice and subtract, where
  `--frame-times` is a line of text. `fnl_set_scene_motion()` and
  `fnl_scene_motion()` are F2's scene clock as calls. `--frame-times` was
  said to be once a second; it is each half second, as it was.
- A renderer named by `FNL_RENDERER` or `FnlInitDesc::renderer` is the one
  drawn with under a GPU ranking too: the ranking's move to its first
  entry counts that renderer's entries alone, where it moved the window to
  the first entry of any renderer -- `FNL_RENDERER=gl` opened OpenGL and
  then drew with Vulkan.
- `fnl_claim()` claims a rectangle anywhere in the window, from
  `on_overlay` as `fnl_titlebar_claim()` does: a press there is the
  caller's event and does not capture the pointer, so a panel can be
  drawn over the scene and worked with the mouse. One list serves both
  calls, and it holds 64 rectangles a frame, where the titlebar's held 16.
- `FnlVulkanGpu` ends in `get_instance_proc_addr`, the loader the library
  opened (MoltenVK's own on a Mac): a scene asks it for its entries and
  links no loader, so the program still starts, on another renderer,
  where there is none. The examples do, and no longer import
  `vulkan-1.dll` or link `libvulkan.so.1`; the Vulkan headers come with
  `fenestal::static`, and the examples no longer link `Vulkan::Vulkan`.
- Late latching holds on a swapchain that throttles where nothing invites
  a frame (X11, Win32, KMS, Cocoa): a window learning its display's
  period is no longer spaced by the loop meanwhile, which starved the
  measurement -- on amdgpu's KMS display path the period read 2 ms and
  the hold 0, and on an Intel panel's no period was learnt at all. A
  software device, whose present is never measured, is spaced as before.
- A runtime that is missing is said once, by its own name: `no Vulkan
  loader: libvulkan.so.1: ...`, where it was said twice and named the
  fallback `libvulkan.so`; and a loader whose drivers will not load says
  `no Vulkan driver would load`, not that none is installed.
- OpenGL on KMS draws onto the output's own buffers: the frame is blitted
  on the GPU into the backend's scanout buffer and flipped on, where it
  was read back and copied by the CPU -- on an HD 630 flat out 796 frames
  a second against 102. A GPU that will not take the buffer, NVIDIA's EGL
  beside an Intel panel among them, reads back as before, into a pack
  buffer, and the log says which way and why.
- `fnl_frame_cpu_ms()` is the render thread's CPU time on a frame, from
  the end of its waits to the return of the present: the submit, OpenGL's
  swap and KMS's copy path's readback and copy are in it, and a wait that
  blocks is not. It was the wall clock over recording and submitting,
  which left the copy path's present out -- on KMS at 4K the GPU ranking
  opened OpenGL, which draws at 30 frames a second there, before Vulkan,
  which draws at 60. Windows under wine, whose thread cycle counter
  fails, keeps the wall clock. Both figures start again on another
  device. The GPU ranking's file has a version; a file kept before is
  measured again.
- `fnl_frame_gpu_ms()` holds KMS's copy path's readback on OpenGL as it
  did on Vulkan: OpenGL's copy path reads back through a pack buffer
  before the frame's end stamp, where it read into client memory after
  it, a wait inside the present that neither figure counted. A snapshot's
  copy, once a move, is left out on every renderer. `FNL_COST_PARTS=1`
  beside `--frame-times` logs what each of the two cost figures is made
  of.
- KMS: a move to another kernel driver refused because the process holds
  NVIDIA's driver names the NVIDIA libraries still loaded beside the node
  (`/dev/nvidiactl (mapped: libnvidia-ml.so...)`), or says none is; and
  when no further move is offered the GPU list is logged again, since
  each entry behind a move entry has a new number. The testbed's F5
  panel, whose NVML query on NVIDIA's OpenGL was such a holder, shuts
  NVML down at the end of each device.
- The GPU ranking on a window server keys a display by the output the
  window follows and its mode, `HDMI-A-1 3840x2160@30`, as on KMS: on
  Wayland it had the model and every output a straddling window touched
  in the name, so each crossing between monitors measured every entry
  again, and no rate (`@0`); on X11 the start read no rate yet. Wayland
  rankings kept before are measured again once.
- On X11, Win32 and KMS a move to another GPU learns the late-latch hold
  again from none: the hold the GPU before had learnt stood for good on a
  software rasteriser, which measures nothing that could cut it, and its
  idle gap each frame kept the CPU's clock down -- llvmpipe at 10 fps
  after NVIDIA's OpenGL where it draws 30, and the GPU ranking's software
  figures as low.
- A window opens on the best ranked entry it can draw on: a ranking
  measured on KMS with `FNL_GPU_DRIVER`, whose entries name a kernel
  driver, and read by a run without it opened on a GPU name no renderer
  lists, which OpenGL took as no choice -- the window server's GPU,
  moved from once the list was whole.
- KMS: a monitor the kernel says went is unplugged only once it has been
  gone 5 s, or 2.5 s if the window is on it, and back in that time it was
  never unplugged: a monitor leaves the bus for a moment as it sleeps and
  as it wakes, and was taken off the output list, or had the window sent
  to another, each time. A move to it asked for meanwhile is made when it
  is back. A window whose monitor is unplugged now moves 2.5 s later.
- The GPU ranking keeps its figures per display and mode
  (`HDMI-A-1 3840x2160@30`), so another monitor on a cable, or another
  mode, is measured once where it was given the old monitor's figures;
  and the GPU box has an unnumbered "Recalibrate" row after the GPUs,
  which measures every entry on this display again and drops the other
  connected displays' figures, to be measured there when the window is.
- KMS: a GPU plugged in or out with the window open -- a dock's, an
  external one -- makes the GPU list again from every renderer, and its
  new entries are measured. Untested: no pluggable GPU has been at hand.
- The GPU list is ranked by measured frame rate, display by display:
  where an entry of the list -- each GPU with each renderer, and on KMS
  with `FNL_GPU_DRIVER` each kernel driver -- has no figure for the
  display the window is on, the library draws on it in turn, some 4 s an
  entry, the window saying so with the time left and taking no input
  meanwhile, and keeps the figures in the user's cache per program and
  backend; a GPU or display taken away keeps its figures. The list is
  ordered by the window's display's, best first, the GPU box says each
  figure, and a window that names no GPU opens on the first entry;
  `gpu_index` and an index in `FNL_GPU` count the ranked list.
  `FNL_GPU_RANK`: `calibrate` measures again, `keep` never measures,
  `order` orders the list alone, `off` reads none. The startup log's
  `GPU n:` lines are numbered as the list is.
- KMS, `FNL_GPU_DRIVER`: the GPU list has a move entry for each renderer
  the program has a scene for, `<GPU>, on <driver>` with that renderer,
  and a move lands on the entry's renderer, where it always landed on
  Vulkan: on a laptop with Vulkan and OpenGL, both on each kernel
  driver are in the list at once, and the titlebar's GPU box names each
  of that GPU's rows by the same name and the driver it draws on. Each
  keeps its number across the moves, where a move renumbered them: the
  row picked is the row in use after it. The
  window is still drawn across the move on Vulkan. A landing that finds
  no GPU of the name withdraws that driver's entry for that renderer
  alone, where it withdrew every move.
- On macOS, going into or out of fullscreen no longer shows the old frame
  small on a grey window, or cropped, after AppKit's animation: through
  the transition, when macOS shows no new frame, the last one is stretched
  to the window. A frame the window has outgrown is held at its top left,
  where it was held at the bottom left.
- On Windows an input method's composition arrives as
  `FNL_EVENT_COMPOSE`, as on Wayland and macOS, and the system no longer
  shows it in a window of its own: under wine on X11 that was a white box
  at the window's bottom-left holding a dead key's accent.
- Under wine, Direct3D 12 no longer spins while a window is resized: the
  frame that answers a size polls the frame statistics instead of
  DXGI's `WaitForVBlank`, which DXVK answers by spinning on the clock.
  A drag's CPU fell from 39% of a core to 15% on X11.
- On X11 with no window manager, a window opens no larger than its
  monitor's room, as on Win32: the default size on a screen smaller than
  it used to open past the screen's edge.
- A window is always vsynced: `fnl_set_vsync()` and `fnl_is_vsync()` are
  gone, and the library no longer binds `F10`, which reaches the program
  as any other key. `FNL_VSYNC=0` in the environment is the one way out,
  a diagnostic as `FNL_ECHO_EVENTS` is, read at `fnl_init()`; the testbed
  and `fenestal-pace` take `--no-vsync` for it. `fnl_present_mode()`
  still names what the renderer set.
- `fnl_input_latency_ms()`: how long an input takes to reach the caller,
  from when the key, the button, the wheel or the pointer moved -- by the
  window server's or the device's own stamp where the library shares its
  clock -- to when `fnl_poll_event()` hands it over. A mean over the last
  fifth of a second, refreshed and re-recorded as `fnl_frame_gpu_ms()` is;
  -1 before the first input. Every backend stamps its input; on Windows,
  where a message's time moves in 16 ms steps, a thread of the library's
  own reads raw input and stamps each report as it arrives. It takes the
  process's raw keyboard and mouse registration while the window is open,
  where the program has not registered its own, and gives back at close
  only what is still its own. The testbed shows it under the cost line, and
  `--frame-times` logs each figure.
- `fnl_set_transition_ms()`: a move to another GPU, renderer or kernel
  driver reads the last frame back, shows it through the gap wherever the
  library says what shows there, and dissolves the first frames on the new
  device from it; where no frame can be read back the window fades to
  black ahead of the move and up again after it. `FNL_EVENT_GPU` follows
  the dissolve. 250 ms until set; 0 is the cut there was. The testbed
  takes `--transition=MS`.
- KMS, `FNL_GPU_DRIVER`: a machine booted on nouveau moves to NVIDIA's
  module, which made none of its nodes there; with that module not
  loaded, `fnl_init()` loads it by a move there and back, where NVIDIA's
  Vulkan and EGL drivers otherwise tried at every instance -- the first
  frame at 5 s where it came at 13.6 to 19.5. A move whose GPU no Vulkan
  driver lists once it lands is offered no more.
- `fnl_request_gpu()` and the titlebar's GPU box move to the entry asked
  for even where the list grows before the move is made -- the other
  renderers' GPUs listed in the meantime, which shifted the kernel-driver
  entry and made a click on it a move to another GPU.
- Across a move to another kernel driver on KMS the renderer drawn
  meanwhile takes the copy path, so the output is not handed over for the
  landing and nothing goes dark; a take-back of the output shows the frame
  read back, not black. On Wayland the subsurface the frames went into is
  kept under the next one until that has shown a frame, and the window's
  own last frame likewise at the first move. On Win32 the child a move
  makes paints the frame read back until its first frame. On macOS the
  layer is no longer framebuffer-only, so that the frame can be read back.
- KMS: no framebuffer of the backend's is removed while a dark output's
  plane still shows it (a kernel `WARNING` on amdgpu at a move back
  between outputs of different sizes), and the pointer comes back after a
  move under OpenGL, where it was refused once on a CRTC not yet lit and
  then never asked for again.
- KMS switches layout groups: with `de,us` and a `grp:` option, the
  group key (Alt+Shift for `grp:alt_shift_toggle`) moves typing to the
  next group and logs which.
- `./build.sh linux-arm64` cross-compiles the Linux tree for 64-bit ARM
  (the Raspberry Pi 4) with `aarch64-linux-gnu-gcc`; consumers configure
  it with `-DCMAKE_TOOLCHAIN_FILE=cmake/linux-aarch64.cmake` and
  `-DFNL_GENERATED_DIR` naming a host tree's `generated/`.
- The macOS tree builds on a Mac as well as on Linux:
  `-DFNL_HOST_INCLUDE` names where a cross tree finds the Vulkan and GL
  headers, `/usr/include` by default, which a Mac does not have.
- `./build.sh macos-arm64` builds the macOS tree for Apple's own
  processors (`cmake/macos-arm64.cmake`); the x86-64 toolchain file takes
  `FNL_MACOS_ARCH`.
- macOS: Vulkan is also looked for in the Frameworks folder of the app
  bundle a program is in, `@executable_path/../Frameworks`, after the
  program's own folder.
- The library's words are these: a *backend* is the window system it opens
  (Wayland, X11, Win32, Cocoa, KMS), a *renderer* the graphics API that
  draws (Vulkan, OpenGL, Direct3D 12, Metal), a *platform* one of each,
  such as Vulkan on X11, and the *GPU* the device that does the work. The
  API follows them, from 1.1.0's names: `FnlGpuBackend` is `FnlRenderer`,
  its `FNL_GPU_DEFAULT` and `FNL_GPU_VULKAN` are `FNL_RENDERER_DEFAULT` and
  `FNL_RENDERER_VULKAN`, and `FnlInitDesc::gpu` is `renderer`;
  `fnl_gpu_backend()` is `fnl_renderer()`, `fnl_gpu_backend_name()` is
  `fnl_renderer_name()` and `fnl_window_system()` is `fnl_backend_name()`;
  `FNL_ERR_BACKEND` is `FNL_ERR_SYSTEM`, a refusal by the system, a driver
  or a runtime, with the same value (`fnl_result_str()` says "system
  error"). What names the device still says "gpu": `fnl_gpu_name()`,
  `FnlInitDesc::gpu_index` and `gpu_name`, `on_gpu_created` and
  `on_gpu_lost`, `FnlVulkanGpu` and its Direct3D 12 and Metal siblings,
  and `FNL_GPU` in the environment. At configure `-DFNL_PLATFORM` is
  `-DFNL_BACKEND` and `-DFNL_GPU` is `-DFNL_RENDERER`, and a build
  directory holding either old one at anything but `linux` or `all` is
  refused with the new name; so is `FNL_PLATFORM` or `FNL_GPU_API` in a
  program's environment. The values and what each returns are unchanged.
- Text entry on Wayland and KMS: the keysym name table has every spelling
  X11's header has, the ones xorgproto deprecated in 2023 included, so a
  keymap that writes `guillemotright` or `masculine` -- xkeyboard-config's
  files do, and so a compositor's keymap -- types `»` and `º` where those
  levels typed nothing; and it is made from xorgproto 2026.1's header,
  which names code points the older one wrote as `U203A`.
- KMS: the layout is read from xkeyboard-config's own files
  (`/usr/share/X11/xkb`, or `XKB_CONFIG_ROOT`), the console's names
  resolved through `rules/evdev` and the symbols files' includes followed
  by the library; `xkbcli` is no longer run, so a console with the data
  and not the tool -- Debian's default -- types. `FNL_KMS_LAYOUT=de(nodeadkeys)`
  names the layout in the console's place (`de,us` for two groups, the XKB
  options after a space), on KMS alone; the testbed's `--layout=` sets it.
  Under a window server the variable is answered with one line and the
  session's layout applies. The log says which layout was applied and
  where its names came from (`KMS: layout ...`).
- A Linux build has every Linux backend -- Wayland, X11 and KMS -- and a
  program opens the one its session has when it starts: Wayland where
  `WAYLAND_DISPLAY` is set, else X11 where `DISPLAY` is, else, with
  neither, KMS on the console, stepping past one that finds no server.
  `FNL_BACKEND=wayland`, `x11` or `kms` in the environment opens that one
  or fails, and the log says which was opened (`backend: <Name>`).
  `-DFNL_BACKEND=x11` (or `wayland`, `kms`) at configure builds one
  backend alone. The targets are `linux-vulkan` and `linux-gl`, in place
  of the six per platform.
- No runtime that is not the system's is linked: the Vulkan loader, EGL,
  `wayland-client`, `wayland-egl` and `xcb` are opened when the library
  first needs them, so a machine missing one still runs the program, on
  another backend or renderer. A consumer's link line is the system's
  libraries alone -- `m`, `pthread` and, before glibc 2.34, `dl` on Linux.
  A program whose own scene calls Vulkan now links the loader itself
  (`Vulkan::Vulkan`), since the library no longer brings it.
- A Windows build has Direct3D 12 and Vulkan in one library
  (`-DFNL_RENDERER=all`, the default there), and `fnl_init()` draws with
  the first of them that works on the machine, on the same window.
  `FnlInitDesc` gains `renderers`, a mask of `FNL_RENDERER_BIT()`s naming
  the renderers the program has a scene for, or `FNL_RENDERERS_ANY` for a
  program that draws none; a build of several renderers refuses a
  descriptor with neither `renderer` nor `renderers`, since a program with
  one scene must not be handed another API. `FNL_RENDERER=d3d12` or
  `vulkan` in the environment names one, which is taken or fails, and the
  log says which draws (`renderer: <Name>`). `fnl_renderer()` answers the
  window's renderer, and with no window `FNL_RENDERER_DEFAULT` in a build
  of several. Each renderer's interop call refuses a window another
  renderer draws. The target is `win64`, in place of `win64-vulkan` and
  `win64-d3d12`.
- Linux and macOS builds have every renderer of their system too: Vulkan
  and OpenGL on Linux (Vulkan first), Metal and Vulkan on MoltenVK on
  macOS (Metal first). `-DFNL_RENDERER=all` is the default everywhere, and
  a name builds that renderer alone. The targets are one per operating system --
  `linux`, `win64`, `macos` -- in place of `linux-vulkan`, `linux-gl`,
  `macos-metal` and `macos-vulkan`.
- `fnl_frame_gpu_ms()` and `fnl_frame_cpu_ms()` say what drawing a frame
  costs -- the GPU's time on it, from the renderer's timestamps, and the
  render thread's recording and submitting it -- apart from how often a
  frame is drawn, which `fnl_fps()` says and which for a window that draws
  only when something changed says nothing of performance. Like
  `fnl_fps()`, they are refreshed five times a second while frames are
  drawn,
  and once more when the window comes to rest, and a change re-records the
  frame, so a caller drawing them needs no `fnl_invalidate()`.
  `fnl_set_cost_sampling()` keeps them and `fnl_fps()` measured while
  nothing changes: a window at rest then draws one frame five times a
  second,
  and the three are refreshed together with it, for a caller while it
  shows them. The testbed shows both under its counter, with the rate the
  slower allows, in fixed columns -- the slower time red, the faster green,
  the rate yellow -- and samples while its panel is up; and the
  testbed and `fenestal-minimal` name the renderer and the GPU in the title
  again at every `FNL_EVENT_GPU`.
- A renderer that would draw on a software rasteriser gives way, at
  startup, to a later one with a hardware GPU -- Vulkan with only lavapipe
  where OpenGL has the card -- unless a renderer or a GPU was named; the
  log says so in one line.
- Direct3D 12 chooses the adapter Windows ranks first for performance --
  the discrete GPU on a laptop with two, or the one Windows' graphics
  settings name for the program -- where it took the first one listed,
  the panel's. `gpu_index` still counts the list in its own order.
- The version goes back to 0.9: the API is not stable yet, and the 1.x
  promise below is withdrawn until it is. A minor release may now rename,
  change or remove anything, at the source and at the binary.
- `fnl_init()` refuses a header from another minor release as well as from
  another major one.
- The shared object's soname is `libfenestal.so.0.9`, and its symbols are
  versioned `FENESTAL_0`, one node.
- The demo program is `fenestal-testbed`, in `testbed/`; it was
  `fenestal-stub`, in `examples/stub/`.
- `on_record` and `on_overlay` are documented to run whenever the frame is
  recorded: at least when something they draw from has moved, and as often
  as the renderer needs -- once per swapchain image under Vulkan, every
  frame shown under a renderer that cannot keep a recording. Nothing calls them
  differently yet; a program that counts calls should not.
- OpenGL 4.5 core is a second renderer, on X11 through EGL:
  `FNL_RENDERER_OPENGL` and `fnl_gl.h`, the counterpart of `fnl_vulkan.h`, with
  `FnlGlSceneLayout` for the scene's framebuffer. The `x11-gl` target builds
  it; a program links `EGL` in place of `Vulkan`, and reaches GL through
  what `eglGetProcAddress` hands out.
- OpenGL on Wayland too, through EGL and `wl_egl_window`: the `wayland-gl`
  target, linking `wayland-egl` beside `wayland-client`.
- OpenGL on KMS too: the `kms-gl` target, on Mesa's surfaceless EGL, its
  frames handed to the backend's copy path. Every Linux backend now
  takes OpenGL; Windows and macOS do not.
- The `--frame-times` line (`FnlInitDesc::frame_times`) reports `wait` in
  its two parts, `fence` and `acquire`: the device's fences, and the
  swapchain's image or frame-latency object.
- Direct3D 12 is a third renderer, on Windows: `FNL_RENDERER_D3D12` and
  `fnl_d3d12.h`,
  with `FnlD3D12Gpu` (the device and the shared queue), `FnlD3D12SceneLayout`
  and `FnlD3D12Scene` (the list, the render target, the depth and the rectangle
  drawn into). The `win64-d3d12` target builds it; a program links `d3d12`,
  `dxgi` and `dxguid` in place of `vulkan-1`. `on_record` runs for every frame
  shown there, as under OpenGL.
- macOS: Cocoa is a fifth backend, and Metal a fourth renderer on it --
  `FNL_RENDERER_METAL` and `fnl_metal.h`, with `FnlMetalGpu` (the device and the
  shared queue), `FnlMetalSceneLayout` and `FnlMetalScene` (the encoder,
  its command buffer and the rectangle drawn into), whose objects are
  Metal's types from Objective-C and `void *` from C. The `macos-metal`
  target builds it and `macos-vulkan` the Vulkan renderer on MoltenVK, both
  cross-compiled; a program links AppKit, QuartzCore and
  Metal, or MoltenVK in Metal's place. `on_record` runs for every frame
  shown under Metal. The window's layer is tagged sRGB, which is what the
  frame's values are taken to be. The library registers
  `ApplePressAndHoldEnabled` off as the program's default, so that a held
  letter repeats under text entry; a program that sets it keeps its own.
- `FnlEvent`'s per-kind structs are named types -- `FnlKeyEvent`,
  `FnlButtonEvent` and the rest, `FnlLengthEvent` for the composition and
  the clipboard -- and its union holds them: a type declared inside an
  anonymous union is not C++, and clang++ refused the header under
  `-pedantic-errors`. `ev.key.pressed` reads as before, and the layout is
  unchanged. The `clang` trees check the headers as C++ with clang++.
- On macOS the library's threads run under the time-constraint policy, so
  that a timed wait comes back on time: the kernel's timer coalescing held
  a 60 Hz window to under 50 frames a second.
- The Vulkan instance enables `VK_KHR_portability_enumeration`, and the
  device `VK_KHR_portability_subset`, where offered: what a driver that
  implements Vulkan on another API, MoltenVK, needs to be found and used.
- X11: a resize by the border no longer shows the new size before its
  frame. The window speaks `_NET_WM_SYNC_REQUEST` where the server has
  SYNC; OpenGL there no longer waits in Mesa's swap under a compositor, or
  during a resize without one. The Vulkan instance enables
  `VK_KHR_get_physical_device_properties2`, and the device
  `VK_KHR_present_id` and `VK_KHR_present_wait`, where offered.
- Win32 under wine: a moved Vulkan window no longer goes black at rest.
- A window the graphics API says is not shown (`DXGI_STATUS_OCCLUDED`)
  draws nothing until it is, with vsync off too, where it drew flat out.
- Win32 with Direct3D 12: the frame that answers a size waits until it is
  shown before the window takes the next, as Vulkan's does; a border drag
  shows far fewer frames of a size the window has left, at a lower frame
  rate while it lasts.
- `fnl_set_title()` changes the window's title after `fnl_init()`, on the
  window server and on the library's own titlebar; `fnl_gpu_short_name()`
  is `fnl_gpu_name()` short enough for one ("Intel HD Graphics 530"). The
  testbed, `fenestal-minimal`, `fenestal-pace` and `fenestal-lifecycle`
  put the backend, the renderer and that name behind their titles.
- The library's titlebar calls its render-scale checkbox `Fs Upsample`.
- A window can be moved onto another GPU while it stays open.
  `fnl_gpu_count()`, `fnl_gpu_current()`, `fnl_gpu_name_at()` and
  `fnl_gpu_kind_at()` list the GPUs the renderer can draw on -- hardware,
  and a software rasteriser where one is installed -- in the order
  `gpu_index` and `FNL_GPU` count, with `FnlGpuKind` saying integrated,
  discrete, virtual or software; `fnl_request_gpu()` moves the window, the
  device rebuilt there with `on_gpu_lost` and `on_gpu_created` between, and
  a GPU that will not build is gone back from. The list spans every
  renderer the program has a scene for (`FnlInitDesc::renderers`), the
  window's own first and the others' listed after the first frame, and
  `fnl_gpu_renderer_at()` says which draws on an entry: a GPU of another
  renderer moves the window to that renderer, the one drawing torn down
  and the other built on the same window, and `on_gpu_created` asks
  `fnl_renderer()` which scene to build. `FNL_EVENT_GPU` (`ev.gpu`: the
  index and the renderer) says the window is on another GPU, or that the
  list grew. The
  titlebar has a GPU drop-down, dim where the list has one GPU, which puts
  69 points on the window's floor. OpenGL lists the GPUs EGL will open the
  window on: every one on KMS, and on Wayland and X11 those whose driver
  takes the GPU it is named -- less any that makes no 4.5 core context
  (nouveau before Turing), which is left out with a line, and which EGL's
  own choice gives way from. On KMS the Vulkan list is every GPU with a
  graphics queue, which reaches the output by taking its display, by the
  copy path on a headless surface, or, where its driver has none
  (NVIDIA's own), by the copy path from images of the renderer's own; a
  move goes between them, and a GPU index there counts that list. On
  Win32 a window moved once presents into a child window of its own from
  then on, and on Wayland into a subsurface of its own: on GNOME a window
  moved from a hardware GPU to lavapipe otherwise kept showing the last
  hardware frame. Under wine on Wayland the child window is there from
  the start: wine keeps a window's old surface showing, and under cage a
  window moved to another GPU stopped drawing.
- Under wine the other renderers' GPUs are listed at `fnl_init()`, before
  the window's renderer is built, rather than after its first frame: a
  Direct3D 12 window on NVIDIA under weston or cage aborted while Vulkan's
  were being listed beside it.
- The titlebar's open drop-down list is drawn over the caller's text as
  well as its rectangles; a caller's readout under it no longer shows
  through.
- The library's titlebar keeps its checkboxes and size drop-down as it
  keeps its buttons: the window's floor is wide enough for all of them
  (556 points; 583 where the window is the output), and in a window too
  narrow for the title as well, the title is what is dropped. The controls
  no longer give way to the title.
- On Windows, a maximized window made fullscreen reports it
  (`FNL_EVENT_WINDOW`, and the area without the bar) on a monitor whose
  work area is the whole monitor -- no taskbar there, or wine on an X
  server with no panel. It went fullscreen and said nothing, since the
  window kept its size.
- Under wine, the wheel is read from raw input while the window has the
  focus and the pointer, so a step is one `FNL_EVENT_SCROLL` of its own.
  Wine's server could lose a step it merged into the previous one's
  message; on its Wayland driver, about one pair of steps in forty came as
  a single notch. Windows is unchanged.
- KMS: a move from OpenGL, or from Vulkan's copy path, to a GPU that takes
  the display no longer crashes inside Mesa's driver: the backend's own
  page-flip event, left in the DRM node's queue, was read by the driver's
  wait thread as one of its own images. The queue is emptied before the
  node is lent and after each mode set (DESIGN *KMS: no window server*).
- KMS: no framebuffer is removed while a plane shows it. The backend takes
  the output back with a black frame of its own before a display swapchain
  is destroyed, keeps its own buffers across a change of size until the
  new ones show, and at exit sets every CRTC back to what it showed at
  start, the console as a rule. On amdgpu each such removal was a kernel
  `WARNING` with a stack in the journal (DESIGN *KMS: no window server*).
- KMS: without vsync the copy path's and OpenGL's flip is asynchronous
  (`DRM_MODE_PAGE_FLIP_ASYNC`): the frame goes out at once and tears, as
  IMMEDIATE does on a window server, where the driver takes the flag, and
  flips at the vblank as before where it does not. Vulkan's display path
  stays FIFO, the one mode Mesa offers.
- KMS: `FNL_GPU_DRIVER` moves an NVIDIA GPU between nouveau and NVIDIA's
  own kernel module with the window open, and is refused with a line under
  a window server, which may hold the GPU itself. Set, the GPU list has an
  entry `<GPU>, on <driver>` that `fnl_request_gpu()` and the titlebar's
  GPU box take; the renderer is built again on Vulkan on the other driver.
  While the move runs the window is drawn on another GPU -- Intel's or
  AMD's, or Mesa's CPU rasteriser, Mesa's drivers alone -- so `on_gpu_lost`
  and `on_gpu_created` come twice for it; where that cannot be, its frames
  pause for a second or two, with a line. A driver's name (`nouveau`,
  `nvidia`) also binds it before the first frame. The move is
  `tools/gpu-driver.sh`'s, as root, asked through the socket
  `tools/drive/bare_driver.py --setup` puts in front of it for the group
  `fenestal-gpu`, where a move outlives the program and one killed halfway
  is bound again by the service, or through sudo where there is none;
  `FNL_GPU_DRIVER_SOCKET` names another socket. No move is offered for
  the GPU whose card the window is scanned out on, which the backend
  holds. An OpenGL window can move too, NVIDIA's own OpenGL included:
  while it is set the library closes `libEGL.so.1` between OpenGL's
  spells, since glvnd left loaded keeps NVIDIA's driver holding or blind
  to the GPU, and OpenGL draws on a GPU under nouveau through Zink (it
  sets `NOUVEAU_USE_ZINK=1` where the environment has none, the one
  variable it writes). Where something else keeps NVIDIA's driver in the
  process, the move is not made and no move is offered after it, with a
  line.
- Vulkan: the instance extensions are asked for once an instance, where
  each optional one asked again, and each asking loads every driver's
  library. A machine with NVIDIA's driver installed and its module not
  loaded paid about 2.5 s a load.
- Vulkan: the pipeline cache file is one a device,
  `fenestal.pipeline_cache-<uuid>` by the device's own cache UUID, where
  one file held the last device's cache alone and a window moved between
  two drivers of one GPU began each build cold and wrote it at every
  move. The old `fenestal.pipeline_cache` is not read.
- OpenGL: the other renderers' GPUs are listed before OpenGL's display
  opens, where they were listed after the first frame beside it: NVIDIA's
  Vulkan driver terminates NVIDIA's EGL display when an instance beside
  it ends, which only a context already current survived. And a display
  terminated under the renderer -- by another library's instance -- is
  opened again at the next recovery, with a new context and surface,
  where the surface was retried for ever.
- A move to another renderer or GPU says what its build took, by step:
  `rebuilt on …, drawn with … in 529 ms (instance 180, surface 181,
  objects 169)`.
- A still window that draws only to show the figures -- the metrics
  panel's, or `fnl_set_cost_sampling()`'s -- stays at rest: on X11 those
  frames, a fifth or a half of a second apart, no longer read as missed
  ones and deepen
  the display queue for the next frames that move. Under wine, where a
  thread's CPU time is not known, `fnl_frame_cpu_ms()` leaves them out as
  it does everywhere else.
- Win32: a press in the window takes the mouse, so a button released
  outside it is still released for the caller and the titlebar; a press
  that starts a move or resize by the system is released as the system
  takes it. Alt+F4 closes the window, and Alt with a letter no longer
  beeps.
- X11: under a window manager that syncs a resize, the last event of a
  gesture -- the release that ends a drag of the border -- is no longer
  held until something else wakes the window.
- Wayland: a copy followed by a paste in the same turn pastes what was
  copied, not the clipboard before it -- even while an earlier paste is
  still arriving, which it then answers too -- and a keymap the
  compositor says is larger than its file is refused rather than read
  past its end.
- X11: a paste is asked, and the clipboard taken by a copy, with the
  server's time rather than `CurrentTime`, as ICCCM wants; a late answer
  to a paste given up -- a slow owner's, after the deadline -- is no
  longer taken for the next paste's; the pieces of a large one cut short
  land where the next transfer does not read. A paste asked after a copy,
  while an earlier paste is still on its way, answers with what was
  copied, as on Wayland.
- The titlebar's open drop-down acts only while the library's bar shows
  it: under `fnl_titlebar_custom()` or in fullscreen a press no longer
  lands on its hidden rows or boxes, the list is put away rather than
  coming back with the bar, and a checkbox pressed while a list is open
  only puts the list away.
- macOS: `fnl_output_name()` names the second of two monitors of one model
  as `fnl_output_name_at()` lists it, "DELL U2720Q (2)", so the name a
  window reports moves it back to the same monitor.
- KMS: a tablet or touchscreen no longer creeps right and down while the
  pointer is captured, nor jumps at a new touch; and on a console with two
  layouts each group keeps its own key type, which an `augment` no longer
  replaces.
- X11: a fullscreen or maximized window moved to another monitor
  (`fnl_request_output()`, the output box) is put back, when it leaves
  that state, where it was on the first monitor's terms, not in the new
  one's corner.
- The GPU ranking keeps two monitors of one model apart on macOS, where
  the second is named "(2)": each is calibrated for itself.
- `fnl_vulkan_gpu()`, `fnl_vulkan_scene_layout()`, `fnl_d3d12_gpu()` and
  `fnl_metal_gpu()` were any thread's and are now the render thread's,
  from `on_gpu_created`, `on_record` or `on_overlay`: they read the
  device and its objects, which a move to another GPU replaces under
  them, and return false on any other -- the thread that called
  `fnl_init()` counting as the render thread while none runs. OpenGL's,
  Direct3D 12's and Metal's scene layouts, `fnl_gl_proc()` and
  `fnl_gl_version()` stay any thread's.
- Direct3D 12: a failed setup of the Direct3D 11 present no longer leaves
  half of it to be used, and an adapter named by `FNL_GPU` is no longer
  left referenced at every build.
- OpenGL: a frame that showed nothing -- no drawable on macOS, no buffer
  on KMS -- is no longer counted as presented, and `fnl_present_mode()`
  on macOS no longer reads the renderer's state from another thread.
- Win32: `fnl_request_output()` on a maximized window moves the window,
  maximized, where it moved only the window's restored rectangle and left
  the window itself where it was until a restore.

## 1.1.0

- X11: where every swapchain made at the window's size is out of date at
  once, as NVIDIA's driver can leave it after a drag on a window nobody
  manages, the library widens the window by a pixel and back, which frees
  it. The window keeps its size; a program may see one resize there and
  back.
- Wayland: the late-latch hold backs off where a compositor answers a late
  frame a little late rather than a whole period late, as weston does for
  NVIDIA's driver. That pairing drew at 40 fps and now draws at 60.
- KMS: the titlebar has no minimize or maximize button, since the window is
  the output and can be neither; fullscreen sits beside close. Beside the
  checkboxes it lists the output's display modes, refresh and all, and the
  card's connected outputs: a pick sets that mode, or moves the window to
  that output at its preferred mode and turns the one left off. It has no
  *Scale scene* box, since no pick there sets the scene's size;
  `fnl_set_scene_size_px()` still does. A monitor plugged in joins the
  list, one plugged back in is the output it was, and the window leaves an
  output unplugged for another, or waits, drawing nothing, until one
  comes.
  `fnl_mode_count()`, `fnl_mode_get()`, `fnl_mode_current()` and
  `fnl_request_mode()`, and `fnl_output_count()`, `fnl_output_name_at()`
  and `fnl_request_output()`, are the same for a program, with `FnlMode`;
  elsewhere the lists are empty and the requests do nothing.
  `fnl_request_size_pt()` does nothing on KMS, as before. A mode or output
  that will not show is gone back from, with a warning. Where the GPU
  drives the display itself a switch rebuilds the device as a lost surface
  does, so `on_gpu_lost` and `on_gpu_created` run; on the copy path it is a
  resize.
- The automatic GPU choice ranks Mesa's NVK on an NVIDIA GPU older than
  Turing after an integrated GPU: it runs at boot clocks there, and on
  Pascal nouveau's runtime suspend garbles the text's atlas. A name or an
  index (`FNL_GPU=nvidia`) still chooses it, and the atlas then goes in
  system memory, which the suspend does not touch.
- Wayland: the window geometry and the buffer scale are sent immediately
  before the present they describe. Sent earlier, a restore from
  maximized could reach the commit of an older frame, and weston 15
  closed the connection over it. The ack of a configure goes ahead of the
  geometry when the window leaves maximized or fullscreen, so weston 15
  applying an update it held back for a FIFO barrier no longer finds the
  restored geometry against the maximized state.
- KMS: a GPU chosen by name or index that does not drive the display --
  the NVIDIA GPU of a laptop whose panel is on Intel -- renders the frames,
  and they reach the display by the copy path that was lavapipe's: read
  back, written into the display's buffer, flipped. Before, the display's
  own GPU was used and the choice was ignored with a warning. The copy
  path reads back from cached system memory where the device has it,
  which on NVK takes a 1920x1080 frame from 425 ms to about 8. An index
  there counts the driver's enumeration, as the startup log of the copy
  path lists it, where before only the display's own GPU was counted: on
  a laptop whose NVIDIA GPU is enumerated first, `FNL_GPU=1` is now that
  one.
- `FNL_EVENT_OUTPUT` (17): the window is on another output, or its output
  changed size. `ev.output` is the size in points; the name is
  `fnl_output_name()`. A caller that compared the name once a frame can
  wait for this instead.
- X11 with no window manager: a window left on an output that is switched
  off is moved onto the primary monitor, as a manager would.
- X11: keys are named by the XKB keymap's key names, so a server whose
  keycodes are not evdev's names them by position too.
- X11 with no window manager: a drag the library runs itself moves the
  window's width in steps of 16 pixels, which keeps Mesa's software
  present from showing its row padding down a widened edge. A size the
  caller asks for is never rounded.
- X11: the window's background is the titlebar's colour above and black
  below, which is what a widened window shows until its next frame.
- `--frame-times` lines end with `period`, the spacing's estimate of the
  display period in milliseconds, zero where none is learnt.
- A fourth platform, `FNL_PLATFORM=kms` (`./build.sh kms-vulkan`): a Linux
  console with no window server. The window is one output at its preferred
  mode, maximized for its whole life; `fnl_window_system()` says `"KMS"`.
  It links Vulkan and pthreads, reads `/dev/dri` and `/dev/input` itself
  (groups `video` and `input`), takes the layout from `xkbcli`, and grabs
  the keyboards while it runs. A device offering
  `VK_EXT_acquire_drm_display` presents the display; on any other --
  lavapipe -- each frame is drawn headless and copied on.
  `FNL_KMS_OUTPUT` names the output.
- Wayland: one line at open names the optional globals the compositor did
  not offer, where it left any out.

## 1.0.0

The API is frozen for 1.x: see `fenestal.h` *stability* and DESIGN
*Compatibility*.

- `libfenestal.a` exports the API and nothing else. It is one partially
  linked object, LTO-optimised with gcc, so a program can define its own
  `app_event` or `create_swapchain` without colliding with ours.
- The shared object's symbols are versioned `FENESTAL_1.0`; its file is
  `libfenestal.so.1.0.0`, its soname still `libfenestal.so.1`.
- `fnl_run()` called off the thread that called `fnl_init()` returns
  `FNL_ERR_INVALID_ARGUMENT`; `fnl_shutdown()` called from a callback logs a
  line and does nothing instead of deadlocking.
- Every enum's values are now written out (`FnlLogLevel`, `FnlEdge`,
  `FnlTextSize`, `FnlSceneTarget`, `FnlGpuBackend` gained explicit numbers,
  all equal to what they had). `FNL_DEPRECATED` is new.
- `FnlInitDesc::scale_override120`, `gpu_index` and `late_latch_fixed` are
  marked `UNSTABLE` and are outside the 1.x promise.
- A lifetime that installed the signal handlers no longer leaks its wakeup
  descriptors; teardown waits out a handler still in flight before freeing.
- `FNL_EVENT_SCROLL` is in notches on every platform: one wheel click is 1.0.
  Wayland delivered the compositor's axis units, about ten to a click.
- A window whose frame would show what the last one did draws nothing: no
  submit and no present until a change, an event, a size or
  `fnl_invalidate()`. A still window cost 2-3% of a core on a GPU and 85%
  under Mesa's software rasteriser with its X server. `fnl_fps()` then
  keeps the rate last drawn at, and `--frame-times` prints `frame at rest`.
- Behaviour that differed between platforms now matches:
  - Win32 no longer hands a click to the window manager while the pointer is
    captured;
  - a caller's `fnl_begin_move()` or `fnl_begin_resize()` no longer leaves a
    titlebar double-click half-armed on X11;
  - `fnl_request_size_pt()` below the window's floor is raised to it on every
    platform;
  - a maximize on Win32 is drawn without waiting for the frame pacing;
  - a held modifier no longer repeats in text mode on Win32;
  - a Wayland paste past 64 MiB is refused, as on X11;
  - the library's own close button posts `FNL_EVENT_CLOSE`, as a close
    from the window manager always did;
  - on X11 with no window manager, a maximized window that leaves
    fullscreen is maximized again, as it is under one, and not restored
    while still reporting itself maximized;
  - on X11 the frame loop is never spaced by less than the refresh of the
    monitor the window is on, which RandR reports: a driver whose present
    blocks on nothing, as Mesa's software one does, drew up to 150 frames
    a second for a 60 Hz panel, resting and resizing.


## 0.x

Pre-release. The window, the device, the swapchain, the render thread and the
text overlay, on Wayland, X11 and Win32 over Vulkan; DESIGN.md records how
each part came to be as it is. No compatibility was promised.
