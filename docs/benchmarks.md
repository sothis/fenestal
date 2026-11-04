# Benchmarks

The figures that compare the environments with each other, in one place: the
stress round's result for every environment and device, and `pace.py`'s frame
rates and CPU across every platform the harness reaches. Each table is one
sweep, so it indicates; two sweeps that agree are a measurement, and those
go in `DESIGN.md` with their reasoning. Every number below is from
2026-10-31 at commit `b4571d7`, release builds; the laptops on Windows
ran that day on the tree that became `b4571d7`, the same code before it
was committed. They are replaced as a set: rerun everything, then change
the commit and date above. The one exception is a table that carries a
date of its own, a sweep of one kind of environment run since or not run
again.

```sh
python3 tools/drive/stress.py                       # every environment
MESA_VK_WSI_DEBUG=noshm python3 tools/drive/pace.py # bare columns need noshm
FNL_DRIVE_BARE=schenker,aspire FNL_DRIVE_BARE_DRIVER= FNL_GPU=intel \
    MESA_VK_WSI_DEBUG=noshm python3 tools/drive/pace.py x11-vulkan-bare \
    wayland-vulkan-bare kms-vulkan-bare             # then FNL_GPU=nvidia
```

A Windows laptop's columns are one a GPU, as `native-hosts` lists them;
`FNL_DRIVE_NATIVE` set to bare names would replace that list with one
column a machine. The NVK columns are the bare ones run again with the
key booted on its nouveau entry (as root there, `fenestal-boot once
fnl-$(uname -r)-plain`, then a reboot). The order of a sweep is every
other machine first and this desktop last, since its rounds hold its
input; the bare machines run side by side, one harness each, since each
round's forwarded display takes a port of its own. `tools/drive/status.py`
has the latest of each from the ledger, with how many commits old it is.

## The machines

| name | CPU | GPU(s) | system |
|---|---|---|---|
| desktop | i7-2700K | RX 590 (RADV, Mesa 26.0.8) | Ubuntu, kernel 7.0, GNOME; wine 10.0 |
| aspire | i7-6700HQ | HD 530, GTX 960M | live key (Arch, kernel 7.2.11, Mesa 26.2.6, NVIDIA 580.196.07) or Windows 26H2 |
| schenker | i7-7700HQ | HD 630, GTX 1050 Ti | live key (as aspire's), Windows 26H2 or macOS 26.7.1 |
| thinkpad | i3 M 370 | none Vulkan drives: lavapipe | live key (Arch, Mesa 26.2.6) |
| win10, win11 | the desktop's, in VirtualBox | lavapipe, WARP | Windows 10 and 11 guests |
| pi4 | Cortex-A72, 8 GB | V3D 4.2 (V3DV, Mesa 26.2.6) | Raspberry Pi 4, Arch Linux ARM, kernel 6.18.58, `linux-arm64` tree; no monitor, HDMI-A-1 forced on |

The live key boots NVIDIA's 580 driver by default and NVK on its nouveau
entry. Windows is build 26300.9818 with NVIDIA's 582.78 (32.0.15.8278)
and Intel's 31.0.101.2115 (HD 530, WDDM 2.1; DESIGN *Benchmarking*) and
.2145 (HD 630); aspire has Windows' Graphics Tools, the Direct3D 12 debug
layer. schenker goes into Windows and macOS from a cold start
(`cold-hosts`). Every panel is 60 Hz.

## Stress round

One release build of each tree, driven in every environment the harness
reaches: the bare machines on their keys, schenker and aspire booted once
on each of the key's two NVIDIA entries; both laptops on Windows, each
GPU; schenker on macOS; the two VirtualBox guests; this desktop. 88
rounds, every one passing at its first run but one, rerun below.
`--capture` was off, so the picture checks are among the not
applicable, and the bare rounds had `MESA_VK_WSI_DEBUG=noshm`. Each cell
is the device, then checks / failed / not applicable (a check whose need
the environment does not meet).

| environment | device | checks / failed / n/a |
|---|---|---|
| `wayland-vulkan` | RX 590 | 73 / 0 / 9 |
| `wayland-gl` | RX 590 | 73 / 0 / 9 |
| `x11-vulkan` | RX 590 | 102 / 0 / 4 |
| `x11-gl` | RX 590 | 102 / 0 / 4 |
| `win64-vulkan` (wine) | RX 590 | 86 / 0 / 5 |
| `win64-d3d12` (wine) | RX 590 | 89 / 0 / 4 |
| `win64-vulkan-wl` | RX 590 | 53 / 0 / 14 |
| `win64-d3d12-wl` | RX 590 | 56 / 0 / 13 |
| `win64-vulkan-vm@win10`, `@win11` | lavapipe | 88 / 0 / 7 each |
| `win64-d3d12-vm@win10`, `@win11` | WARP | 88 / 0 / 7 each |
| `win64-vulkan-native@aspire` | HD 530 / GTX 960M, through DXGI | 98 / 0 / 8 each |
| `win64-d3d12-native@aspire` | HD 530 / GTX 960M | 98 / 0 / 8 each |
| `win64-vulkan-native@schenker` | HD 630 / GTX 1050 Ti, through DXGI | 99 / 0 / 8 each |
| `win64-d3d12-native@schenker` | HD 630 / GTX 1050 Ti | 99 / 0 / 8 each |
| `macos-metal-native@schenker-mac` | HD 630 | 78 / 0 / 8 |
| `macos-vulkan-native@schenker-mac` | HD 630 (MoltenVK) | 78 / 0 / 8 |

The bare machines, a column for each boot. On the nouveau entry the
library's own pick is the integrated GPU (NVK's on a GPU before Turing
ranks below it). A KMS round on a kernel driver opens where the machine's
GPU ranking says, the integrated GPU on both laptops, and walks every
renderer on every GPU from the titlebar's box, moving the NVIDIA GPU to
the other driver and back on the way; so the KMS OpenGL round on a
driver is that same round and is skipped as its twin.

