# Plan: frame costs that compare across renderers

`fnl_frame_gpu_ms()` and `fnl_frame_cpu_ms()` are read as one figure in
three places: the testbed's cost line, the F5 metrics, and the GPU ranking
(`src/core/rank.c`, DESIGN *Ranked by measured frame rate*), which puts
every GPU-and-renderer pair in one list by `1000 / max(gpu, cpu)`. Each
renderer times both halves over its own span, and the spans differ. The
differences favour OpenGL and Metal, so a ranking across renderers partly
ranks how they are timed. This plan makes the spans the same thing
everywhere, measures the gap before and after, and checks the result
against a frame rate actually drawn.

Nothing here is built yet. Each phase lists what it changes and what it is
measured by; the user drives every build and every run.

## What started it

This desktop's ranking cache (`gpu-rank-fenestal-testbed-<backend>.txt`
in `~/.cache/fenestal/`), the RX 590 on RADV and radeonsi:

| display | OpenGL | Vulkan | gap |
|---|---|---|---|
| Wayland, DP-1 3773x2134@60 (2026-10-27) | 3615 fps, 0.28 ms | 2840 fps, 0.35 ms | 0.08 ms |
| KMS, DP-3 3840x2160@60 (2026-10-26) | 2698 fps, 0.37 ms | 1789 fps, 0.56 ms | 0.19 ms |
| KMS, software (llvmpipe, lavapipe) | 29.5 fps, 34 ms | 22.0 fps, 45 ms | 11 ms |

The file keeps the figure, not its halves, so which half set each row is
not known. The testbed now opens on OpenGL on this desktop for that reason.

## What each renderer times today

The CPU half is `late_cost`, the running mean the late-latch cap already
kept, from `t_rec` (past every device and display wait) to `t_present`
(just before the present call). The GPU half is a timestamp pair.

| renderer | CPU span ends | the submit inside it? | GPU span |
|---|---|---|---|
| Vulkan | before `vkQueuePresentKHR` (vulkan/frame.c:941) | yes, `vkQueueSubmit` (:1189) | `TOP_OF_PIPE` at the command buffer's start (:338) to `BOTTOM_OF_PIPE` (:452), the readback (:450-456) inside |
| OpenGL | before the swap (gl/frame.c:563) | no: Mesa sends the batch at the swap (:569) | `GL_TIMESTAMP` after the slot's wait (:487) to after the blit (:553); the copy path's readback after it |
| D3D12 | before `Present` (d3d12/frame.c:1002) | yes, `ExecuteCommandLists` in `submit()` (:979) | a timestamp at the list's start (:943) to after the present barrier (:958), the snapshot's copy (:963) inside |
| Metal | before `[cb commit]` (metal/frame.m:503, :514) | no | `GPUStartTime` to `GPUEndTime`, the whole command buffer, the snapshot's blit (:490) inside |

`fenestal.h` promises "the render thread's recording and submitting it",
which OpenGL and Metal do not keep.

The differences, and which way each one leans:

1. **The submit.** Vulkan and D3D12 count the call that hands the frame to
   the driver; OpenGL and Metal count only queuing commands. On radeonsi
   the gap is larger still: its threaded context and its winsys do the
   driver's work and the kernel submission on threads of their own, and
   glthread, where on, moves the API calls off the render thread too. None
   of it is the render thread's time, under any span.
2. **The start of the GPU span.** A `GL_TIMESTAMP` is written once every
   earlier command has completed. Vulkan's `TOP_OF_PIPE` is written when
   the command processor reaches it, which on some hardware is before the
   previous submission has drained. D3D12 does not say which it is.
3. **A wait for the window's image.** Vulkan's batch waits for the
   acquired image at `COLOR_ATTACHMENT_OUTPUT` (vulkan/frame.c:1171). A
   driver that honours that stage runs the offscreen scene pass first and
   holds the window's pass inside the span. RADV waits before the whole
   submission, so not on this desktop; NVIDIA is the candidate. OpenGL,
   D3D12 and Metal wait on the CPU or before the queue runs the list.
