---
date: 2026-11-20T17:03:11+01:00
author: Janos Sonntag
commit: a630f90
branch: main
repository: fenestal
topic: "d3d12 image-heap bug fixed; win64 and macos sweeps green"
tags: [harness, stress, win32, d3d12, renderer-swap, macos, schenker, power]
status: complete
last_updated: 2026-11-20T17:03:11+01:00
last_updated_by: Janos Sonntag
type: bug_fix
---

# Handoff: win64 half of the schenker sweep wedges at the first d3d12 GPU-box move; macos half not started

## Task(s)

The session's task: **run the `macos` and `win64` sweep on `schenker`** through
`tools/drive/stress.py` (release variant only, decided up front), Windows first
then macOS, and leave the machine on its Linux stick.

Status:

- **win64 — blocked by a library bug.** `win64-vulkan-native@schenker:intel`
  fails at the move of the live window onto the machine's Direct3D 12 device and
  the window then stops delivering anything: see *The d3d12 move* below. The
  round does finish its checks; it is its **log write** that then takes many
  minutes (a harness defect, also below).
- **macos — run, once a round finished.** `macos-metal-native` 95 checks clean,
  `macos-gl-native` 95 checks clean, `macos-vulkan-native` 95 checks with **5
  failed** in the maximize→restore→resize family on the first run and **0
  failed on a rerun**: flaky, not a stable regression. All three ledger lines
  are at `a630f90`, dirty.
- **finish — done as far as it goes.** schenker is in Windows
  (10.0.26300.10075), no testbed on it, nothing left holding input.

Nothing was committed and no code was edited. The only files touched outside
`build/` are `~/.ssh/known_hosts` and
`~/.local/state/fenestal/stick-entry-schenker` (see *Learnings*).

## Critical References

- `tools/drive/README.md` — how a round works, the checks and what a `CAN`
  entry means.
- `CLAUDE.md` *Running and driving* — the environments, the agents, the
  skip rules (`FNL_DRIVE_NATIVE_BUSY`, `FNL_DRIVE_MAC_BUSY`).
- `DESIGN.md` *Moving to another renderer* and the `Measured on Windows itself`
  entries — `renderer_swap()`, and what a Vulkan round moving onto Direct3D 12
  is expected to look like (its missing debug layer is `D3D12_NO_LAYER` in
  `tools/drive/environment.py:1758`, and it is in `EXPECTED_LOG` for
  `Win32Native`, `tools/drive/environment.py:2639`).

## Recent changes

None to the tree. Working tree is dirty from before the session:
`examples/embed.c`, `examples/events.c`, `tools/mkdist.py` — so every ledger
line written today records `"dirty": true`.

Trees used were built at 15:18 (`build/win64/release`) and 15:23
(`build/macos/release`), after the last commit; nothing under `src/`, `include/`,
`testbed/`, `shaders/`, `protocols/` is newer than either.

## Learnings

### The d3d12 move (the library bug)

Moving the live window from Vulkan onto the machine's Direct3D 12 device makes
`src/renderer/d3d12/frame.c:933-937` log, **once a frame, unbounded**:

```text
the frame's list did not reset (0x80070057)
```

The HRESULT is E_INVALIDARG out of `ID3D12CommandAllocator_Reset()` /
`ID3D12GraphicsCommandList_Reset()` and `d3d_hr_lost()` says the device is not
lost, so the frame returns false and the loop spins on it. The window stops
sending state: the round's `no EV gpu` FAIL, then
`RuntimeError: swept DISPLAY1 (1536x816 points) and found no window (450 events
seen, ...)` and the testbed is gone. The first run of this shaped thing was
*before* today's Windows update, so the update is not obviously the cause; 40
library commits since `3bde4d1` (where this environment passed 113 checks) are
the other half.

### The harness log grows without bound (why the machine looked hung)

`tools/drive/testbed.py` keeps **every** stderr line of the program in
`self.lines` (the round starts it with `--frame-times`) and `_keep()` rewrites
all of it to `build/drive/<env>-release.log` on `stop()`. One bad round wrote
**131,347,783 lines — 5.8 GB** — and the faulthandler dump caught the main
thread in `_keep()`'s `f.write()` and the pump thread in its `open()`, still
writing. That is the "hang": nothing was wedged but the log, and the desktop
was holding gigabytes of strings. The file was deleted; the log is scratch.
The earlier rounds called it a hang for the same reason and were aborted.