| environment | thinkpad | pi4 | aspire, NVIDIA entry | aspire, nouveau entry | schenker, NVIDIA entry | schenker, nouveau entry |
|---|---|---|---|---|---|---|
| `x11-vulkan-bare` | lavapipe 69 / 0 / 7 | V3D 84 / 0 / 6 | GTX 960M 91 / 0 / 6 | HD 530 91 / 0 / 6 | GTX 1050 Ti 91 / 0 / 6 | HD 630 91 / 0 / 6 |
| `x11-gl-bare` | llvmpipe 84 / 0 / 6 | llvmpipe 88 / 0 / 6 | HD 530 91 / 0 / 6 | HD 530 91 / 0 / 6 | HD 630 91 / 0 / 6 | HD 630 91 / 0 / 6 |
| `wayland-vulkan-bare` | lavapipe 46 / 0 / 12 | V3D 56 / 0 / 11 | GTX 960M 65 / 0 / 11 | HD 530 62 / 0 / 11 | GTX 1050 Ti 65 / 0 / 11 | HD 630 62 / 0 / 11 |
| `wayland-gl-bare` | llvmpipe 57 / 0 / 11 | llvmpipe 59 / 0 / 11 | HD 530 65 / 0 / 11 | HD 530 62 / 0 / 11 | HD 630 65 / 0 / 11 | HD 630 62 / 0 / 11 |
| `kms-vulkan-bare` | lavapipe 55 / 0 / 15 | V3D 58 / 0 / 16 | | | | |
| `kms-vulkan-bare@…:nouveau` | | | HD 530 97 / 0 / 24 | HD 530 97 / 0 / 24 | HD 630 97 / 0 / 24 | HD 630 97 / 0 / 24 |
| `kms-vulkan-bare@…:nvidia` | | | HD 530 97 / 0 / 24 | HD 530 97 / 0 / 24 | HD 630 97 / 0 / 24 | HD 630 97 / 0 / 24 |
| `kms-gl-bare` | llvmpipe 55 / 0 / 15 | llvmpipe 58 / 0 / 16 | | | | |
| `win64-vulkan-bare` | modesetting 59 / 0 / 9 | | GTX 960M 73 / 0 / 8 | GTX 960M 73 / 0 / 8 | NVIDIA-G0 73 / 0 / 8 | HD 630 73 / 0 / 8 |
| `win64-d3d12-bare` | modesetting 71 / 0 / 7 | | GTX 960M 79 / 0 / 7 | GTX 960M 79 / 0 / 7 | NVIDIA-G0 79 / 0 / 7 | GTX 1050 Ti (NVK) 79 / 0 / 7 |
| `win64-vulkan-wl-bare` | lavapipe 41 / 0 / 17 | | GTX 960M 54 / 0 / 16 | GTX 960M 54 / 0 / 16 | GTX 1050 Ti 54 / 0 / 16 | HD 630 54 / 0 / 16 |
| `win64-d3d12-wl-bare` | llvmpipe 52 / 0 / 15 | | GTX 960M 60 / 0 / 15 | GTX 960M 60 / 0 / 15 | GTX 1050 Ti 60 / 0 / 15 | GTX 1050 Ti (NVK) 60 / 0 / 15, second run |
| `win64-vulkan-cage-bare` | lavapipe 37 / 0 / 15 | | GTX 960M 48 / 0 / 14 | GTX 960M 48 / 0 / 14 | GTX 1050 Ti 48 / 0 / 14 | HD 630 48 / 0 / 14 |
| `win64-d3d12-cage-bare` | llvmpipe 45 / 0 / 13 | | GTX 960M 54 / 0 / 13 | GTX 960M 54 / 0 / 13 | GTX 1050 Ti 54 / 0 / 13 | GTX 1050 Ti (NVK) 54 / 0 / 13 |

The one round that did not pass at its first run, wine's Direct3D 12 on
weston on schenker's nouveau entry, lost its window to the harness after
an edge resize, 13 checks in, the program still drawing at the size the
drag left; run again on the same boot it passed, as did weston's own
Vulkan round beside it. It is TODO's NVK-under-weston item, met again.

The Pi has no wine, so its six wine environments are skipped; its X11
and Wayland rounds run on the output its kernel line forces on. Under
wine the device is the program's own account of it: wine names a GPU
the X server drives with `modesetting` after that driver, and schenker's
NVIDIA GPU as `NVIDIA-G0` under Xorg. The devices are each round's
`GPU in use` line, which the ledger now reads off the program's opening
alone (`ledger.device()`).

### This desktop's console, 2026-10-24 at `60f6862`

`black` booted into `fenestal-console`: the RX 590 on amdgpu, HDMI-A-1 at
3840x2160 and DVI-D-1 at 1920x1200, driven as a bare machine. The stress
rounds, checks / failed / not applicable, all with `--capture`; the four
unplug checks are skipped in each (a forced unplug sends no uevent on this
kernel), and the captures after a move to Vulkan's display path too (RADV's
image has no CPU access).

| environment | release | asan | tsan | gc |
|---|---|---|---|---|
| `kms-vulkan-bare@black` (RADV) | 63 / 0 / 19, three runs | 63 / 0 / 19 | 63 / 0 / 19 | 63 / 0 / 19 |
| `kms-gl-bare@black` (radeonsi) | 68 / 0 / 14 | 68 / 0 / 15 | 68 / 0 / 15 | 68 / 0 / 15 |

### The Raspberry Pi 4's checker rounds, 2026-10-24 at `6ef6044`

KMS on the output the kernel's line forces on with no monitor (DESIGN
*Cross-compiling for ARM Linux*), checks / failed / not applicable,
with `--capture` where marked:

| environment | release | asan | tsan | gc |
|---|---|---|---|---|
| `kms-vulkan-bare@pi4` (V3D) | 63 / 0 / 7 (capture) | 63 / 0 / 7 (capture) | 56 / 0 / 14 | 56 / 0 / 14 |
| `kms-gl-bare@pi4` (llvmpipe) | 63 / 0 / 7 (capture) | 56 / 0 / 15 | 56 / 0 / 15 | |

### schenker's KMS rounds on a kernel driver, the checkers, 2026-10-31

