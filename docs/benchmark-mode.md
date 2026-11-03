# Plan: write `docs/benchmark-mode.md`, the benchmark mode's design

## Context

The user wants `fenestal-testbed` to grow a **benchmark-only mode**: a run
that measures every useful figure (frame rate, the frame's phases, input
latency, the process's CPU, RAM and VRAM, and figures to come), where every
optional aspect (run length, background CPU load, thread affinity, ...) is
selectable from the program's own UI, and whose output is a standardised
record that can be stored, compared and visualised reproducibly.

**This turn's deliverable is one new document and nothing else:**
`docs/benchmark-mode.md`, the detailed plan for that mode. No source, build
file, DESIGN.md, TODO.md or CHANGELOG.md edit (the user: "Do not touch
anything in this tree, except writing a new document"). `docs/` is packed
whole by `tools/mkdist.py`, so no manifest row; `.md` is not style-checked
but the house style applies: 80 columns, no tabs, British spelling, `/* */`
in any C shown.

Decisions the user made this session (AskUserQuestion):
- **Library API additions are in scope** for the plan: minimal, each with
  the ABI baseline, a snippet, a CHANGELOG line and a DESIGN section. The
  mode's first phase must still work with today's getters alone.
- **The option UI is mouse-first through a new public library API**, not a
  keyboard-only panel and not a tall custom titlebar. Keys stay as the
  harness's way in.

## Verified facts the document rests on

Verified against commit 8835063 (2026-10-25), before the benchmark mode was
built; DESIGN *Benchmarking* is what was built. The citations name the same
code in the tree as it stands (2026-11-03), and a **Now:** line under a fact
that no longer holds says what does.

- The testbed has no timing code; every figure is a getter: `fnl_fps()`
  (0.2 s window), `fnl_frame_gpu_ms()`/`fnl_frame_cpu_ms()` (EMAs 0.9/0.1,
  published each 0.2 s), `fnl_input_latency_ms()`; `fnl_set_cost_sampling()`
  keeps them live at rest. Events are drained only in `demo_overlay`
  (testbed/main.c:606). `--no-vsync` sets `FNL_VSYNC=0` before
  `fnl_init` (main.c:360). Scene: two fixed draws, no load knob; motion is
  the library's F2 alone (`App::scene_motion`, src/core/app.c:2014).
  **Now:** the testbed has the benchmark mode (`testbed/bench*.c`); events
  are drained at the top of `on_record` too (`demo_drain()`,
  testbed/main.c:572, called at main.c:591); F2's flag is public as
  `fnl_set_scene_motion()` and `fnl_scene_motion()`.
- The phase split exists only as `--frame-times` text every 0.5 s
  (`FRAME_TIMES_WINDOW`, src/core/frame.c:112; header, usage and DESIGN say
  "once a second": stale), declared unstable (fenestal.h:48). The `fc_*`
  sums are **windowed and gated** on `a->frame_times` (frame.c:1527, 1923,
  1956, 2028, 2055, 2078, 2151, 2168, 2221; reset at 1513-1517 and
  1725-1729), so a cumulative stats API needs fields and a gate of its own.
  **Now:** that API is `fnl_set_frame_stats()` and `fnl_frame_stats()`, and
  the sums are gated on `a->timing`, `frame_times` or the caller's counting
  (frame.c:1838); the usage text says half a second.
- `App::out_refresh_mhz` (app.h:1971) is already published by X11 (RandR,
  src/backend/x11/x11_ext.c:832), Cocoa (cocoa.m:371) and KMS, read by the
  pacer (frame.c:1043); Wayland discards `r` in `output_mode`
  (wayland.c:300-303), Win32 never publishes. A public getter is cheap.
  **Now:** Wayland publishes it (wayland.c:97) and so does Win32
  (win32.c:405), and the getter is `fnl_output_refresh_mhz()`.
- GPU timestamps are durations only (`app_gpu_time(a, seconds)`,
  app.c:2341; readers vulkan/frame.c:616, gl:452, d3d12:326, metal.m:354).
  No present-to-display stamp anywhere: KMS reads only `drm_event.type`
  (kms.c:1707), D3D12 calls `GetFrameStatistics` for `PresentCount` alone
  (d3d12/frame.c:420), present_wait gives "shown" not "when", no
  wp_presentation protocol, no CAMetalDisplayLink.
  **Now:** `app_gpu_time()` takes the frame's index too, and a frame's
  shown stamp comes from Wayland's `wp_presentation` (wayland.c:1503),
  X11's Present completions (x11_ext.c:339), KMS's vblank event
  (kms.c:1716), D3D12's `SyncQPCTime` (d3d12/frame.c:655) and Metal's
  `presentedTime` (metal/frame.m:311), handed out by `fnl_frame_shown()`.
- Capture-on-click: `app_pointer_button()` queues `FNL_EVENT_BUTTON` first,
  then `deco_press()` (app.c:1736-1761); `deco_press()` ZONE_CONTENT ->
  `capture_pointer` (decor.c:346-349). Claims (`fnl_titlebar_claim`,
  api_overlay.c:387-398; `App::claims`/`pub_claims`, 16, swapped whole under
  `App::lock` in draw.c:485-489 **only when `ds->custom_bar`**) are scanned
  only in the bar band (`deco_zone_at`, decor.c:164-169); `deco_hit()` ends
  `return ZONE_CONTENT` (decor.c:255). ZONE_CLIENT_ITEM already gets the
  caller's cursor (`deco_cursor_is_callers`, app.h:2201) and HTCLIENT on
  Win32 (win32.c:998); Cocoa's `acceptsFirstMouse` is `!= ZONE_CONTENT`
  (cocoa.m:625-629). Controls (src/core/controls.c) are internal.
  **Now:** the swap runs every frame, whoever owns the bar (draw.c:485-489),
  and `claim_at()` (decor.c:140) is asked in the content too: `deco_hit()`
  ends `return claim_at(a, x, y) ? ZONE_CLIENT_ITEM : ZONE_CONTENT`.
- Lifetimes repeat in one process (tools/drive/lifecycle.c `one_lifetime`,
  20 of them); every env read is inside `fnl_init()` (app.c:810, 928, 2612,
  2690), so a `setenv` between lifetimes is honoured.
- Since 2026-10-26 the testbed's F1 panel reads process CPU and RSS
  (`testbed/proc.c`, one file with the three systems' branches)
  and VRAM per renderer (`scene_<api>_vram()` in each `scene_<api>.c`,
  which stays the one file naming its API), so the `bench_proc_*` and
  `bench_gpu_*` files below are those instead. Nothing sets affinity. The
  library's threads: render (app.c:2892), lister (2565), mover
  (drivers.c:396), Win32 raw input (win32_rawin.c:304), Vulkan reaper
  (swapchain.c:725): all created from the main or render thread, so a
  process mask set before `fnl_init()` is inherited by all.
- Vulkan instance enables `VK_KHR_get_physical_device_properties2` where
  offered (vulkan/device.c:263-267); `VK_EXT_memory_budget` is a device
  extension the library does not enable, but physical-device queries are
  spec-legal without enabling.
- Records: ledger JSONL (`tools/drive/ledger.py` `record(instrument, env,
  variant, **what)`), `status.py`; `pace.py` scrapes fps from log text and
  samples CPU from /proc; `docs/benchmarks.md` is hand-written; the harness
  reads **stderr only**, stdout is drained to nowhere (testbed.py:160-172);
  the remote drivers fetch only screenshots by scp (native_driver.py:485,
  mac_driver.py:420), the VM has no scp path, but every driver relays
  stderr. No JSON writer in C; no plotting under tools/; Python in tools/
  must pass ruff (80 col) and mypy.
- ABI: `tools/abi/check.cmake` byte-compares `.abi` (layouts, enum values;
  from `tools/abidump.c` `S()`/`F()` lines) and `.syms` (exports; three
  files: `fenestal-0.syms`, `-gl.syms`, `-all.syms`). A new `FNL_API` needs
  a comment above it, a thread tag, and a caller in `examples/`
  ("Shows: ...") or `build.sh` fails (`tools/mkapidoc.py`).

## The document: `docs/benchmark-mode.md`

Shape: the project's own (a `#` title; a status paragraph with the date
2026-10-25 and commit `d50298e`, saying it is a plan, nothing of it is
built, and that what it owes moves to TODO.md and what it measures to
DESIGN.md as each lands); `##` sections with bold-led paragraphs; tables for
the option set, the metric catalogue, the schema and the phases; file:line
references into the tree as it is today. Roughly 700-900 lines. Sections:

1. **What it is, and is not.** One instrument across the 26 environments,
   with an application in the frame (`fenestal-pace` measures the library's
   floor, DESIGN *Frame pacing*); not a replacement of `pace.py`/`stress.py`;
   a record, not a log line. Reproducible = every option recorded, each run
   a cold lifetime, a run replayable from its own file.
2. **What exists today.** The inventory above, in prose with references:
   the four getters, the frame-times text and why it cannot be the
   instrument, pace.c/pace.py, the ledger and benchmarks.md, and the gaps
   (no phase struct, no per-frame history, no process/VRAM figures, no
   present stamps, no public refresh rate, no scene-motion setter, no
   click-through in the content area).
3. **The shape of the mode.** `--bench` (setup panel), `--bench-run` (start
   at once, for the harness), `--bench-<key>=<value>` for every option,
   `--bench-replay=<file>`, `--bench-out=<path>`, `--bench-quit`,
   `--bench-emit`, `--bench-list`. Precedence: defaults < replay file <
   command line < panel. `--custom-titlebar` refused with `--bench`.
   **Lifetimes:** SETUP (panel, library defaults) -> RUN (env + process
   affinity/priority set, `desc = base + INIT options`, `fnl_init`, driver
   thread, `fnl_run`, join, `fnl_shutdown`, write the file) -> RESULTS
   (panel: rerun / back / quit); repeats are RUN again; `fnl_run` stays on
   the init thread. Pattern: tools/drive/pace.c (own thread, `fnl_quit()`).
4. **The options: one table, four consumers.** `BenchOption {key, label,
   kind BOOL|INT|ENUM|TEXT, min, max, step, default, names, when
   INIT|RUN|RECORD, page, help}` drives the command line, the panel, the
   JSON `options` object and replay; `--bench-list` prints it and is the
   self-check. The full table (pages Run / Window / Device / Load / Record):
   `label`, `settle-s` (4), `warmup-s` (2), `measure-s` (20), `cooldown-s`,
   `repeats`, `interval-ms` (1000), `frames` (keep per-frame rows), `out`,
   `emit`, `quit`; `size` enum + `width-pt`/`height-pt` (INIT; KMS shows
   `mode` from `fnl_mode_get()` instead), `fullscreen`, `scale` (INIT
   `scale_override120`), `vsync` (INIT `FNL_VSYNC`), `late-latch` auto/off/
   fixed + `late-latch-ms`, `antialias` (dim where `!fnl_offscreen_supported`),
   `scene-scale`; `backend`/`renderer`/`gpu` (INIT; `auto` leaves the
   harness's `FNL_BACKEND`/`FNL_RENDERER`/`FNL_GPU` standing), `scene-motion`
   (phase 2 setter), `scene-load` (1-256 star draws), `redraw` every-frame /
   motion-only / at-rest, `cost-sampling`, `transition-ms`; `load-threads`,
   `load-duty`, `load-affinity`, `load-priority`, `process-affinity` (INIT,
   n/a on macOS), `priority` (INIT); `library-log`, `frame-times` (INIT),
   `notes`. INIT rows re-init; RUN rows apply at the run's first
   `on_overlay` or before `fnl_run()` where `[any]`; RECORD rows change only
   the file.
5. **The panel.** Mouse-first on `fnl_claim()` (section 10): each visible
   row claims one rectangle; hover from `FNL_EVENT_MOTION`, left click on a
   row/arrow/box, wheel steps the hovered row, right click cycles back; the
   pointer's shape over a claim is the caller's (`fnl_set_cursor`). An open
   drop-down claims the whole content rectangle, so the dismissing click
   reaches the panel and captures nothing (the library's own modality rule,
   DESIGN *The titlebar controls*, kept on the caller's side). Every control
   has a key: Up/Down, Left/Right/Enter, Space, PgUp/PgDn, Tab pages, F5
   start, Esc back/abort, F3 text entry (`fnl_set_text_input`, as
   overlay.c:425), R/B/Q on the results panel. Widgets: checkbox, spinner,
   drop-down, text; drawn with the overlay primitives in the order the rules
   require (rects, strokes, text). Results panel: the summary rows, the path
   written, the disqualifiers.
6. **The metrics.** A catalogue table: key, unit, source, availability
   (today / phase 2 / phase 4 / later; per platform). Groups: `frame.*`
   (rate from the library's 0.2 s window **and** the bench's own present
   count per interval, interval distribution from `on_overlay` entry
   stamps, 1 % and 0.1 % low, gpu_ms, cpu_ms, on_record/on_overlay
   durations, presents/recorded counts), `input.*` (latency from the getter
   while the harness or a hand provides input, else null; events count),
   `process.*` (cpu %, user/sys s, rss, peak rss, threads, per-thread times
   with names), `gpu.*` (VRAM used/budget via the interop handles: Vulkan
   `VK_EXT_memory_budget` through `vkGetPhysicalDeviceMemoryProperties2KHR`
   gated on `vkEnumerateDeviceExtensionProperties`; D3D12
   `EnumAdapterByLuid` + `QueryVideoMemoryInfo`; Metal
   `currentAllocatedSize`; GL `NVX_gpu_memory_info`/`ATI_meminfo` optional),
   driver identity (`VkPhysicalDeviceDriverProperties`, DXGI adapter desc,
   OS build, `GL_VERSION`), `library.*` (phase means from two
   `fnl_frame_stats()` snapshots, gpu idle %, missed cycles, period, hold;
   null in phase 1), and **later**: `frame.present_interval_ms` and
   input-to-photon from present stamps (key reserved), power/thermal.
   Rule: unknown or not applicable is `null`, never 0.
7. **The run.** Driver thread owns settle -> warmup -> measure -> cooldown
   -> done (atomics the callbacks read); `bench_on_record()` wraps the scene
   (duration, `demo_scene_set_load`); `bench_on_overlay()` is the one drain,
   stamps the interval, applies RUN options on its first call, samples VRAM
   (GL must be inside a callback, fnl_gl.h:22-24), `fnl_invalidate()` under
   `every-frame` (pace.c's `--draw`; DESIGN *At rest*: the window never
   rests). Interaction with the 0.2 s cost window and the three redraw
   scenarios (at-rest reads `fnl_fps()` as the 5 Hz sampling rate and says
   so). Percentiles: nearest rank over a capped `double` array (1<<20);
   1 % low = 1000 / mean of the slowest ceil(n/100) intervals. Clocks: the
   bench's monotonic clock (pace.c:90-139), offset to `FnlFrame::time`
   recorded at the first callback. **Disqualifiers** (status `invalid`,
   named): focus lost, unrequested WINDOW/RESIZE/SCALE/OUTPUT, GPU event,
   `on_gpu_lost`, `FNL_EVENT_LOST`, a sampling gap > 3 intervals, present
   mode changing, fps 0 under every-frame; **notes** (not disqualifiers):
   present mode not matching the vsync asked, zero inputs, VRAM unsupported,
   missed cycles unavailable on X11/Win32.
8. **Load and affinity.** `bench_load.c`: n threads, duty-cycled busy/nap
   over 10 ms slots, started at warmup, stopped after cooldown. Per OS:
   Linux `pthread_setaffinity_np`, `sched_setaffinity(0)` before `fnl_init`
   (inherited by every library thread), `setpriority`; Windows
   `SetThreadAffinityMask`, `SetProcessAffinityMask` before init,
   `SetPriorityClass`; macOS none (rows read n/a, JSON null; the library's
   `THREAD_TIME_CONSTRAINT_POLICY` noted). Record what was achieved, not
   what was asked.
9. **The record: `fenestal-bench/1`.** One JSON document per run; keys end
   in their unit (`_ms`, `_us`, `_fps`, `_pct`, `_kib`, `_mib`, `_hz`,
   `_n`); `metrics.*` is `{"value": x}` or the seven-field distribution
   `{n, mean, min, p50, p95, p99, max}`; `run` (id, series, repeat,
   started/finished UTC, label, program, library_version, commit, dirty,
   variant, status complete|aborted|invalid, invalid_reasons, notes_events),
   `machine`, `platform`, `options` (every key, defaults included),
   `metrics`, `series` {columns, rows}, `threads`, `frames` (null or
   {columns, rows}), `library_log`. Additive versioning: a renamed key is a
   new key, readers ignore unknown keys; `tools/bench/schema.json`
   normative. An example document (~60 lines) in the text. File name
   `bench-<UTC stamp>-<host>-<backend>-<renderer>-<gpu slug>-<id>.json`;
   default dir `$XDG_STATE_HOME/fenestal/bench` (the ledger's state dir),
   `%LOCALAPPDATA%\fenestal\bench`, `~/Library/Application Support/
   fenestal/bench`; one stderr line always (`bench: <status> ... -> <path>`;
   stdout is lost under the harness and the Win32 GUI subsystem); `--bench-
   emit` writes the document between marker lines on stderr, which is how
   results come home from every remote machine, the VM included.
10. **Library additions**, each with the ritual (header comment + thread
    tag, implementation file, `.syms` x3 and, for a struct, `.abi` x2 +
    `tools/abidump.c` lines, snippet, CHANGELOG line, DESIGN section):
    - `fnl_claim(app, x, y, w, h)` `[overlay]`, phase 1a: the claim seam
      extended to the whole window. `fnl_titlebar_claim()` becomes this call
      bounded to the band; shared `claim_add()` in api_overlay.c;
      `TITLEBAR_CLAIMS_MAX 16` -> `CLAIMS_MAX 64` (and the assert's text);
      draw.c publishes the list unconditionally; `deco_hit()` tests the
      published claims before `return ZONE_CONTENT` -> ZONE_CLIENT_ITEM
      (no capture, no drag; inert over the library's own bar, which
      `deco_zone_at` answers first). Cursor and HTCLIENT already follow.
      Why not export the controls (they are the bar's layout and the
      library's knobs; DESIGN *The titlebar controls*' rule) and why not an
      opt-out of capture (the benchmark wants a panel **and** a captured
      run in one window; DESIGN *Pointer capture* "one way in" narrows to
      "the unclaimed area", as *Replacing the titlebar* narrowed the band).
      Cocoa `acceptsFirstMouse` then lets a first click reach a claim, as
      the bar's buttons already do. Snippet `examples/panel.c`.
    - `fnl_set_scene_motion(app, bool)` / `fnl_scene_motion(app)` `[any]`,
      phase 2: the F2 flag made public (app.c:2014), with the wake
      `fnl_invalidate()` does.
    - `fnl_set_frame_stats(app, bool)` + `fnl_frame_stats(app,
      FnlFrameStats *)` `[any]`, phase 2: cumulative counters the caller
      diffs (turns, frames, presents, rested; pace/fence/acquire/record/
      submit/present/rebuild/draw µs; gpu µs + samples; gpu idle; cycles/
      missed; rebuilds; device losses; inputs, stamped, input µs, longest;
      period, hold; events lost), size-first struct, prefix-copied by the
      caller's `size`; counted on the render thread at the sites the
      frame-times line uses, published with `app_publish_window()` under
      `App::lock`. The setter widens the `a->frame_times` clock gates to
      `frame_times || stats_on` (symmetry with `fnl_set_cost_sampling()`;
      off, nothing is read). Header: the text line is the diagnostic, this
      is the instrument. `abidump.c` `S()/F()` rows; both `.abi` files.
    - `fnl_output_refresh_mhz(app)` `[any]`, phase 4: `out_refresh_mhz`
      read; Wayland keeps `r` in `output_mode`, Win32 reads
      `QueryDisplayConfig`'s `vSyncFreq`.
    - Later: `fnl_frame_samples()` draining a per-frame ring (needs a frame
      index on `app_gpu_time()` and the four readers); present stamps per
      backend (KMS flip `tv_sec/tv_usec` first, then wp_presentation + wl_dyn
      rows, DXGI `SyncQPCTime`, Metal `presentedTime`) appended to the
      sample. Not proposed: `FnlInitDesc` affinity fields (the process mask
      suffices).
11. **Tooling (phase 4).** `tools/drive/bench.py` mirroring pace.py's
    measure/native/bare split, running `--bench-run --bench-quit
    --bench-emit ...` with the environment's RUNNER/environ/stage, cutting
    the document from stderr, validating, writing the file, recording
    `ledger.record("bench", env, variant, file=..., device=...)`;
    `status.py` learns the row; `tools/bench/report.py` (N files ->
    Markdown in benchmarks.md's shape, `--html` with inline SVG, `--csv`,
    `--compare a b`, grouping repeats by `series` with mean ± sd and n = 1
    flagged "indication"); `tools/bench/check.py` (types, required keys,
    unit suffixes; no dependency); `schema.json`. A latency scenario where
    bench.py's hands drive the window during measure.
12. **Files.** New under testbed/: `bench.h`, `bench.c` (entry,
    lifetimes, driver thread, hooks, write), `bench_options.c`,
    `bench_panel.c`, `bench_json.c` (writer + flat reader), `bench_stats.c`,
    `bench_load.c`, `bench_proc_{linux,win32,macos}.c` (one per OS),
    `bench_gpu_{vulkan,gl,d3d12}.c` + `bench_gpu_metal.m` (under the
    `SCENE_HAS_` loop). Changes: main.c (`--bench*` ahead of the unknown
    option, callbacks template built before init, `demo_bench_main()`),
    demo.h (four prototypes + `demo_scene_set_load`), each `scene_*` draws
    the star `load` times, CMakeLists.txt testbed block (per-OS and per-
    renderer source lists). **No new link entries**, with the exact calls:
    Linux libc/pthread; Windows kernel32 incl. `K32GetProcessMemoryInfo`
    by name, `RtlGetVersion` by `GetProcAddress`, `__cpuid`; macOS
    libSystem/Mach/Foundation inside Cocoa; DXGI only where D3D12 is
    built; Vulkan through the loader the testbed already links.
13. **Phases and their checks.** 0 this document. 1a `fnl_claim` (build.sh
    linux + clang for `-Wthread-safety`; ABI/snippet; stress rounds
    unchanged; `--custom-titlebar` still works; the snippet by hand with
    `--echo-events` showing no `capture`). 1 the mode on today's getters
    (by hand on X11/Wayland/wine; `--bench-run --bench-measure-s=5
    --bench-quit --bench-emit`; `check.py`; replay prints the same options;
    asan/tsan trees for the new thread and mutex). 2 `fnl_frame_stats`,
    scene motion (phase means agree with the frame-times lines of the same
    run; pace.py before/after shows the gates cost nothing). 3 load,
    affinity, VRAM, driver identity, re-init, replay (win64 under wine and
    native laptops via `native_run.py`, Mac, the Pi for ARM /proc and
    affinity; 20 repeats on X11). 4 bench.py/report.py/check.py, ring,
    refresh (ruff + mypy clean; three files + ledger lines; the generated
    table becomes a section of benchmarks.md). 5 present stamps (KMS on a
    bare machine first, then Wayland).
14. **Risks and open questions**, each with its recommendation: Win32 GUI
    subsystem and stdout; results from remote machines (emit on stderr; a
    scp `fetch()` later for frame-heavy runs); KMS cannot resize (mode row
    instead); software GPUs (valid, VRAM null, as pace's bare column);
    wine (`window_host` recorded, never compared against Windows without
    the column); GPU moves (disqualifier until a move scenario); clocks and
    the offset; name collisions and repeats (`series`); desktop variance;
    the 0.2 s window vs 1 s samples (both recorded); the stats gate's cost
    (measure once); the 64-claim scan under `App::lock` (microseconds);
    input latency needs input; `VK_EXT_memory_budget` without enabling and
    the validation layer in checker rounds (`EXPECTED_LOG`).
15. **Where it lands.** The DESIGN sections to write (*Benchmarking*, the
    zone-table row in *Replacing the titlebar*, the sentence in *Pointer
    capture*, a paragraph in *Where the time actually goes*, *macOS* on
    `acceptsFirstMouse`), the TODO.md entries (one per phase not yet done),
    CHANGELOG lines, the testbed option rows in DESIGN *The testbed*
    and README, and the stale "once a second" to fix in passing.

## Verification (of the document itself)

- Read it back for 80-column wrap, no tabs, British spelling, no `//` in C
  excerpts, and that every file:line reference names what the text says
  (spot-check with `sed -n`).
- `git status` shows exactly one new untracked file, `docs/benchmark-mode.md`.
- No build, run or capture: not asked for.