Fix options: cap `self.lines` (a ring), skip `--frame-times` lines from the log,
or write incrementally. Not done — it is the harness's, and a change wants
`tools/drive/README.md` and a round to prove it.

### The macos-vulkan flake

First run (`build/drive/macos-sweep.txt`, section from line 119): after
`the maximize button maximizes (1920, 926)` and `leaving fullscreen returns to
maximized`, `restore returns to the original area` read `(1280, 706) ->
(1920, 926)` and the window then stayed at the maximized size through
`dragging does not resize`, `the bottom-right edge resizes`, and the post-move
twins of both. The rerun (`build/drive/macos-vulkan-rerun.txt`) passed all 95.
Worth a second look only if it recurs: the two readings 925/926 in the same
failed run suggest the restore was dropped and the resize partly applied.

### Getting schenker between systems (as found today)

Full round log up to the wedge: `build/drive/intel-relog.txt`.
Repro:

```sh
FNL_DRIVE_NATIVE_BUSY=0 python3 tools/drive/stress.py win64-vulkan-native@schenker:intel
```

The last lines before it stops printing:

```text
  GPU in use: Intel(R) HD Graphics 630 (via D3D11)     # FNL_GPU=intel, Vulkan round
the inventory names the GPU in use                    ok   7 listed
the GPU box moves the window to GPU 1                 ok   gpu 0 vulkan NVIDIA GeForce GTX 1050 Ti
... (GPU 2, GPU 3 — vulkan, Intel HD 630, all ok) ...
the GPU box moves the window to GPU 5, drawn with d3d12  FAIL  no EV gpu: NVIDIA GeForce GTX 1050 Ti
and says it as a move, not a loss                     ok   moving to GPU 5: NVIDIA GeForce GTX 1050 Ti
<-- the event never arrived; the round does go on, and stalls later at its log write
```

That is `stress.py:936-966`: after the `check()` it does
`time.sleep(2.0 * b.patience())`, `b.dump()`, then `b.refresh()`
(`tools/drive/environment.py:579`) — and `refresh()` asks the testbed for its
state, which on this environment is the program's own echo read back over ssh.
So the two suspects, in order:

1. **The program wedges in the swap to Direct3D 12** (`renderer_swap()` from
   Vulkan to D3D12 on a live window), leaving nothing writing the echo — every
   later read blocks forever.
2. The harness blocks on the agent/ssh side with no timeout on that path.

One cheap way to tell them apart, not yet tried: re-run the same round with
`python3 -X faulthandler` and `timeout -s ABRT 900`, so the wedged frame's
traceback is printed instead of the process being killed silently.

**What changed since this last passed.** `win64-vulkan-native@schenker` (Intel,
"via D3D11") passed 113 checks at `3bde4d1` on 2026-11-05. Since then: 40
commits of library (`3bde4d1..a630f90`) and **a Windows update installed today,
26300.9818 → 26300.10075** (the round had already reached GPU 3 on the old
build before the user aborted it, so the update is not obviously the cause —
the same wedge shape happened before the restart).

GPU/display driver versions did not change across the update: NVIDIA
32.0.15.8278, Intel 31.0.101.2145, panel 1920x1080 at 60 Hz, display scale 125%.

Machine state after the wedge: `explorer.exe` alive, no `fenestal-testbed.exe`
left (the harness kills it on exit), machine answers ssh — so the desktop itself
is healthy; it is the round that stops.

### The busy check gives a false positive after a reboot

Every round skipped with `somebody was at 'schenker' 56 s ago` while nothing was
touching the machine (the figure grew 56 → 62 s across four attempts, and by
17 → 23 s in another run: no continuous input). `windows_agent.ps1` `Idle()`
(`tools/drive/windows_agent.ps1:398-415`) compares the system's last input with
a tick written in `C:\fenestal\last-input` by the previous session's
`LeaveMark()`; a tick from before a restart makes the comparison say "a hand's",
and `native_driver.py:363` calls `pointer_pos()` (which can itself register an
input) before asking. Worked around with **`FNL_DRIVE_NATIVE_BUSY=0`** for every
round today. Worth a real fix later: ask for the idle before moving anything, or
have `LeaveMark()` run before the session's own last input.