The rounds that move schenker's NVIDIA GPU between nouveau and NVIDIA's
module from the titlebar's box and walk every renderer on every GPU,
under each checker, at `b4571d7` with this set's OpenGL and harness
fixes (TODO's overnight item). With the ranking schenker keeps, a round
opens on its first entry and the KMS OpenGL round is its twin, skipped;
with the ranking set aside, both run, each told its renderer. Checks /
failed / not applicable.

| round | asan | tsan | gc |
|---|---|---|---|
| ranked, `kms-vulkan-bare@schenker:nouveau` | 97 / 0 / 24 | 71 / 2 / 21 | 97 / 0 / 24 |
| ranked, `kms-vulkan-bare@schenker:nvidia` | 97 / 0 / 24 | skipped | 97 / 0 / 24 |
| unranked, `kms-vulkan-bare@schenker:nouveau` | 97 / 0 / 24 | 71 / 2 / 21 | 97 / 0 / 24 |
| unranked, `kms-vulkan-bare@schenker:nvidia` | 97 / 0 / 24 | skipped | 97 / 0 / 24 |
| unranked, `kms-gl-bare@schenker:nouveau` | 97 / 0 / 25 | 71 / 2 / 22 | 97 / 0 / 25 |
| unranked, `kms-gl-bare@schenker:nvidia` | 97 / 0 / 25 | skipped | 97 / 0 / 25 |

Under ThreadSanitizer the rounds on NVIDIA's module are skipped, and the
round on nouveau sets NVIDIA's userspace aside, since libglvnd loaded
NVIDIA's EGL vendor to list OpenGL's GPUs and ThreadSanitizer spent 80
minutes in it (`Environment.driver_bind()`); a ranked round under it
orders and keeps nothing, so both renderers run. Its two failures are
the exit status and the reports: a lock-order inversion inside the
validation layer and lavapipe's `_mesa_hash_table_copy()`, the drivers'
own (`tsan.supp`, TODO). Before the fixes, ASan and gc failed every round
on NVIDIA's OpenGL debug output alone: its refusal of the output's buffer
and its move of the copy's readback buffer (DESIGN *KMS: no window
server*).

## Pacing

`fenestal-dev pace` asks for its own sizes at a fixed rate and registers no
callbacks, so each cell is the library's own cost: frames presented, then
CPU as per cent of one core (above 100 is more than one). 12 s a cell, CPU
over 6 s of it. `draw` asks for every frame and no size; `border` is
`fenestal-testbed` with its corner carried in and out by the harness, so it has
the testbed's scene in it and is read across, not down. A dash is an
environment whose border the harness cannot drag. The wine columns' CPU is
wine's too. The frames are the presents the pace mode counts while it
drives (`fnl_frame_stats()`), never the library's `--frame-times` lines,
which a window resting between two sizes does not print (DESIGN *At
rest*).

### This desktop, RX 590

The `border` row is measured again (2026-10-31, `1d987ac`, two sweeps):
`pace.py` now counts the frames the `--frame-times` lines report under
the press, over the time they span (`frames_between()`), where it had
averaged lines picked by position and taken the drag's resting lines
with the rest. Its CPU is unchanged; its frames are lower, most of all
OpenGL's and Direct3D 12's, which the old count read near 60.

| sizes/s | wayland-vulkan | wayland-gl | x11-vulkan | x11-gl | win64-vulkan | win64-d3d12 | win64-vulkan-wl | win64-d3d12-wl |
|---|---|---|---|---|---|---|---|---|
| idle | 0, 0.3% | 0, 0.3% | 0, 0.3% | 0, 0.2% | 0, 0.7% | 0, 0.5% | 0, 0.5% | 0, 0.5% |
| 5 | 20, 1.5% | 20, 1.8% | 20, 3.2% | 19, 1.8% | 19, 4.2% | 19, 5.8% | 19, 3.0% | 19, 5.0% |
| 15 | 47, 3.0% | 48, 5.0% | 48, 7.8% | 45, 4.3% | 46, 10.5% | 46, 12.5% | 45, 7.2% | 45, 12.5% |
| 30 | 60, 3.5% | 60, 7.0% | 59, 11.3% | 61, 6.8% | 60, 15.5% | 59, 14.3% | 59, 9.5% | 59, 17.8% |
| 45 | 60, 3.7% | 60, 7.3% | 60, 14.0% | 61, 7.3% | 60, 15.5% | 59, 14.8% | 59, 11.7% | 59, 18.2% |
| 60 | 60, 3.8% | 60, 8.2% | 60, 15.2% | 61, 7.7% | 60, 14.8% | 58, 14.7% | 59, 11.0% | 59, 17.7% |
| 250 | 60, 5.5% | 60, 9.5% | 60, 15.3% | 62, 10.3% | 60, 16.0% | 58, 14.8% | 59, 12.5% | 59, 19.5% |
| draw | 60, 8.3% | 60, 7.0% | 60, 8.2% | 62, 6.2% | 60, 7.7% | 60, 11.0% | 60, 7.7% | 60, 10.2% |
| border | -- | -- | 52-57, 12.6-12.9% | 36-40, 7.6-7.7% | 53-56, 13.8-15.6% | 35-37, 14.6-15.2% | 55-58, 14.4% | 18-23, 20.0-20.7% |

### The laptops on Linux, Intel GPUs and lavapipe

`bare` is Xorg with no window manager, `weston` is weston, `kms` is no
server at all. thinkpad is lavapipe, so its CPU is the frame's rasterising.
schenker and aspire booted on the key's NVIDIA entry, put on the Intel GPU
by `FNL_GPU`.

| sizes/s | bare aspire | bare schenker | bare thinkpad | weston aspire | weston schenker | weston thinkpad | kms aspire | kms schenker | kms thinkpad |
|---|---|---|---|---|---|---|---|---|---|
| idle | 0, 0.3% | 0, 0.2% | 0, 0.2% | 0, 0.2% | 0, 0.3% | 0, 0.2% | 0, 0.3% | 0, 0.2% | 0, 0.2% |
| 5 | 19, 7.0% | 19, 5.8% | 20, 25.2% | 19, 2.3% | 19, 2.0% | 19, 17.3% | 0, 0.3% | 0, 0.3% | 0, 0.2% |
| 15 | 47, 19.1% | 46, 16.1% | 45, 65.4% | 48, 5.0% | 47, 4.5% | 46, 44.1% | 0, 0.5% | 0, 0.5% | 0, 0.2% |
| 30 | 60, 38.4% | 60, 30.1% | 60, 92.0% | 60, 7.0% | 60, 6.1% | 60, 58.4% | 0, 0.5% | 0, 0.3% | 0, 0.3% |
| 45 | 60, 45.3% | 60, 41.9% | 60, 91.4% | 60, 7.3% | 60, 6.1% | 59, 58.4% | 0, 0.7% | 0, 0.7% | 0, 0.3% |
| 60 | 60, 48.4% | 60, 50.5% | 60, 92.7% | 60, 8.1% | 60, 6.3% | 60, 59.9% | 0, 0.7% | 0, 0.7% | 0, 0.5% |
| 250 | 60, 50.5% | 60, 50.5% | 60, 92.8% | 60, 9.9% | 60, 8.4% | 60, 61.1% | 0, 2.2% | 0, 1.8% | 0, 1.3% |
| draw | 60, 11.1% | 60, 8.4% | 60, 75.8% | 60, 10.9% | 60, 8.6% | 30, 40.7% | 60, 8.9% | 60, 7.0% | 60, 86.8% |
| border | 60, 45.8% | 60, 41.8% | 40, 298% | -- | -- | -- | -- | -- | -- |