4. **Copies that are not the frame.** Vulkan's copy-path readback is
   inside its span at every frame on KMS's copy path; OpenGL's is after
   its end stamp. The software rows above are that. Snapshot copies (a
   move's) are inside Vulkan's, D3D12's and Metal's, and rare.
5. **Work the swap does.** radeonsi's end-of-batch flush and whatever it
   does to hand the back buffer to the display come after OpenGL's end
   stamp. Vulkan's render pass ends with the change to `PRESENT_SRC`
   inside its span. Not yet sized.

One difference is not a measurement's and stays: Vulkan reuses a recorded
command buffer while the draw state stands (DESIGN *Recording*), and the
other three record every frame. That saving is real.

## What the figures should mean

Written once, here and then in DESIGN *At rest* and `fenestal.h`:

- **The GPU's time on a frame**: from the point where the GPU has finished
  everything before the frame to the point where the frame's image is
  complete, with any wait for the window's image left out, and with no
  copy that exists only to read the frame back (the copy path's, a
  snapshot's).
- **The CPU's time on a frame**: the CPU time the frame costs the threads
  that make it -- the render thread from the end of its waits to the
  return of the call that hands the frame over, and the driver's own
  threads -- with the time blocked in a wait for the device or the display
  left out. Counted as a pipeline is: a frame cannot be drawn faster than
  its slowest thread can do its share, so the figure is the larger of the
  render thread's share and the busiest driver thread's, not their sum.
- `late_cost` keeps its own meaning, the wall time from latch to present
  that the late-latch cap leaves room for (src/core/frame.c:247). It is
  pacing's, and the cost figure stops borrowing it.

## Phase 0: measure the gap first

Before changing a span, find out which of the five differences make up the
figures above. A diagnostic under `--frame-times`, switched on by
`FNL_COST_PARTS=1` in the environment (`src/core/parts.c`), in every tree:
the Debug trees are the checker trees, whose timings are the checker's.
Built, and kept after the plan: off by default, it costs nothing, and
it is how a ranking that looks wrong is read. Windows reads its CPU half
since Phase 2's clock; under wine it says `not known`.

- Vulkan: four more timestamps, each at the bottom of the pipe -- at the
  start, after the scene pass, before `vkCmdEndRenderPass` and after it --
  logged per window as the parts of the span: the lead of the top-of-pipe
  stamp over a bottom-of-pipe one (difference 2), the scene pass, the
  window pass (difference 3, where the image wait lands there), the end of
  the pass (difference 5), the readback (difference 4). Stamps at the
  bottom of the pipe drain it, so the span with them on runs a little long;
  the parts are read against each other, and the span against a run with
  them off.
- OpenGL: the render thread's own CPU time over the swap
  (`CLOCK_THREAD_CPUTIME_ID` around `gl_bridge_swap()`), beside the wall
  time the line already reports as `present`.
- Every renderer: `CLOCK_THREAD_CPUTIME_ID` over `t_rec..t_present`,
  beside the wall-clock span, so the line shows how much of each is CPU.
- The process's threads: once a window, each thread's CPU from
  `/proc/self/task/*/schedstat` and its name from `comm`, the library's
  two threads marked. This says which driver threads exist (radeonsi's,
  glthread, NVIDIA's) and how busy each is per frame.

Runs, one per renderer in the same session, the testbed moving its scene:
this desktop on Wayland and on the console; schenker's NVIDIA on its own
driver and on NVK (difference 3), and its Intel GPU; thinkpad's software
devices on KMS (difference 4). The result goes into DESIGN as the
measurement behind the phases that follow, and a phase whose difference
measures as nothing is dropped.

### Measured on schenker's console (2026-10-28)

The release tree, `FNL_BACKEND=kms`, the testbed's scene moving (F2 by
uinput), eDP-1 1920x1080 at 60 Hz, vsync on, `--gpu-rank=off`; the mean
of the run's later lines, milliseconds a frame. The panel is the Intel
GPU's, and so is HDMI-A-1: the NVIDIA GPU takes neither, so on either of
its drivers both renderers draw offscreen and copy each frame over.

| GPU, renderer | path | GPU span | its parts | record+submit, wall / CPU | present, wall / CPU |
|---|---|---|---|---|---|
| NVIDIA 580, Vulkan | copy | 1.79 | window 0.51, readback 1.28 | 0.29 / 0.29 | 8.6 / 2.1 |
| NVIDIA 580, OpenGL | copy | 0.62 | (the readback after it) | 0.52 / 0.51 | 8.9 / 8.0 |
| NVK on nouveau, Vulkan | copy | 8.79 | window 2.94, readback 5.85 | 0.34 / 0.30 | 12.7 / 2.2 |
| Intel, Vulkan (ANV) | the panel's own | 2.6-2.75 | window 2.55-2.73 | 0.43 / 0.41 | 0.20 / 0.20 |
| Intel, OpenGL (iris) | copy | 2.2-2.3 | (the readback after it) | 0.46 / 0.45 | 13.4 / 3.6 |

`lead` was 0.002 to 0.006 and `endpass` 0.001 to 0.02 in every Vulkan
row, `scene` nothing (the testbed's scene is drawn in the window's pass
at this size). OpenGL under nouveau is not in the table: the library
finds no way onto that GPU for it here (`no GPU named "nvidia" that
OpenGL can draw this window on`), `DRI_PRIME=1` or not, and draws on the
Intel GPU.

What each difference measured as, here:

1. **The submit, and the driver's threads**: nothing to speak of. The
   record+submit span is CPU throughout on both renderers, so nothing
   blocks or spins in it, and the busiest thread not the library's was
   NVIDIA's Vulkan `[vkps] Update` at 0.15 a frame; OpenGL on both GPUs
   had no busy thread but the render thread. radeonsi, on this desktop,
   is the case still owed.
2. **The start of the span**: nothing -- a bottom-of-pipe stamp 0.002 to
   0.006 after the top-of-pipe one. Phase 1's first item would change
   nothing here.
3. **The image wait**: cannot happen on a copy path, which has no image
   to wait for, so not measurable on the NVIDIA GPU on this console. On
   Intel's own panel `lead`, which a wait at the stage would hold as
   well, is 0.003; whether ANV waits there or before the batch is not
   known.
4. **Copies that are not the frame**: the whole gap. Vulkan's copy path
   puts its readback inside the span, 72% of it on NVIDIA's driver and
   66% on NVK; OpenGL's comes after its end stamp. Left out, NVIDIA's
   Vulkan is the faster at 0.51 against OpenGL's 0.62, where the ranking
   today reads 559 frames a second against 1600.
5. **The pass's end**: under a tenth of the frame everywhere; Phase 1's
   item stays out unless radeonsi says otherwise.

And one for Phase 2: **the copy path's present is CPU work.** Vulkan's
copies the frame into the scanout buffer, 2.1 of its 8.6; OpenGL's reads
it back as well, and NVIDIA's `glReadPixels` waits by spinning, 8.0 of
8.9. Ending the CPU span at the present's return, as Phase 2 has it,
would put that into the CPU half on a copy path. Whether it belongs there
is open: it is the frame's cost to the render thread, and it is a cost of
the display path rather than of the renderer.

### Measured on this desktop, Wayland (2026-10-28)

The release tree, the RX 590 on RADV and radeonsi (Mesa 26.0.8), GNOME
on DP-1 3773x2134 at 60 Hz, the testbed's window at 1280x720, its scene
moving (F2 through the portal), vsync on; harness-started, so the ranking
is off. Milliseconds a frame, the run's later lines.

| renderer | GPU span | record+submit, wall / CPU | present, wall / CPU | busy threads besides render and main |
|---|---|---|---|---|
| Vulkan | 0.16 | 0.29 / 0.29 | 0.065 / 0.065 | none |
| OpenGL | 0.20 | 0.23 / 0.23 | 0.34 / 0.21 | radeonsi's `:cs0`, 0.11-0.12 |

Vulkan's parts: `lead` 0.006, `scene` 0.002, `window` 0.14, `endpass`
0.010, `readback` 0.002. No glthread thread showed as busy.

Both renderers are bound by the CPU half here, and the ratio of the two
(0.29 against 0.23, 1.27) is the ranking file's for this display (2840
against 3615 frames a second, 1.27): the ranking's lead for OpenGL is
this one difference.

1. **The submit, and the driver's threads**: the whole gap. OpenGL's
   swap does 0.21 of CPU work on the render thread, after the span ends,
   and radeonsi's submission thread 0.11 more; Vulkan's span holds its
   submit, and its present costs 0.065. Counted to the present's return,
   as Phase 2 has it, the render thread's share is 0.35 for Vulkan and
   0.44 for OpenGL; with the submission thread beside it, as Phase 3's
   larger-of rule takes it, OpenGL stays at 0.44. Vulkan's recording is
   the dearer half on its own (0.20 against 0.10, every frame re-recorded
   while the scene moves), which is the renderer's and stays.
2. **The start of the span**: nothing, 0.006.
3. **The image wait**: no sign; RADV waits before the batch, as the plan
   expected, and `lead` holds nothing.
4. **Copies**: none on this path.
5. **The pass's end**: 0.010, under a tenth of the frame.

On the GPU the order is the other way: Vulkan 0.16 against OpenGL's
0.20, at this size.

### Measured on thinkpad's console (2026-10-28)

The release tree, KMS, LVDS-1 1366x768 at 60 Hz, the window maximized
(1366x734), its scene moving (F2 by uinput), vsync on. An i3 M 370: two
cores, four threads, lavapipe and llvmpipe at 128 bits. Neither takes the
panel, so both draw offscreen and copy each frame over, and both run at
half the refresh, 33.19 ms.

| renderer | GPU span | its parts | record+submit, wall / CPU | present, wall / CPU |
|---|---|---|---|---|
| Vulkan (lavapipe) | 19.7 | window 17.6, readback 1.8, endpass 0.21 | 0.14 / 0.14 | 32.7 / 1.8 |
| OpenGL (llvmpipe) | 17.8 | (the readback after it) | 14.2 / 1.0 | 18.8 / 3.4 |

`lead` 0.06, `scene` 0.02. The threads, a frame: on both, the four
`llvmpipe-N` rasterisers at 16.1 to 16.8 each; on Vulkan an unnamed one
(lavapipe's queue, by its share) at 3.5 and the render thread at 2.0; on
OpenGL the render thread at 4.5.

1. **The driver's threads**: the rasterisers are the software device's
   GPU, and each is as busy as the GPU span is long. Under Phase 3's
   larger-of rule they come out level with the GPU half and change
   nothing; Phase 3 should still leave a software device's rasteriser
   threads out of the CPU half by name, since they are counted already.
2. **The start of the span**: nothing, 0.06.
3. **The image wait**: none on a copy path.
4. **Copies**: the gap. Vulkan's readback is 1.8 of its 19.7; without
   it the two are level, 17.9 against 17.8. The ranking would read 51
   frames a second against 56. This desktop's software row (45 ms against
   34 at 3840x2160, eight times the pixels) fits the same readback
   scaled up, which the desktop's console run is to confirm.
5. **The pass's end**: 0.21, about 1%.

And the opposite of the spinning wait: **OpenGL's record span blocks**,
14.2 ms of wall clock with 1.0 of it on the CPU -- llvmpipe makes the
render thread wait inside the recording (where, not looked for). The CPU
half today counts that wait as CPU; at this size the GPU half is the
larger and it does not show, at a smaller window it would. Phase 2's
thread CPU time takes it out.

### Measured on this desktop's console (2026-10-28)

The release tree, booted into `fenestal-console`, KMS on DP-1 3840x2160
at 60 Hz, the window maximized (3840x2126), its scene moving (F2 by
uinput), vsync on. The RX 590 takes the output, so Vulkan draws into the
display's own images; OpenGL on KMS always draws offscreen and reads each
frame back (`egl_kms.c`), and so does lavapipe, which takes no display.

| GPU, renderer | path | GPU span | its parts | record+submit, wall / CPU | present, wall / CPU | drawn at |
|---|---|---|---|---|---|---|
| RX 590, Vulkan (RADV) | the output's own | 0.55-0.58 | window 0.52-0.54, endpass 0.025 | 0.24 / 0.24 | 2.1 / 0.49 | 60 fps |
| RX 590, OpenGL (radeonsi) | copy | 0.31 | (the readback after it) | 0.24 / 0.24 | 28 / 16.3 | about 30 fps |
| Vulkan (lavapipe) | copy | 42.3 | window 35.7, readback 6.0, endpass 0.49 | 0.09 / 0.09 | 54 / 6.5 | 18 fps |
| OpenGL (llvmpipe) | copy | 23.4 | (the readback after it) | 22.6 / 1.3 | 27.3 / 20.6 | 20 fps |

`lead` 0.004 on the RX 590 and 0.06 in software. The threads besides the
render thread: radeonsi's `:cs0` at 0.14 and one unnamed at 0.05 on
Vulkan; in software, eight `llvmpipe-N` at 16 to 35 each.

**The copy path's present belongs in the CPU half**, which answers the
question schenker left open. OpenGL's present reads the 4K frame back
and copies it into the scanout buffer on the render thread: 16.3 ms of
CPU on the RX 590, which no figure counts. It is what sets OpenGL's frame
rate here, about 30 a second against Vulkan's 60; and the ranking reads
the other way, 2698 frames a second for OpenGL against 1789 for Vulkan
(the cache's row for DP-3 at this mode; this run's figures give 3270
against 1730). So the ranking opens this console on the renderer that
draws at half the rate. In software the same: the figures say 43 frames
a second for OpenGL against 24 for Vulkan, and they draw at 20 and 18.
Phase 2, ending the CPU span at the present's return, counts it, and
"How it is checked" must take KMS's copy path in rather than leave it
out: it is where the ranking is furthest wrong.

The five differences, here:

1. **The driver's threads**: radeonsi's submission thread at 0.14, the
   rest the render thread's. The software rasterisers as on thinkpad.
2. **The start of the span**: nothing.
3. **The image wait**: RADV, drawing into the display's images, shows
   none (`lead` 0.004).
4. **Copies**: lavapipe's readback is 6.0 of its 42.3. Out of it,
   lavapipe's span is still 36 against llvmpipe's 23: its rasterisers
   are busier (35 ms each a frame against 21), which is the renderer's
   work and stays.
5. **The pass's end**: 0.025 and 0.49, about 1%.

And one not on the list: **the RX 590 takes longer to draw into the
display's image**, 0.54 against OpenGL's 0.31 into an image of its own,
where under Wayland at 1280x720 Vulkan was the faster. Not one of the
five; the scanout image's layout (no compression on an image the display
reads) is the likely reason, not checked. A cost of drawing onto the
display itself, and it stays.

