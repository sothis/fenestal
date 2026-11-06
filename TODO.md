# TODO

What is owed, as opposed to what is known. `DESIGN.md` is the record of
what was measured; an item answered there, with its measurement, comes off
this list.

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
  schenker's are 125%. Under wine a maximized window on a monitor with no
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
below, x11-gl 106, wayland-gl 74, win64-vulkan 89, win64-d3d12 93 and
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
- **KMS's per-group key types** (`key_cut()`): a two-layout console
  (`FNL_KMS_LAYOUT=us,de`) typing each layout's third level where its type
  says, on the thinkpad, and an option that augments a key (`compose:ralt`)
  leaving its group's levels. Seen on the thinkpad (2026-11-05): the
  round's `de,us grp:alt_shift_toggle` console typed 17 sequences, the
  third levels among them, and the group key switched Y both ways; a
  `--layout='de,us compose:ralt'` run parses and draws (the log names it),
  but no check types through that option yet -- owed, an option that
  augments a key and the levels still typed through it.
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
  touch.
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
- **OpenGL's presented count** (`gl_bridge_scan_show()`): a frame with
  no drawable on macOS, or no buffer on KMS, not counted, and nothing
  else then changed.
- **The Vulkan bail path** (`frame_bail()`): a recording forced to fail,
  the swapchain made again and the window drawing on, with no
  validation message.
- **The swapchain's present wait from the waiter thread**: the spec
  makes `vkWaitForPresentKHR`'s swapchain externally synchronised, and
  `shown_main()` waits beside the render thread's acquires and presents.
  Asked of the validation layer's thread checks, and of drivers beyond
  Mesa's, whether that holds or the wait wants the lock in slices.
- **OpenGL's GPU list against its pick** where the server's display is
  on none of the listed devices (`gl_bridge_list_gpus()` lists all, the
  pick one): which the list should say is a decision, not yet made.
- **`fnl_d3d12.h` compiled alone** (`FNL_CHECK_D3D12` in
  `tools/headercheck.c`): the win64 tree's first build, as C11 with
  `COBJMACROS` and as C++11 to C++20, against mingw's `<d3d12.h>`.