### The thinkpad, its Linux and Windows trees, 2026-11-01 at `63c31e4`

The `linux` tree on Xorg, weston and KMS, and the `win64` tree under wine
11.21 on Xorg, weston and cage, Vulkan and then Direct3D 12 on
vkd3d-proton, all drawn by lavapipe. Two sweeps; a range is the two. Run
as:

```sh
FNL_DRIVE_BARE=thinkpad MESA_VK_WSI_DEBUG=noshm python3 tools/drive/pace.py \
    x11-vulkan-bare@thinkpad wayland-vulkan-bare@thinkpad \
    kms-vulkan-bare@thinkpad win64-vulkan-bare@thinkpad \
    win64-vulkan-wl-bare@thinkpad win64-vulkan-cage-bare@thinkpad \
    win64-d3d12-bare@thinkpad win64-d3d12-wl-bare@thinkpad \
    win64-d3d12-cage-bare@thinkpad
```

| sizes/s | bare | weston | kms | wine bare | wine weston | wine cage | d3d12 bare | d3d12 weston | d3d12 cage |
|---|---|---|---|---|---|---|---|---|---|
| idle | 0, 0.3% | 0, 0.2-0.3% | 0, 0.2% | 0, 1.1-1.2% | 0, 1.0-1.1% | 0, 1.2-1.3% | 0, 10.7-77.1% | 0, 23.1-30.5% | 0, 30.2-33.8% |
| 5 | 19-20, 17.5-18.9% | 19, 13.2-13.3% | 0, 0.2-0.3% | 20, 17.8-18.8% | 19-20, 19.8-21.7% | 0, 1.3% | 19, 44.0-44.4% | 19, 60.1-60.6% | 0, 9.8-15.8% |
| 15 | 45, 46.5-46.6% | 46, 34.2-34.5% | 0, 0.2-0.3% | 46-47, 44.9-45.2% | 47, 53.2-53.4% | 0, 1.5% | 45, 92.9-95.6% | 38-45, 98.7-116.2% | 0, 10.7-11.6% |
| 30 | 60, 64.5-67.5% | 59, 44.1-44.7% | 0, 0.2% | 60, 62.0-65.7% | 60, 75.3-79.8% | 0, 2.0% | 52, 107.6-108.5% | 36-48, 102.0-136.3% | 0, 8.2-12.2% |
| 45 | 60, 67.0-67.1% | 60, 45.6-46.7% | 0, 0.3% | 60, 70.8-71.4% | 61-62, 81.4-87.2% | 0, 2.5-2.6% | 52-53, 115.4-118.7% | 46-54, 134.9-140.1% | 0, 11.2-23.8% |
| 60 | 60, 68.4% | 60, 46.7-46.8% | 0, 0.3-0.5% | 60, 72.9-74.2% | 60-61, 84.2-85.1% | 0, 3.1-3.3% | 47-50, 108.9-109.1% | 34-50, 101.4-117.8% | 0, 14.8-15.7% |
| 250 | 60, 68.5-68.7% | 60, 47.8-48.2% | 0, 1.3% | 60, 75.1-76.5% | 60, 87.8-88.2% | 0, 9.7-9.8% | 46-47, 104.9-108.3% | 50-51, 136.0-136.3% | 0, 17.4-17.7% |
| draw | 60, 59.2-59.6% | 30, 30.8-31.3% | 60, 67.1% | 70-72, 74.8-77.3% | 36-37, 34.6-36.3% | 59, 66.6% | 51, 148.1-148.5% | 36, 110.0-110.6% | 51, 143.9-145.0% |
| border | 39-40, 185.4-185.7% | -- | -- | 36-37, 182.4-183.4% | 33, 176.5-178.4% | -- | 14-15, 202.7-203.1% | 15-16, 188.9-189.4% | -- |

### The laptops on Linux, NVIDIA's own driver

The key's NVIDIA entry, NVIDIA 580.196.07, by `FNL_GPU=nvidia`.

| sizes/s | bare aspire | bare schenker | weston aspire | weston schenker | kms aspire | kms schenker |
|---|---|---|---|---|---|---|
| idle | 0, 0.8% | 0, 0.5% | 0, 0.8% | 0, 0.7% | 0, 0.8% | 0, 0.7% |
| 5 | 19, 11.1% | 19, 9.6% | 19, 2.0% | 20, 3.2% | 0, 0.7% | 0, 0.7% |
| 15 | 45, 29.8% | 46, 24.7% | 47, 4.6% | 47, 5.3% | 0, 1.0% | 0, 0.8% |
| 30 | 58, 35.3% | 59, 38.2% | 60, 6.0% | 60, 6.5% | 0, 1.2% | 0, 0.7% |
| 45 | 58, 47.2% | 59, 47.1% | 60, 6.1% | 60, 7.0% | 0, 1.0% | 0, 0.8% |
| 60 | 59, 42.6% | 59, 38.6% | 60, 6.8% | 60, 7.3% | 0, 1.3% | 0, 0.8% |
| 250 | 57, 43.7% | 59, 49.1% | 60, 7.9% | 60, 8.0% | 0, 2.6% | 0, 2.0% |
| draw | 60, 11.3% | 60, 7.9% | 60, 9.9% | 60, 8.9% | 58, 17.2% | 58, 17.5% |
| border | 57, 43.9% | 55, 45.8% | -- | -- | -- | -- |

### The laptops on Linux, NVK

The key's nouveau entry, by `FNL_GPU=nvidia`.

| sizes/s | bare aspire | bare schenker | weston aspire | weston schenker | kms aspire | kms schenker |
|---|---|---|---|---|---|---|
| idle | 0, 0.3% | 0, 0.0% | 0, 0.3% | 0, 0.0% | 0, 0.3% | 0, 0.2% |
| 5 | 19, 5.1% | 19, 4.8% | 19, 1.7% | 19, 1.3% | 0, 0.3% | 0, 0.3% |
| 15 | 46, 14.4% | 47, 12.4% | 47, 3.8% | 48, 3.2% | 0, 0.3% | 0, 0.3% |
| 30 | 60, 26.8% | 60, 22.8% | 60, 4.8% | 60, 4.0% | 0, 0.3% | 0, 0.5% |
| 45 | 60, 38.7% | 60, 33.1% | 60, 5.2% | 60, 4.3% | 0, 0.5% | 0, 0.7% |
| 60 | 60, 43.0% | 60, 42.3% | 60, 5.5% | 60, 4.3% | 0, 0.7% | 0, 0.5% |
| 250 | 60, 39.9% | 60, 43.3% | 60, 7.4% | 60, 5.6% | 0, 1.8% | 0, 1.5% |
| draw | 60, 10.9% | 60, 7.9% | 60, 10.6% | 60, 7.3% | 60, 18.7% | 59, 15.6% |
| border | 59, 38.3% | 60, 33.5% | -- | -- | -- | -- |