## Phase 1: the same GPU span everywhere

**Built, narrowed by Phase 0, and turned round by Phase 4's check**
(DESIGN *What a frame costs, compared*). First built as the end stamp
before any readback; the check against a rate drawn showed the copy
path's readback is work that bounds the rate there, and neither half
counted it (OpenGL's was a wait in the present). So it is in both GPU
spans: Vulkan's end stamp after the copy path's readback, OpenGL's copy
path reading back into a pack buffer ahead of its end stamp. A
snapshot's copy is out everywhere: Vulkan's and D3D12's after the end
stamp, OpenGL's and Metal's frame with one not sampled. The rest of this
phase measured as nothing and is
dropped: the start stamp (0.002 to 0.06 between the two), the image
wait (none on any driver measured), and the pass's end (at most 1%).
Owed from it: D3D12's timestamp semantics on the laptops, which Phase 0
could not stamp apart (the diagnostic cuts Vulkan's span alone).

What follows is the phase as planned, kept for the reasoning.

- **Vulkan's start stamp at `BOTTOM_OF_PIPE`**, the semantics of a
  `GL_TIMESTAMP`: written once everything before it on the queue is done.
  A frame's span then starts where the previous frame's work ended,
  whichever renderer is timing. One line (vulkan/frame.c:338).
- **The end stamp before any readback**: Vulkan's above
  `record_readback()` (:449-456), D3D12's above `snapshot_record()`
  (:963). Metal times the whole command buffer, so a frame that carries a
  snapshot's blit is not sampled there; a snapshot is a move's, once.
- **The image wait out of the span (difference 3), only where Phase 0
  finds it**: the window's pass submitted as a second batch of the same
  `vkQueueSubmit` call, waiting at `ALL_COMMANDS`, each batch with its own
  stamp pair and the GPU's time their sum. The scene pass keeps running
  ahead of the image as it does now. Two command buffers per image then,
  both under the record cache (`img_cmd_gen`), and two pairs in the query
  pool. On a frame with no offscreen scene, one batch as now. The
  alternative, waiting at `ALL_COMMANDS` in the one batch, is simpler and
  gives up the overlap where the image is late; rejected unless Phase 0
  shows the overlap is worth nothing.
- **D3D12's timestamp semantics** checked on the laptops' Intel and NVIDIA
  drivers against a `Signal`-and-wait measured on the CPU. If its start
  stamp is the top of the pipe, a barrier before it, or the same
  treatment as Vulkan's.
- **Difference 5** stays as it is unless Phase 0 sizes it above a tenth of
  the frame. OpenGL cannot stamp what the swap does inside Mesa. The way
  to even it is the other side: Vulkan's end stamp before
  `vkCmdEndRenderPass`, the layout change then out of both.

## Phase 2: the render thread's CPU, not its wall clock

**Built** (DESIGN *What a frame costs, compared*): `app_frame_mark()`
in every renderer, the thread's CPU time from the end of its waits to
the present's return, `late_cost` left to the pacing; Windows' clock is
`QueryThreadCycleTime()` at a rate measured against the performance
counter, and wine 10.0's fails, which falls back to the wall clock.
D3D12's `record` ends before the submit. Brought forward from Phase 4,
since the figures changed under every entry kept: the ranking file's
version (`v2`). Checked on aspire's Windows: a 200 ms busy loop reads
193.7 ms, and the testbed's record+submit 0.29 ms of CPU in 0.30 of wall
clock. The calibration after, on schenker's console: Vulkan on NVIDIA
410, Vulkan on Intel 392, OpenGL on Intel 240, OpenGL on NVIDIA 218
(was OpenGL on NVIDIA first, 1600 against Vulkan's 559); on this
desktop's Wayland Vulkan 2565 against OpenGL's 2029 (was 2840 against
3615). Owed: the spin, unmeasured -- no X server here runs NVIDIA's
driver.

What follows is the phase as planned.


- The cost's CPU sample becomes the render thread's **CPU time** from the
  end of its waits to the return of the call that hands the frame over:
  the swap on OpenGL, `[cb commit]` on Metal, `vkQueueSubmit` and
  `vkQueuePresentKHR` on Vulkan, `ExecuteCommandLists` and `Present` on
  D3D12. A block in a wait costs no CPU, so the present call can be inside
  the span where it is also the throttle (OpenGL's swap, a FIFO present),
  and the four spans end at the same point: the frame handed over.
- The clocks, all in the system's own libraries:
  `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` on Linux and macOS (a system
  call, about a microsecond, two a frame); on Windows
  `QueryThreadCycleTime()`, cycles at the TSC's rate, turned into seconds
  by the rate of `QueryPerformanceCounter()` over the same window --
  `GetThreadTimes()` is in 15.6 ms ticks, too coarse for a frame. Wine's
  implementation of the cycle counter is checked on both wine
  environments before it is trusted there.
- **The risk is a wait that spins.** A driver whose throttle yields in a
  loop (NVIDIA's OpenGL, `__GL_YIELD`) puts the vsync wait into the
  thread's CPU. Phase 0's swap measurement on schenker answers it. If it
  spins, OpenGL gets a `glFlush()` before the swap, the span ends there,
  and the swap is left out as a wait; Vulkan and D3D12 keep the present
  call in.
- In code: a `cost_cpu_sample` beside `late_cost`, written by each
  renderer at the end of the frame, folded by `app_frame_cost()` as the
  GPU's is; `app_cost_publish()` and `rank.c`'s `sample()` read it in
  place of `late_cost`. `late_cost` keeps its wall-clock span for pacing.
- `--frame-times`: `record` ends before the submit in every renderer
  (D3D12's includes `submit()` today, d3d12/frame.c:974), so the phases
  line compares across renderers as well.

## Phase 3: the driver's own threads

**Dropped.** Phase 0 found no driver thread busier than the render
thread on any machine (radeonsi's submission thread 0.11 to 0.14 ms a
frame, NVIDIA's Vulkan 0.15) but a software device's rasterisers, which
are its GPU and already counted as its span. The larger-of rule would
have changed no figure measured.

What follows is the phase as planned.


- Threads the renderer's driver starts are a stage of the frame's
  pipeline, and for radeonsi with glthread the busiest one may be the
  slowest stage. They are found as the threads that appear while the
  renderer opens its device and draws its first frames, beside the
  library's two and the caller's, a list taken before and after; a thread
  that appears later (a move to another GPU) is added by the same
  comparison at that renderer's opening.
- Read once a `COST_WINDOW` (0.2 s), never per frame, and divided by the
  frames drawn in it: Linux `/proc/self/task/<tid>/schedstat` (nanoseconds
  on the CPU), macOS `task_threads()` and `thread_info()`, Windows a
  Toolhelp snapshot and the cycle counter of Phase 2. All the systems'
  own; nothing new on the link line.
- The CPU figure becomes the larger of the render thread's share and the
  busiest driver thread's. The sum is rejected: two threads at 0.2 ms each
  allow 5000 frames a second, not 2500.
- Off the render thread's hot path entirely, and off by default in a
  window that is not sampling (`fnl_set_cost_sampling()`) or ranking.

## Phase 4: the ranking and the public figures

**Built.** The ranking file's version came in with Phase 2;
`fnl_frame_cpu_ms()` is Phase 2's figure, Phase 3 being dropped, with
its `CHANGELOG.md` line, the header comment and DESIGN *At rest*. No
`fnl_frame_driver_ms()`: with Phase 3 dropped there is nothing for it to
say. The check against a rate drawn turned Phase 1 round (above); its
figures are in *How it is checked*.

What follows is the phase as planned.


- The ranking file gets a version in its first line
  (`# fenestal GPU ranking v2: ...`); a file without it is read as no
  figures at all, so every display is measured again under the new
  spans. Without that, old Vulkan figures would stand against new
  OpenGL ones.
- `fnl_frame_cpu_ms()` reports the Phase 3 figure, so the testbed's cost
  line and the ranking agree. Its meaning changes, which 0.9 allows
  (DESIGN *Compatibility*): a `CHANGELOG.md` line, the header comment
  rewritten to the definitions above, and DESIGN *At rest*.
  No new function, so no ABI or export-list change.
- Open: whether the render thread's and the driver's shares are worth a
  getter each (`fnl_frame_driver_ms()`), so the cost line can say which
  stage is the slow one. The F5 panel would use it. Recommended later, once
  Phase 3's figures show it would differ from the CPU half often enough.

## How it is checked

- **Against a frame rate actually drawn.** `fenestal-pace --draw
  --no-vsync` presents as fast as the device allows. For each renderer on
  one machine, the rate the cost predicts (`1000 / max(gpu, cpu)`) over
  the rate presented is a ratio. The two are not equal, since CPU and GPU
  overlap, but after this plan that ratio should be the same across
  renderers within 10%, on this desktop, schenker's three drivers, a
  Windows laptop (D3D12 against Vulkan) and the Mac (Metal against
  MoltenVK). Today it should not be, and Phase 0 records by how much.
  KMS's copy path is in it: its presents are the copy's, and that copy is
  the render thread's work a frame, the one difference that moves the
  frame rate actually drawn (Phase 0, this desktop's console).
  **Run** (`docs/benchmarks.md` *Frame costs and the GPU ranking*): this
  desktop's Wayland 1.40 and 1.35, schenker's console 1.46 for Vulkan and
  1.40 for OpenGL on Intel, and 1.01 for OpenGL on NVIDIA, which is
  CPU-bound alone; before, 1.44 to 10.2. The first run, with the copy
  path's readback out of both GPU spans, read 4.19 and 3.26 on the copy
  path, which is what turned Phase 1 round. Not run: the Windows laptop
  and the Mac, whose renderers have no copy path.
- **Against the order.** The ranking run twice with the entries in the
  opposite order gives the same figures within the same 10%, so a GPU's
  clocks rising during the first entry are not counted to the second.
  **Run** in part: two calibrations on schenker's console visited the
  entries in different orders (not the opposite) and agree within 1% but
  for Vulkan on the HD 630, 357 against 415.
- `tools/drive/pace.py` and the stress round are unchanged; the ranking's
  rounds on a kernel driver re-measure under v2, and `docs/benchmarks.md`
  gets the before-and-after table.

## What it touches

`src/renderer/{vulkan,gl,d3d12}/frame.c`, `src/renderer/metal/frame.m`,
`src/core/app.c` (`app_frame_cost()`, `app_cost_publish()`), `src/core/
rank.c` (`sample()`, the file's version), `src/core/app.h` (the new
fields), a thread-time shim beside `core/thread.h` for the three systems,
`include/fenestal/fenestal.h` (the comment), DESIGN *At rest* and
*Ranked by measured frame rate*, `CHANGELOG.md`, `docs/benchmarks.md`.
Every renderer and every backend that draws, so each phase is one commit
and a checker round on each renderer before the next. Nothing outside the
four renderers and core.