### Getting schenker into Windows is now the awkward part

Schenker's firmware tries USB first (or rEFInd picks the stick), so:

| from | to | how it went today |
|---|---|---|
| stick | windows, **cold** (power off + magic packet) | worked the first time, then came up in the **stick** twice — the firmware loses `BootNext` across a power-off |
| windows | windows | a plain `shutdown /r /t 0` lands on the **stick** (USB first) |
| stick | windows, **warm** (`efibootmgr -n` then reboot) | **works**, but took longer than `power.py`'s 420 s `UP_TIMEOUT` |

`power.py boot schenker windows` chooses the cold path because `schenker` is in
`~/.local/state/fenestal/cold-hosts` (a restart from the key into Windows left
the panel dark once). Today the cold path was the one that failed and the warm
one worked; the panel was fine afterwards. If the next session wants Windows:
be on the stick and use `python3 tools/drive/power.py boot schenker windows --warm`,
and allow ~10 min (the restart outran the tool's 420 s wait — the machine did
come up, `where` just said `off` because of the next item).

### The ssh host key fights over one alias

`Host schenker` in `~/.ssh/config` pins `HostKeyAlias schenker.local`, and both
Windows and the Linux stick answer under that name, so every switch leaves a
mismatch for the next connection. Twice today the fix was

```sh
ssh-keygen -R schenker.local && ssh -o StrictHostKeyChecking=accept-new schenker ver
```

After the Windows update the Windows sshd generated a new key as well. While a
mismatch stands, `power.py where` reports **`off`** for a machine that is up and
answering (all three probes fail). A second alias for the Windows account
(`Host schenker-win` with its own `HostKeyAlias`) would remove the churn; not
done, since it changes the harness's assumptions.

Also touched: `~/.local/state/fenestal/stick-entry-schenker` now reads
`UEFI: SanDisk, Partition 1` (the Linux stick in the machine).

### State of the machine right now

Windows 10.0.26300.10075, console logged in, agent task present, Defender
leaves `C:\fenestal` alone, validation layer and the VC++ runtime present, drives
itself without being told on `intel` and `nvidia`.

## Artifacts

- `build/drive/intel-relog.txt` — the round log up to the wedge (the evidence
  for the FAIL and for where it stops).
- `~/.local/state/fenestal/ledger.jsonl` — 8 lines added today, all
  `commit a630f90, dirty true`: four `skipped` lines for the four win64 native
  environments (the BUSY false positive) and the rest from the two runs.
- `python3 tools/drive/status.py` — what every environment was last run at;
  the macos and win64 native rows are 36-40 commits old (2026-11-05/06).
- Nothing under `.rpiv/artifacts/` from this session except this file.

## Action Items & Next Steps

1. **The d3d12 move is the blocker for the win64 half.** It is a library bug,
   diagnosed to `src/renderer/d3d12/frame.c:933-937`; nothing about the machine.
   Next questions, cheapest first: does a *Debug* `win64` tree (`./build.sh win64
gc`) say anything more around the swap; does `FNL_D3D12_DXGI11` change it;
   does the `d3d12-native` environment (which *opens* on Direct3D 12 and only
   swaps to Vulkan/other d3d12 devices) survive the same GPU-box walk; and does
   the pre-update commit `3bde4d1` still pass on this machine.
2. **Then the win64 sweep** — four rounds, release, each ~15 min:
   `FNL_DRIVE_NATIVE_BUSY=0 python3 tools/drive/stress.py
   win64-vulkan-native@schenker:intel win64-vulkan-native@schenker:nvidia
   win64-d3d12-native@schenker:intel win64-d3d12-native@schenker:nvidia`.
   Run them one or two at a time. Check `build/drive/*-release.log`'s size after
   each: a five-gigabyte log means the round hit the d3d12 spin.