### The Raspberry Pi 4, V3D

The size rows of the KMS column draw nothing, as on every KMS machine:
the window is its output and refuses every size.

| sizes/s | bare pi4 | weston pi4 | kms pi4 |
|---|---|---|---|
| idle | 0, 0.3% | 0, 0.2% | 0, 0.3% |
| 5 | 19, 12.1% | 20, 9.6% | 0, 0.3% |
| 15 | 47, 30.1% | 48, 14.2% | 0, 0.3% |
| 30 | 59, 50.0% | 59, 21.1% | 0, 0.5% |
| 45 | 59, 51.9% | 59, 23.0% | 0, 0.5% |
| 60 | 60, 55.6% | 60, 25.9% | 0, 0.7% |
| 250 | 60, 53.6% | 59, 28.1% | 0, 1.7% |
| draw | 60, 20.5% | 60, 20.2% | 60, 14.9% |
| border | 60, 46.6% | -- | -- |

### The laptops on Windows

Vulkan, through DXGI on every GPU now (DESIGN *Vulkan through DXGI*),
then Direct3D 12, each GPU.

| sizes/s | aspire HD 530 | schenker HD 630 | aspire GTX 960M | schenker GTX 1050 Ti | D3D12 aspire HD 530 | D3D12 schenker HD 630 | D3D12 aspire GTX 960M | D3D12 schenker GTX 1050 Ti |
|---|---|---|---|---|---|---|---|---|
| idle | 0, 0.0% | 0, 0.5% | 0, 0.3% | 0, 0.8% | 0, 0.3% | 0, 0.5% | 0, 0.8% | 0, 0.5% |
| 5 | 19, 1.3% | 19, 1.0% | 20, 3.9% | 20, 5.2% | 20, 0.8% | 19, 0.8% | 20, 2.6% | 20, 2.3% |
| 15 | 44, 4.7% | 46, 3.6% | 40, 10.2% | 46, 7.8% | 40, 3.9% | 46, 2.1% | 40, 4.7% | 46, 6.0% |
| 30 | 60, 6.2% | 60, 6.5% | 60, 15.9% | 60, 14.1% | 60, 6.2% | 60, 3.4% | 55, 8.3% | 60, 8.1% |
| 45 | 60, 7.5% | 60, 7.5% | 60, 14.8% | 60, 16.9% | 60, 6.0% | 60, 4.7% | 60, 9.9% | 60, 6.5% |
| 60 | 60, 10.4% | 60, 10.4% | 60, 18.0% | 60, 17.2% | 60, 4.9% | 60, 3.9% | 57, 13.8% | 60, 8.3% |
| 250 | 60, 8.1% | 60, 9.9% | 60, 21.3% | 60, 16.9% | 60, 7.3% | 60, 3.9% | 60, 9.1% | 60, 8.1% |
| draw | 60, 2.9% | 60, 2.6% | 60, 11.7% | 60, 12.5% | 60, 3.1% | 60, 2.6% | 60, 8.6% | 60, 4.7% |
| border | 60, 9.2% | 59, 7.7% | 60, 18.3% | 60, 15.7% | 58, 6.7% | 60, 5.6% | 58, 10.6% | 60, 8.0% |

### schenker on macOS

The HD 630 under macOS 26.7.1, on the built-in panel alone (1920x1080,
60 Hz, on AC power), from a cold start and once the machine had settled.

| sizes/s | Metal | Vulkan |
|---|---|---|
| idle | 0, 0.2% | 0, 0.2% |
| 5 | 20, 4.8% | 20, 6.1% |
| 15 | 47, 11.8% | 46, 15.8% |
| 30 | 60, 19.9% | 60, 23.4% |
| 45 | 60, 23.6% | 60, 27.6% |
| 60 | 60, 30.1% | 60, 31.1% |
| 250 | 60, 56.2% | 60, 60.3% |
| draw | 60, 10.4% | 60, 10.4% |
| border | 60, 22.3% | 60, 27.0% |

## Benchmark mode, 2026-11-04 at `5a10308`

The testbed's benchmark mode in every environment the harness reaches
but this desktop's and the VirtualBox guests': the bare machines on
their keys, the KMS rounds once on each kernel driver, both laptops on
Windows on each GPU, and schenker on macOS with each renderer. One
release build of each tree, at `065e0f1`, the harness alone changed
since; the defaults, settle 4 s, warm-up 2 s and measure 20 s, vsync
on, and `--hands`, the pointer circling the window through each measure
for the latency rows. A row is one run, so it indicates and does not
measure; `--bench-repeats` several times would put the standard
deviation beside each figure.

`size px` is the area drawn, and two rows compare only with it: on KMS
the window is the output and its area the mode's, and schenker's
Windows rows are 1600x900 for its display's 125% scale where aspire's
are 1280x720. `MP/s` is the image the path delivered a second and
`GPU ms/MP` and `CPU ms/MP` what a megapixel of it cost; `latch→shown`
and `read→shown` are the record's own stamps, a frame's latch to its
screen and an input's read to it, p50. Read across the sizes: KMS
delivers the most image a second, 119 MP/s against the bare windowed
rows' 55 on the laptops and 60 against the Pi's 44 and 28, and shows a
frame soonest, 8.7 to 12.7 ms against X11's 16.4 to 16.5. Its copy path
costs 1.1 to 4.3 ms a megapixel on the laptops against X11's 0.9 to
2.0, NVK's readback the expensive one, and on the Pi 8.8 against X11's
12.6: the copy is cheaper per pixel where the rasteriser is the whole
cost, and the thinkpad's 20.5, 21.8 and 20.6 for KMS, X11 and weston
are that rasteriser to the last decimal. The Mac's rows are the longest
from latch to screen, 48 to 70 ms, the same lesson from the other end.
The Pi's KMS round carries no stamps, so its latency cells are empty; a
dash is a figure the driver does not keep.