- **X11's sync value kept for its frame, run out at a drag's first press**
  (`SYNC_SHOWN_WAIT`, `x11_present_event()`): `x11-vulkan`'s stress round
  says so once in release and in `gc`, `pace.py --border` never (DESIGN *X11:
  xcb, not Xlib*). Logged (`FNL_X11_PRESENT_LOG=1`): the in-order match is
  about five frames behind once swapchains have been made again, frames
  queued that are never answered -- Mesa's, presented to a swapchain retired
  before it sent them. Matched by serial now (DESIGN): `x11-vulkan` and
  `x11-gl` here clean (OpenGL's 3,383 completions 17.5 ms after the push at
  the median, 3 frames dropped), and `x11-vulkan-bare` and `x11-gl-bare` on
  the thinkpad, aspire and the Pi (2026-11-02). The shown stamps read back
  through sizes (`pace: shown`): right on `x11-vulkan`, and on `x11-gl`
  since a stamp from before its present is moved to the next refresh
  (DESIGN *X11: xcb, not Xlib*), and on the bare machines' Xorg (the
  thinkpad, aspire, the Pi). The stamp before its latch on aspire was
  NVIDIA's out-of-date present shown anyway (DESIGN), counted now; the
  rounds after it clean (2026-11-02: `x11-vulkan-bare` and `x11-gl-bare` on
  aspire, 96 checks each; `x11-vulkan` 106 and `wayland-vulkan` 74 here).
  Wayland's take-back answers the same where the compositor has spoken
  first (DESIGN); its rounds clean (2026-11-02: `wayland-vulkan-bare` on
  aspire, 66 checks; `wayland-vulkan` here, 74).
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
  X11, weston and KMS, Vulkan and OpenGL (aspire, the thinkpad, the Pi:
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
  `si_begin_new_gfx_cs()`) when the GPU box moved the window onto OpenGL;
  each is suppressed in `tsan.supp` with its frames named. Owed still:
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
- **OpenGL to OpenGL under a window server** cannot be asked: Mesa (26.0
  here, 26.2 on the thinkpad's Xorg) opens a display asked of the
  software device on the hardware one, so OpenGL lists the hardware GPU
  alone there (DESIGN, 2026-10-31). Owed: the move on GNOME, once a
  Mesa honours the device -- or the software device's display opened
  another way (`LIBGL_ALWAYS_SOFTWARE` in a process of the lister's own).

## Linux

- **OpenGL's resize on Wayland**: the `wl_egl_window` is
  `app_buffer_room()`'s while the size is changing and the window's at
  rest, and the viewport shows the top-left extent (DESIGN *A swapchain
  larger than the window*). What that saves is unmeasured. The Pi's
  `wayland-gl` under weston (2026-10-31, `pace.py wayland-gl-bare@pi4`,
  OpenGL's bare columns now asked for by name) draws on llvmpipe, since
  V3D 4.2 has GL 3.1: 19, 29 and 30 fps at 5, 15 and 30 sizes a second,
  and 30 from there to 250, at 175 to 179% of a core -- its draw row, no
  sizes at all, 30 fps at 209%. So the ceiling is the rasteriser's, and
  beside it the EGL window's remaking costs nothing to be seen; Vulkan
  on the V3D, same sweep, 56 to 58 fps from 30 sizes a second. Owed: a
  pace row on a GL 4.5 Wayland device, where the draw is faster than the
  remaking.
- **A dip every 21 s on this desktop's console at 4K**: 44 to 48 frames
  a second for about 3.5 s, then 60 again, the RX 590 on DP-1 at
  3840x2160. Not the library's: `vkcube --wsi display`, with none of it,
  dips the same (4.5, 26 and 47 s, Mesa's overlay layer logging), and so
  does the library's OpenGL, which flips through the backend's own page
  flips and no Mesa display WSI; the kernel's connector polling off
  (`drm_kms_helper.poll=N`) changes nothing, and no DRM uevent comes with
  it. What it is wants hands: another monitor on DP-1, the dark DVI-D-1
  unplugged, amdgpu's DC debug. Seen as "45 frames a second without
  vsync" before: the display path has no tearing mode there, so vsync off
  runs FIFO at 60 between the dips. Since the period estimate holds there
  (DESIGN *Late latching*) the dip costs frames of 17.8 to 18.5 ms, not
  21 to 25.
- **A report to NVIDIA's egl-x11**, drafted in `docs/nvidia-egl-xcb.md`
  and not sent: `EGL_EXT_platform_xcb` is listed and every xcb display
  refused, `EGL_NO_DISPLAY` with no error, where the same server through
  Xlib is taken -- on Intel's Xorg under PRIME offload, and on one
  NVIDIA's driver drives, `__NV_FORCE_ENABLE_X11_EGL_PLATFORM` or not. It
  is why NVIDIA's OpenGL never draws an X11 window here. egl-x11's source
  declines a server with `NV-GLX`, or with DRI3 or Present below 1.2, in
  silence, which may be the whole of it; owed on schenker before sending,
  the four checks the draft's source section lists (2026-10-31).
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
    move, under the checker trees: schenker's, Vulkan's and OpenGL's on
    both drivers, under `asan` and `gc`, 97 checks each and none failed
    (2026-10-31, `b4571d7`), OpenGL's move under `asan` among them;
    aspire's Vulkan rounds on both drivers under `asan`, 97 checks, none
    failed, the same day, and under `gc` likewise; and aspire's OpenGL
    ones on both drivers under `asan` and `gc`, 97 checks each, none
    failed (2026-10-31, `1d987ac`, the ranking set aside, as a ranked
    machine skips them as the Vulkan round's twins); and `tsan` on
    nouveau passes on both, the drivers preloaded and Mesa's threaded
    context off (DESIGN, 2026-10-31). Owed: a Vulkan round there under
    `tsan` loses F11 now and then and then the window (three times on
    2026-10-31, on schenker and aspire, each passing when run again, no
    TSan report in any): NVK on the copy path draws 14 to 30 fps under
    TSan, and the round's fullscreen waits may be what is short.
  - *The harness* knows the driver (`FACTS`, `discrete_sleeps()` by
    NVIDIA's "Runtime D3 status", the closed ICD in `DEBUGINFO`, `__NV_`
    and `__GLX_` forwarded, the ICD kept loaded under ASan and
    `lsan.supp` keyed on its two libraries, a `tsan` round on it
    skipped with the reason). ASan needed no `protect_shadow_gap=0`.

## The bare machine, without Xorg

- **A report to weston**, drafted in
  `docs/weston-held-update-xdg-state.md`: a content update held for a
  FIFO barrier is applied with xdg_surface state sent after it (DESIGN
  *Wayland*; worked around here), reproduced alone by
  `tools/probe/heldgeom.c`; main read and the tracker searched
  (2026-10-17, and again 2026-10-31: still there, the same in 16.0.2,
  not reported). Owed: `heldgeom` on 16.0.2, the current release; then
  sending it.
- **A window lost to the harness on weston after an edge resize**
  (2026-10-31): wine's Direct3D 12 round on weston on schenker's nouveau
  entry, 13 checks in, the program drawing at the size the drag left; run
  again on that boot it passed, and weston's own Vulkan round beside it.
  Owed: `tools/probe/heldpresent.sh` (built here and run on the machine's
  weston by `bare_run.py --weston`) the next time it happens, before the
  round is run again. The NVK losses of 2026-10-16 were the harness's
  sweep (DESIGN *The startup inventory*).
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
  three laptops (DESIGN *Input is evdev*); the feel is not.
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
- **The unplug section with a force that reaches the program and two
  outputs**: no machine has both today -- `black`'s kernel sends no uevent
  for a forced status (DESIGN *KMS*), and the laptops have one output each.
  A second monitor on a laptop, or a cable pulled by hand on `black`.
  The same reaches the away grace's other half (DESIGN *A monitor that
  leaves is away first*), driven only with monitors that came back: a
  dark output pulled leaving the list after 5 s, the window's own pulled
  moving it after 2.5 s, and a move owed to an output that does not come
  back dropped with its line.

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
  Vulkan or GL files (HTTP 404 for all five, `bare_driver.py --setup`;
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

Cocoa beside Win32, Wayland, X11 and KMS, and Metal beside Vulkan, GL and
Direct3D 12, on schenker's macOS stick (DESIGN *macOS*): the `macos` tree,
cross-compiled here (DESIGN *Cross-compiling for macOS*).
Written: the backend (`src/backend/cocoa/`), the Metal renderer
(`src/renderer/metal/`), Vulkan's bridge on MoltenVK (`surface_cocoa.c`,
with the portability extensions), `fnl_metal.h`, the testbed's and a
snippet's scenes, the toolchain, `build.sh`'s `macos` target, the checks'
tables, and the harness's environments for the Mac (`mac_driver.py` and its
agent; `macos-metal-native`, `macos-vulkan-native` and `macos-gl-native`,
and three `pace.py` columns). Owed, in this order:

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
   PL4071UH and back under Metal, Vulkan and GL, 95 checks each and none
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
`Contents/Frameworks` under Vulkan (dyld's own line), and Vulkan, Metal
and GL each drawing a complete record at 60 fps from the bundle; and
`./build.sh macos-arm64` built here, an arm64 Mach-O, its first on this
desktop. CI builds both trees and joins them with `lipo` into the
ad-hoc signed bundles, MoltenVK in one only where its program has a
Vulkan scene (`.github/stage.sh`). Owed, in this order:

1. **The arm64 half run anywhere**: no Mac here has Apple's processor.

MoltenVK is staged only where a scene is Vulkan's: the testbed and
every demo but hello carry `scene_vulkan.c`, fenestal-dev has none, and
CI's zip of 2026-11-05 lists exactly those six bundles with it, at 43.9
MB against 50.5. A bundle without it runs on the Mac -- `hello.app`
opened under Metal and GL, its log saying `Vulkan lists 0 GPUs here`,
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

## OpenGL on macOS

Written and driven on schenker's macOS (DESIGN *OpenGL on macOS*,
*OpenGL 4.1*): the 4.1 profile, its translation, the caller's half, the
CGL bridge, OpenGL in the `macos` tree and `macos-gl-native`. Owed:

- **The bridge on a Mac with two GPUs**: whether the Metal device found
  by the GL context's registry id is the one GL draws on, where there is
  more than one to match (DESIGN *OpenGL on macOS*).

## Direct3D 12 and OpenGL 4.5

Two renderers beside Vulkan: D3D12 on Win32, and GL 4.5 core on Linux --
EGL on Wayland, X11 (`EGL_EXT_platform_xcb`; GLX would bring Xlib) and KMS
(Mesa's surfaceless display). Each is in every tree of its system beside
Vulkan, chosen at `fnl_init()` (DESIGN *One build per operating system*):
the `linux` tree draws with Vulkan and OpenGL, `win64` with Direct3D 12
and Vulkan. `libEGL` and `libwayland-egl` are opened at run time, never
linked, and Direct3D 12 is Windows' own.

In this order, each step leaving every round passing. Done: the Vulkan
renderer's state behind `App::renderer_state`, with no file of `src/core/`
excused by `check-layering.py`; `on_record`'s contract, which `fenestal.h`
states for every renderer (DESIGN *Recording*); and one shader source,
translated by `spirv-cross` and checked at build time (DESIGN *One shader
source*).

1. **GL on X11**, the cheapest to drive and capture here; then Wayland
   and KMS. The GL renderer (`src/renderer/gl/`, EGL over core xcb),
   `fnl_gl.h`, the testbed's scene and a snippet are written, the `linux`
   tree builds them, and the harness drives it as an environment of its
   own (`x11-gl`). **Wayland** (`egl_wayland.c`, the `wayland-gl`
   environment, `WaylandGL` in the harness) is written too. Both pass the
   stress round, with and without `--capture`, a pace sweep, and every
   checker tree (DESIGN *Wayland*, *X11*), and a Debug tree's GL debug
   output is shown to reach the log (`FNL_SELFTEST=gl`). **KMS**
   (`egl_kms.c`, surfaceless, the frames blitted into the backend's
   scanout buffers, the copy path where that is refused; the
   `kms-gl-bare` environment, `KmsGLBare`) passes on the three laptops (DESIGN
   *KMS*), and has a pace column (`kms-gl@aspire`, 2026-10-31).
   GL is on every Linux backend.
2. **D3D12 on Win32** is written (`src/renderer/d3d12/`, the `win64-d3d12`
   environment, `fnl_d3d12.h`, a scene for the testbed, and a snippet):
   DXGI flip discard, the frame-latency object waited on before each frame,
   sync interval 1 or 0 with `ALLOW_TEARING`, and DXIL from `dxc` (DESIGN
   *One shader source*). It records afresh every frame, as
   GL does (DESIGN *Recording*). It passes the stress round under wine on
   vkd3d-proton, on X11 and on Wayland (release and `gc`), in both guests
   on WARP, and on both laptops on each GPU; the `analyzer` tree builds
   clean. The frame-latency wait is skipped on the frame that answers a
   size (under wine on Wayland the two waits held each other to their
   bounds on every maximize) and under sync interval 0 (on Windows it held
   vsync-off to the refresh rate: 8 to 22% of a core flat out, 17 to 63%
   once skipped); see `frame.c`.
3. **The harness**: four environments (`win64-d3d12`, `-wl`, `-vm`,
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
  each size. (OpenGL on X11 was the same and is answered: the ack held
  until Present shows the frame, 48 to 49 fps, DESIGN *X11*,
  2026-11-01.) Owed: the `--frame-times` lines' `other` under wine, to
  name the gap.
- **Resizing under wine** shows the new size before its frame: wine's X11
  window has no `_NET_WM_SYNC_REQUEST` (DESIGN *X11*). Recorded now
  (`tools/drive/screencast.py`): black margins. A later wine, or wine's
  Wayland driver, is where it closes.
- **NVIDIA's OpenGL frames onto an Intel panel** stay a copy (DESIGN
  *KMS: no window server*): a renderbuffer over the
  imported buffer is refused, and a texture over it is drawn into a copy
  of NVIDIA's own and crashes its EGL when let go (DESIGN). A buffer that
  the NVIDIA GPU allocates and the Intel GPU scans out would be the other
  way round, and wants GBM on NVIDIA's side.
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
  start that names nothing opened on the first (NVIDIA's OpenGL), with
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
  and again booted on NVIDIA's driver, alike. On a ranked machine
  `kms-gl-bare@<m>:<driver>` is that round's twin and skipped. Unranked
  (schenker's ranking deleted 2026-10-26), `kms-gl-bare@schenker:nouveau`
  and `:nvidia`, 161 checks each, none failed, the walk on both displays.
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
  window was moved there (33 s, the 4K output reordering the list: Intel's
  OpenGL ahead of NVIDIA's Vulkan); a start with HDMI unplugged and one
  with it back measured nothing, every figure kept. With the window open,
  HDMI pulled and pushed measured nothing; pulled while the window was
  measured on it, the calibration ended with 3 of 8 kept and the window
  on the panel; pushed back and the window moved there, the 5 missing
  measured (21 s). A move between displays put the window on the
  display's first entry: on HDMI on Intel's OpenGL, moved to the panel,
  it went to NVIDIA's OpenGL by itself, nothing measured. A calibration
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