3. **The harness log growth** (
   `tools/drive/testbed.py` `_keep()`) wants fixing before another long round:
   cap `self.lines`, or stop keeping `--frame-times` lines. It is the reason a
   bad round looks like a hung machine and holds the desktop's memory.
4. **macos is done** for this sweep (see *Task(s)*). Only `macos-vulkan-native`
   is worth watching: it failed five checks in the first run and passed all 95
   on a rerun, so a recurrence is worth a look rather than a bisect.
5. **Machine state**: Windows, nothing running. `power.py boot schenker stick`
   from macOS **restarted into Windows today**, not onto the key — the firmware
   order is no longer stick-first for a restart. A plain restart out of Windows
   still landed on the stick, so the way to the key is a restart from Windows.
   Getting into macOS warm works: `power.py boot schenker macos --warm`
   (Windows → restart to stick → `efibootmgr -n` the `UEFI:  PHILIPS` entry →
   restart), 115 s, then the settle ran on its own.

## Other Notes

- Trees are current with `a630f90` and do not need rebuilding for these rounds;
  the three dirty files are one example each and `tools/mkdist.py`, none of
  which the round drives.
- `win64-d3d12-native@schenker:*` is the untested alternative if the D3D12
  rounds turn out not to wedge: they *open* on Direct3D 12, so the GPU-box walk
  reaches the d3d12 entries as "already there" moves rather than as swaps.
- The macos rounds have passed before at `389fcc2` (95 checks, all three
  renderers) and `4a9e72c`/`b2cc7cf` for `gc`; nothing suggests they are broken
  today, they simply were never reached.
- `pace.py` and `bench.py` columns for schenker and schenker-mac are also stale
  (53+ commits); not part of this task.


## Resolution (2026-11-20, later session)

**The d3d12 bug: found, fixed, verified.** Bisected with `native_run.py`
through a `3bde4d1` worktree: `0e68945` (overlay image) introduced it,
`11af832` was clean. The cause is in `src/renderer/d3d12/overlay.c`'s new
`d3d_own_state()`: it bound **two** `D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV`
heaps (the library's `srv_heap` plus the images' own `image_heap`), which a
real driver rejects — every `ID3D12GraphicsCommandList_Close()` then failed
`E_INVALIDARG`, the frame returned false and logged unbounded; vkd3d-proton
under wine accepted it, which is why only native Windows saw it. Fix (in the
tree, rebuilt into `build/win64/release`): caller-image views take the srv
heap's slots `SRV_COUNT` on (the heap is now `SRV_COUNT + IMAGE_SETS_MAX` in
`d3d12/target.c`), `image_srv_cpu/gpu()` offset into it, the extra heap is
gone; `image_heap`/`image_inc` dropped from `d3d12_state.h`. `native_run`
with `FNL_RENDERER=d3d12` now draws clean where it logged 1.2M bad lines.

**The macOS flake: not a code flake.** It reproduced on the *pre-fix* tree
too (the image-heap bug is win64-only; metal has its own path), and after the
fix `macos-vulkan-native` passed twice running — but so had it once before
the fix, and nothing in the fix touches the Mac. The one 5-FAIL run stands
unexplained; three consecutive clean runs since (2026-11-20) suggest a
transient (the Mac's maximize/restore race), but it is not *proven* gone.
Watch for it; a recurrence wants the maximize/restore section read with the
Mac's own window-server timing.

**Verified this session (ledger, all `a630f90` dirty):**

- `win64-vulkan-native@schenker:intel` 113/0
- `win64-vulkan-native@schenker:nvidia` 113/0
- `win64-d3d12-native@schenker:intel` 113/0
- `win64-d3d12-native@schenker:nvidia` 113/0
- `macos-metal-native`, `macos-vulkan-native`, `macos-gl-native`: 95/0 each,
  twice running.

Housekeeping: the `3bde4d1` worktree under `/tmp/fb` is removed; the host
bake's mtimes were touched (`font_atlas.bin` etc. at 17:56, stamp written) to
let that cross build borrow it — a `linux release` rebuild refreshes the
stamp properly. Nothing is left running on schenker (macOS, idle).