```sh
python3 tools/drive/bench.py --hands x11-vulkan-bare wayland-vulkan-bare \
    kms-vulkan-bare win64-vulkan-native win64-d3d12-native \
    macos-metal-native macos-vulkan-native macos-gl-native
```

The two KMS rows of an NVIDIA machine are its two kernel drivers,
`nouveau` first and `nvidia` second; the record does not carry the
driver, so only the figures tell them apart. This sweep is one size a
place: `--sizes 640x360,1280x720` runs each once per area -- on KMS as a
display mode, its window being the output -- and `report.py --fit` reads
such records into a frame's fixed cost and its cost a megapixel.

| where | run under | n | size px | MP | fps | MP/s | p50 ms | p99 ms | 1% low | GPU ms | GPU ms/MP | CPU ms | CPU ms/MP | CPU % | latch→shown | read→shown |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SCHENKER, Vulkan on Win32, Intel HD Graphics 630 | defaults | 1, indication | 1600x900 | 1.44 | 60.0 | 86 | 16.66 | 17.26 | 55 | 1.16 | 0.81 | 0.42 | 0.29 | 5 | 20.2 | 26.2 |
| SCHENKER, Vulkan on Win32, NVIDIA GeForce GTX 1050 Ti | defaults | 1, indication | 1600x900 | 1.44 | 60.0 | 86 | 16.66 | 17.29 | 57 | 0.44 | 0.30 | 0.67 | 0.46 | 11 | 20.2 | 25.9 |
| ASPIRE, Vulkan on Win32, Intel HD Graphics 530 | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.68 | 17.12 | 58 | 1.55 | 1.68 | 0.52 | 0.56 | 5 | 20.3 | 26.3 |
| ASPIRE, Vulkan on Win32, NVIDIA GeForce GTX 960M | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.63 | 17.53 | 57 | 1.43 | 1.55 | 0.83 | 0.90 | 12 | 20.2 | 26.1 |
| SCHENKER, Direct3D 12 on Win32, Intel HD Graphics 630 | defaults | 1, indication | 1600x900 | 1.44 | 60.0 | 86 | 16.66 | 16.81 | 58 | 1.10 | 0.76 | 0.33 | 0.23 | 4 | 20.2 | 22.1 |
| SCHENKER, Direct3D 12 on Win32, NVIDIA GeForce GTX 1050 Ti | defaults | 1, indication | 1600x900 | 1.44 | 60.0 | 86 | 16.66 | 16.84 | 59 | 1.18 | 0.82 | 0.82 | 0.57 | 7 | 19.8 | 22.7 |
| ASPIRE, Direct3D 12 on Win32, Intel HD Graphics 530 | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.66 | 16.83 | 59 | 1.03 | 1.12 | 0.48 | 0.52 | 5 | 20.4 | 22.2 |
| ASPIRE, Direct3D 12 on Win32, NVIDIA GeForce GTX 960M | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.66 | 16.86 | 58 | 1.17 | 1.27 | 1.20 | 1.31 | 11 | 19.8 | 22.5 |
| aspire, Vulkan on X11, NVIDIA GeForce GTX 960M | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.66 | 16.83 | 59 | 1.83 | 1.99 | 0.87 | 0.94 | 8 | 16.4 | 22.1 |
| schenker, Vulkan on X11, NVIDIA GeForce GTX 1050 Ti | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.66 | 16.83 | 59 | 0.83 | 0.91 | 0.59 | 0.64 | 7 | 16.5 | 22.7 |
| thinkpad, Vulkan on X11, llvmpipe | defaults | 1, indication | 1280x720 | 0.92 | 49.5 | 46 | 18.22 | 27.84 | 33 | 20.05 | 21.76 | 0.15 | 0.16 | 197 | 28.9 | 34.3 |
| pi4, Vulkan on X11, V3D 4.2.14.0 | defaults | 1, indication | 1024x720 | 0.74 | 60.0 | 44 | 16.66 | 17.17 | 57 | 9.31 | 12.62 | 1.64 | 2.22 | 20 | 16.6 | 22.6 |
| aspire, Vulkan on Wayland, NVIDIA GeForce GTX 960M | defaults | 1, indication | 1280x720 | 0.92 | 59.5 | 55 | 16.66 | 17.07 | 33 | 1.96 | 2.13 | 0.60 | 0.65 | 7 | 16.6 | 22.2 |
| schenker, Vulkan on Wayland, NVIDIA GeForce GTX 1050 Ti | defaults | 1, indication | 1280x720 | 0.92 | 59.5 | 55 | 16.67 | 17.09 | 33 | 0.96 | 1.04 | 0.60 | 0.65 | 8 | 16.2 | 21.3 |
| thinkpad, Vulkan on Wayland, llvmpipe | defaults | 1, indication | 1280x720 | 0.92 | 29.3 | 27 | 33.22 | 49.84 | 20 | 18.96 | 20.58 | 0.22 | 0.24 | 109 | 54.3 | 60.1 |
| pi4, Vulkan on Wayland, V3D 4.2.14.0 | defaults | 1, indication | 1280x720 | 0.92 | 30.2 | 28 | 33.32 | 34.21 | 22 | 8.47 | 9.19 | 1.70 | 1.84 | 11 | 46.4 | 50.5 |
| aspire, Vulkan on KMS, NVIDIA GeForce GTX 960M | defaults | 1, indication | 1920x1046 | 2.01 | 59.0 | 119 | 16.66 | 33.27 | 30 | 7.50 | 3.74 | 2.90 | 1.44 | 20 | 12.0 | 17.4 |
| aspire, Vulkan on KMS, NVIDIA GeForce GTX 960M | defaults | 1, indication | 1920x1046 | 2.01 | 58.5 | 117 | 16.66 | 33.35 | 30 | 7.25 | 3.61 | 2.50 | 1.25 | 17 | 12.4 | 17.7 |
| schenker, Vulkan on KMS, NVIDIA GeForce GTX 1050 Ti | defaults | 1, indication | 1920x1046 | 2.01 | 59.2 | 119 | 16.66 | 32.47 | 30 | 8.64 | 4.30 | 2.49 | 1.24 | 17 | 12.7 | 18.4 |
| schenker, Vulkan on KMS, NVIDIA GeForce GTX 1050 Ti | defaults | 1, indication | 1920x1046 | 2.01 | 59.1 | 119 | 16.67 | 33.24 | 30 | 2.29 | 1.14 | 2.40 | 1.20 | 18 | 8.7 | 15.2 |
| thinkpad, Vulkan on KMS, llvmpipe | defaults | 1, indication | 1366x734 | 1.00 | 30.1 | 30 | 33.19 | 33.42 | 30 | 20.56 | 20.51 | 1.89 | 1.88 | 126 | 33.8 | 39.0 |
| pi4, Vulkan on KMS, V3D 4.2.14.0 | defaults | 1, indication | 1920x1046 | 2.01 | 30.0 | 60 | 33.34 | 33.50 | 30 | 17.58 | 8.75 | 1.36 | 0.68 | 9 | - | - |
| jsons-MBP, Metal on Cocoa, Intel HD Graphics 630 | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.66 | 17.10 | 58 | 1.46 | 1.59 | 0.20 | 0.22 | 15 | 69.5 | 70.6 |
| jsons-MBP, Vulkan on Cocoa, Intel HD Graphics 630 | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.66 | 17.06 | 58 | 1.47 | 1.60 | 0.41 | 0.44 | 18 | 48.3 | 53.9 |
| jsons-MBP, OpenGL on Cocoa, Intel HD Graphics 630 | defaults | 1, indication | 1280x720 | 0.92 | 60.0 | 55 | 16.68 | 17.10 | 58 | - | - | 0.96 | 1.04 | 22 | - | - |

## Benchmark mode at two sizes, 2026-11-04 at `5f7df40`

The same benchmark run once at 800x600 and once at 1920x1080 points, on
the bare machines and on both laptops' Windows on each GPU and each
renderer -- on KMS a display mode, its window being the output, so the
Pi's KMS took both (its HDMI modes) and the laptops' and thinkpad's
KMS, whose panels list one mode each, stayed at it and are named below
as one size. The records were made on the tree before it was committed,
the same code. The fit is `cost = fixed + slope x area` over the areas
the runs recorded (`report.py --fit`): the milliseconds a frame costs
at no pixels and the milliseconds a megapixel adds. Two points fit a
line exactly and each is one run, so the intercepts carry the noise --
a negative CPU slope is noise, not a saving -- and a third size or
repeats would settle them. The two sizes read as 0.48 and 2.01 MP on
the bare screens, 0.48 and 1.92 on aspire's Windows (100%), and 0.75
and 1.88 on schenker's (125%).

| where | run under | sizes MP | GPU fixed ms | GPU ms/MP | CPU fixed ms | CPU ms/MP |
|---|---|---|---|---|---|---|
| SCHENKER, Vulkan on Win32, Intel(R) HD Graphics 630 | defaults | 0.75, 1.88 | 0.04 | 0.77 | 0.40 | 0.01 |
| SCHENKER, Vulkan on Win32, NVIDIA GeForce GTX 1050 Ti | defaults | 0.75, 1.88 | -0.01 | 0.33 | 0.66 | 0.01 |
| ASPIRE, Vulkan on Win32, Intel(R) HD Graphics 530 | defaults | 0.48, 1.92 | 0.09 | 1.57 | 0.50 | 0.00 |
| ASPIRE, Vulkan on Win32, NVIDIA GeForce GTX 960M | defaults | 0.48, 1.92 | -0.13 | 1.70 | 0.97 | -0.12 |
| SCHENKER, Direct3D 12 on Win32, Intel(R) HD Graphics 630 | defaults | 0.75, 1.88 | 0.07 | 0.70 | 0.34 | -0.00 |
| SCHENKER, Direct3D 12 on Win32, NVIDIA GeForce GTX 1050 Ti | defaults | 0.75, 1.88 | 0.41 | 0.27 | 0.82 | 0.01 |
| ASPIRE, Direct3D 12 on Win32, Intel(R) HD Graphics 530 | defaults | 0.48, 1.92 | 0.16 | 0.92 | 0.46 | 0.02 |
| ASPIRE, Direct3D 12 on Win32, NVIDIA GeForce GTX 960M | defaults | 0.48, 1.92 | 0.11 | 1.13 | 1.20 | -0.04 |
| aspire, Vulkan on X11, NVIDIA GeForce GTX 960M | defaults | 0.48, 2.01 | 0.11 | 1.83 | 0.84 | 0.01 |
| schenker, Vulkan on X11, NVIDIA GeForce GTX 1050 Ti | defaults | 0.48, 2.01 | 0.25 | 0.39 | 0.57 | -0.01 |
| thinkpad, Vulkan on X11, llvmpipe (LLVM 23.1.4, 128 bits) | defaults | 0.48, 1.00 | 1.86 | 19.52 | 0.19 | -0.05 |
| pi4, Vulkan on X11, V3D 4.2.14.0 | defaults | 0.48, 0.75 | 3.67 | 4.08 | 2.62 | -2.02 |
| aspire, Vulkan on Wayland, NVIDIA GeForce GTX 960M | defaults | 0.48, 2.07 | 0.32 | 1.75 | 0.58 | 0.00 |
| schenker, Vulkan on Wayland, NVIDIA GeForce GTX 1050 Ti | defaults | 0.48, 2.07 | 0.13 | 0.95 | 0.58 | 0.01 |
| thinkpad, Vulkan on Wayland, llvmpipe (LLVM 23.1.4, 128 bits) | defaults | 0.48, 2.07 | 3.67 | 16.88 | 0.21 | 0.01 |
| pi4, Vulkan on Wayland, V3D 4.2.14.0 | defaults | 0.48, 2.07 | 3.59 | 7.26 | 3.03 | -0.57 |
| pi4, Vulkan on KMS, V3D 4.2.14.0 | defaults | 0.45, 2.01 | 0.27 | 8.61 | 1.16 | 0.10 |

one size, not fitted: aspire, Vulkan on KMS, NVIDIA GeForce GTX 960M, aspire,
Vulkan on KMS, NVIDIA GeForce GTX 960M (NVK GM107), schenker, Vulkan on KMS,
NVIDIA GeForce GTX 1050 Ti, schenker, Vulkan on KMS, NVIDIA GeForce GTX 1050
Ti (NVK GP107), thinkpad, Vulkan on KMS, llvmpipe (LLVM 23.1.4, 128 bits)

The split is what one size could not show. On the Pi the window
servers carry a per-frame GPU floor that KMS does not: 3.67 ms on X11
and 3.59 on weston against KMS's 0.27, whose cost is then all per
pixel, 8.61 ms/MP -- the copy path's readback. The laptops' servers
cost a fraction of a millisecond a frame, where the display path is
not the bottleneck. CPU is a per-frame cost everywhere: every CPU
slope is about zero and the intercept 0.2 to 1.2 ms, so the image's
size is the GPU's. On Windows the fixed parts are a few tenths at
most, and Direct3D 12 is the cheaper per pixel of the two renderers on
both GPUs of both laptops -- aspire's HD 530 0.92 against Vulkan's
1.57 ms/MP, its 960M 1.13 against 1.70 -- where schenker's 1050 Ti is
the cheapest of all at 0.27 to 0.33.

## From latch to screen, 2026-10-31

The testbed's bench (`tools/drive/bench.py`, `--bench-measure-s=8`, vsync
on, the window at rest at 1280x720 points), the median from a frame's
latch to its shown stamp. The other systems' figures are DESIGN
*Benchmarking*'s, with the stamps each takes.

| the laptops on Windows | Vulkan, through DXGI | Direct3D 12 |
|---|---|---|
| aspire HD 530 | 20.3 ms | 20.3 ms (through Direct3D 11) |
| aspire GTX 960M | 20.2 ms | 19.9 ms |
| schenker HD 630 | 20.1 ms | 20.2 ms |
| schenker GTX 1050 Ti | 20.1 ms | 19.8 ms |

## Frame costs and the GPU ranking, 2026-10-28

What the cost figures predict against what is drawn, before and after
the spans were made one (`docs/comparable-frame-costs.md`, DESIGN *What a
frame costs, compared*). The testbed, its scene moving, release builds.

Drawn flat out (`FNL_VSYNC=0`), the predicted rate `1000 / max(GPU,
CPU)` over the rate presented, from `FNL_COST_PARTS`'s lines; the ratio
should be alike across renderers, since the halves overlap alike:

| place | renderer | drawn | ratio before | ratio after |
|---|---|---|---|---|
| this desktop, Wayland 1280x720, RX 590 | Vulkan | 7407 | 1.74 | 1.40 |
| | OpenGL | 6122 | 1.89 | 1.35 |
| schenker, KMS 1920x1046, GTX 1050 Ti | Vulkan | 387 | 1.44 | 1.46 |
| | OpenGL | 588 | 10.2 | 1.01 |
| schenker, KMS, HD 630 | OpenGL | 162 (102 before) | 5.03 | 1.40 |

Vulkan on the HD 630 takes the panel and has no tearing mode there, so
it draws at 60 whatever is asked and is not in the table. OpenGL on the
GTX 1050 Ti is CPU-bound alone, where the larger of the two halves is
exact. OpenGL's copy path draws faster after: its readback into a pack
buffer no longer holds the render thread.

The GPU ranking, `FNL_GPU_RANK=calibrate`, frames a second:

| place | before | after |
|---|---|---|
| schenker, KMS eDP-1 1920x1080@60 | OpenGL GTX 1050 Ti 1600, Vulkan GTX 1050 Ti 559 | Vulkan GTX 1050 Ti 412-414, Vulkan HD 630 357-415, OpenGL GTX 1050 Ti 249-251, OpenGL HD 630 188 |
| this desktop, Wayland | OpenGL 3615, Vulkan 2840 | Vulkan 2565, OpenGL 2029 |
| aspire, Windows 1920x1032 | -- | D3D12 GTX 960M 1337, Vulkan GTX 960M 1105, Vulkan HD 530 864, D3D12 HD 530 371 |

Two calibrations on schenker, visiting the entries in different orders,
agree within 1% but for Vulkan on the HD 630 (357 and 415). With vsync
on, both renderers' CPU half on the GTX 1050 Ti is about twice what it
is flat out -- the copy at a low CPU clock -- which is why the
calibration puts Vulkan first there while OpenGL is the faster flat out:
the ranking measures the scene at the display's rate, as DESIGN *Ranked
by measured frame rate* says it does.

## Reading them

- **A size costs three to four frames under the panel's rate.** A window
  draws nothing when nothing changes, so a size is the frame that shows
  it and `REST_SETTLE` after (DESIGN *At rest*): 19 to 20 fps at 5 sizes
  a second and 40 to 48 at 15, on every machine counted. From 30 a
  second every refresh has a new size and the rate is the panel's.
- **The KMS window refuses every size**, being the output, so its rate rows
  read 0 fps and say only what asking costs. Its `draw` row is the one that
  times a frame there.
- **Every environment holds 55 to 62 from 30 sizes a second**, on every
  GPU and every system, the Pi's weston too, which drew 30 in the last
  set. NVIDIA's own driver on Xorg with no window manager holds 57 to 59
  and its border 55 to 57, where Intel and NVK hold 60.
- **Windows sizes at the panel's rate now**, both renderers on all four
  GPUs: the windows at rest are flipped onto a plane where DWM composed
  them, Vulkan through DXGI has three buffers and Direct3D 12 queues two
  frames while a window is sized (DESIGN *Vulkan through DXGI*). Vulkan
  on the Intel GPUs, which fell to 33-43 fps at three quarters of a core
  in the last set, holds 60 at under 11%, and its border went from 15-19
  to 59-60. Direct3D 12 sizes at 3 to 14% of a core, Vulkan through DXGI
  at 6 to 21%.
- **weston on thinkpad draws at 30** with nothing asked, where DESIGN
  *KMS: no window server* measured 60 fps at 61.8% for the `draw` row,
  and at 58 to 60 under sizes.
- **weston's CPU under sizes is a fifth or less of the last set's** on
  the Intel, NVIDIA and NVK GPUs (4 to 10% from 30 sizes a second, where
  it was 15 to 48), and this desktop's Wayland under Vulkan about three
  fifths; its OpenGL and every Xorg did not move. It is `8de7465`, a
  size inside the images drawn with no new swapchain on Wayland:
  schenker's weston at 27.0% from 30 sizes a second the commit before
  and 5.6% with it, built side by side (DESIGN *A swapchain larger than
  the window*).
- **wine with Vulkan costs what the native program does, and a little
  more on weston**: its rates match the `linux` tree's row by row on the
  thinkpad, at up to 8 points more of a core on Xorg and 7 to 40 more
  on weston, and its border 36-37 against 39-40. cage refuses every
  size as KMS does, being a kiosk. Under Direct3D 12 on vkd3d-proton
  the machine's two cores are the limit: 46 to 53 from 30 sizes a
  second at more than a core, a resting window still 11 to 77% (the
  77 the first sweep, on a prefix made that boot), and the two sweeps
  apart by up to 14 fps on weston where every other column agrees
  within 2. `draw` under wine on Xorg presented 70 to 72 a second on a
  60 Hz panel.
- **CPU across columns compares the machines, not the environments.**
  aspire and schenker cost about the same now on every server, as they
  did in the last set.
