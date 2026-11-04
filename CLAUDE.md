# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

**Fenestal** is a portable C11 library — a window, a device, a swapchain, a
render thread and a text overlay, wrapped around a scene that is the caller's.
Its backends are Wayland, X11, Win32, Cocoa (macOS, cross-compiled here and
driven on a Mac over ssh) and KMS -- a Linux console with no window server, the
window being the output; its renderers are Direct3D 12, OpenGL (4.5 on Linux,
4.1 on macOS), Metal and Vulkan; and a platform is one of each, such as Vulkan
on X11. The GPU is the device that draws, hardware or a software rasteriser. A
build has every backend its system has -- on Linux, Wayland, X11 and KMS, the
one a program opens chosen at `fnl_init()` or named by `FNL_BACKEND` in its
environment -- and every renderer its system has, chosen the same way or named
by `FNL_RENDERER` (DESIGN *One build per operating system*); every runtime that
is not the system's is opened at run time, never linked. It makes
`libfenestal.a` and a shared object, with `testbed/` as the application the
library is developed with.

The seam is the `on_record` callback; see `testbed/scene_vulkan.c`.

`demos/` holds the programs to copy (`demos/README.md`), each a folder
whose `CMakeLists.txt` configures on its own (`add_subdirectory` on this
tree, `fenestal::static` and `fenestal::fenestal`) and is also added by the
top-level build, so every tree builds each demo through a consumer's own
lines. They came from the `fenestal-demo` repository with their history
(merge `eac0ad1`); that repository is retired. `examples/*.c` are the API
reference's snippets.

`DESIGN.md` is the spec — it records the measurements, the rejected
alternatives, and the reasoning behind nearly every constant. Read the relevant
section before changing behaviour it describes. A comment's `DESIGN *Name*`
names a section there. `README.md` is only the user's introduction: build,
consume, the API reference, compatibility; detail goes in `DESIGN.md`.

`TODO.md` is what is owed rather than what is known: platforms not yet tested
and the questions left open for them. A question it lists is answered in
`DESIGN.md`, with the measurement, and then taken off the list.

## Working agreement

- **Do not configure, build, run, or capture unless asked in that turn.** That
  includes `cmake --build`, `./build.sh`, launching a program, screenshots,
  and the sanitizer / `-fanalyzer` trees. Edit and reason; the user drives
  verification.
- Commit directly to the checked-out branch when asked. Push by hand and
  only when asked: one remote, `origin`, fetched from the NAS
  (`CLAUDE.local.md` says where) and pushed to two URLs, the NAS first and
  then the public `sothis/fenestal`, so a bare `git push` sends `main` to
  both (`remote.origin.push` names `main` alone). GitHub gets `main` and
  nothing else -- never `history`, `--all` or `--mirror`; the local
  `pre-push` hook refuses any other ref there. GitHub's CI
  (`.github/workflows/ci.yml`) builds each target's release tree on a
  push to `main` and on a pull request -- macOS on a Mac runner, against
  its Xcode's SDK -- and runs ruff; it builds no checker tree and opens no
  window. This tree's own work takes no PR flow — do not propose one;
  pull requests are how others send changes (`CONTRIBUTING.md`).

## Build

```sh
./build.sh                        # every tree -- thirteen, and the ARM
                                  # Linux target's four where its cross
                                  # compiler is
./build.sh linux                  # that target, release: Wayland, X11 and
                                  # KMS, drawn with Vulkan and OpenGL
./build.sh win64                  # that target: Win32, drawn with
                                  # Direct3D 12 and Vulkan
./build.sh macos                  # that target: Cocoa, drawn with Metal,
                                  # Vulkan on MoltenVK and OpenGL 4.1
./build.sh macos-arm64            # the macos tree for Apple's own
                                  # processors, cross-compiled; release
                                  # and gc
./build.sh linux-arm64            # the linux tree for the Raspberry Pi 4,
                                  # cross-compiled; release, asan, tsan
                                  # and gc
./build.sh linux asan             # that target, that checker
./build.sh linux clang            # that target, built by clang
./build.sh dist                   # the two source zips alone, from the
                                  # working tree as it stands
```

Every tree lands under **`build/<target>/<variant>`**. Targets are
`linux`, `win64` and `macos`, one for each operating system; variants are
`release`, `clang`, `asan`, `tsan`, `analyzer` and `gc`. Each tree holds
`libfenestal.a`, the shared object and two programs -- `fenestal-testbed`
and `fenestal-dev`, the development instruments, whose modes are `pace` and
`lifecycle` -- and each demo's two, `<name>` and `<name>-dyn`, as `.dll` and
`.exe` in the cross tree.

- **`linux-arm64` is the `linux` tree for 64-bit ARM** (the Pi 4), built
  here by Ubuntu's `aarch64-linux-gnu-gcc` with its own glibc
  (`cmake/linux-aarch64.cmake`); the runtimes' headers are the host's,
  and nothing links the loader. It takes
  release, asan, tsan and gc, and a checker round there runs on the Pi's
  own sanitizer runtimes, never Ubuntu's cross ones (`_runtimes()` in
  `bare_driver.py` says why); the sweep builds it where that compiler is
  installed and says it skipped it where not. It borrows
  `build/linux/release`'s bake as the other cross trees do. It builds
  `verifykeymap` and the fuzz harnesses for ARM and runs none:
  `tools/probe/bare_checks.py` runs them on the Pi, from the commands the
  tree writes to `generated/checks.txt`. DESIGN *Cross-compiling for ARM
  Linux*.

- **A Linux tree has Wayland, X11 and KMS.** A program opens the first its
  session has -- Wayland where `WAYLAND_DISPLAY` is set, X11 where
  `DISPLAY` is, KMS where neither is -- stepping past one whose `open()`
  says `FNL_ERR_NO_DISPLAY`, and logs `backend: <Name>`;
  `FNL_BACKEND=x11` in its environment opens that one or fails. The
  harness names one for every round, and so should a command run by hand
  here, since this desktop has both variables. The old per-backend
  target names are refused with the new one. `-DFNL_BACKEND=x11` after
  the target builds one backend alone (`linux` is the default), and
  `-DFNL_RENDERER=vulkan` one renderer alone (`all` is the default).
- **Every tree has every renderer its system has**: Direct3D 12 and Vulkan
  on Windows, Vulkan and OpenGL on Linux, Metal, Vulkan (on MoltenVK)
  and OpenGL on macOS. A program passes the renderers it has a scene for
  (`FnlInitDesc::renderers`; `FNL_RENDERERS_ANY` for none), and
  `fnl_init()` tries them in that order on the open window, stepping past
  one that fails, and logs `renderer: <Name>`; `FNL_RENDERER=gl` in its
  environment draws with that one or fails. The harness names one for
  every round, as it names the backend. The old per-renderer target names
  are refused with the new one. `FNL_GPU` in a program's environment is
  the *device*.
- A bare `./build.sh` sweeps every valid target/variant pair: `linux`,
  then `linux-arm64` (where its compiler is), then `win64`, then `macos`,
  then `macos-arm64`, and `release` first within each — the cross
  trees borrow `build/linux/release`'s baked atlases, by that name, so that
  order is what keeps the sweep self-sufficient. It stops at the first
  failure.
- The cross targets need `linux`'s release tree first: `tools/mkfont`
  links FreeType and
  runs *during* the build, so a cross build would compile it for Windows and
  then fail to run it. `build.sh` borrows that tree's atlases and says so if
  it is missing, older than what it is baked from or not yet passed by
  `verifyfont` (`FNL_VERIFY_FONT` off), the test a checker tree's lender
  passes. A native target bakes in its `release` and
  `analyzer` trees;
  its `clang`, `asan`, `tsan` and `gc` trees borrow the release tree's bake
  when it is there and fresh (`FNL_FONT_DIR`), and bake their own when it is
  not.
- mingw-w64 has **no sanitizer runtimes**, so `win64` refuses `asan` and
  `tsan` and takes `release`, `analyzer` and `gc` — both of the latter are
  compile- and link-time passes needing no runtime. Its `gc` tree also
  carries UBSan in trap mode, which needs none: the one runtime check
  `win32.c` runs under. It refuses `clang` too:
  clang against mingw-w64 wants a sysroot of its own. Thirteen trees: six
  variants of the native target, three of the Windows one and two of each
  macOS one.
- **The macOS tree is cross-compiled here**, by clang-21 and `ld64.lld`
  (`lld-21`) against Apple's SDK, which `tools/macos_sdk.py --fetch` copies once
  from the Mac (schenker's stick, `ssh schenker-mac`) into
  `~/.local/state/fenestal/macos-sdk/`; they borrow the host tree's bake as the
  Windows tree does and takes `release` and `gc`, the Debug tree there
  (`-dead_strip` where a GNU linker takes `--gc-sections`). `src/backend/cocoa/`
  and `src/renderer/metal/` are **Objective-C under ARC**, with GL's
  `cgl.m` the library's only other; the archive's one object is made from
  the library's bitcode (`llvm-link`, `opt`'s internalize), since `ld64.lld`
  has no `-r`. MoltenVK
  goes beside the programs (`tools/moltenvk.py`), where the library opens it and
  hands it to the examples' Vulkan scenes. DESIGN *Cross-compiling for macOS*,
  *macOS*. **`macos-arm64` is the same tree for Apple's own processors**
  (`cmake/macos-arm64.cmake`: the x86-64 file, included with
  `FNL_MACOS_ARCH` set), built here and run nowhere here, since the one
  Mac is an Intel one; CI joins the two trees into universal binaries and
  app bundles (`.github/stage.sh`).
- **`clang` is the release build compiled by clang** (`clang-21`, or
  `FNL_CLANG`), optimised, with `-Wthread-safety` and `-Werror`
  (`FNL_WERROR`). It is what makes warning-clean under clang a check.
- **Optimised builds are hardened** (`FNL_HARDEN`, on by default):
  `-fhardened` where gcc has it, and otherwise each part the compiler takes
  that links without a new library. `analyzer` turns it off. The Debug
  checker trees do not get it.
- `CMAKE_BUILD_TYPE` **defaults to Release** (cache-`FORCE`d only when nothing
  was chosen, so the checker trees' `Debug` still wins) and the compiler is
  **pinned to gcc** before `project()`. Both are deliberate; `CC=clang` wins,
  and the `clang` variant passes it.
  `analyzer` chooses no build type, so it is the one checker tree that is
  optimised.
- **A build that succeeds packs `build/fenestal.zip`** (`tools/mkdist.py`,
  from `build.sh` and not from CMake), comments stripped, and
  `build/fenestal-commented.zip`, the same files with them; a pack that
  fails fails the build. Those take the git index's files, read from
  disk; `./build.sh dist` packs both alone, with no build, from the
  working tree as it stands -- untracked files in, deleted ones out,
  modes from disk (`mkdist.py --worktree`).
  Its manifest is an allowlist (`NEEDED` there): `docs/`, `tools/` and
  the top-level `*.md` go in whole, and
  any other new top-level file is out until it gets a row.
- **A build that succeeds is followed by `tools/check-layering.py`**, once
  for a sweep, and a violation fails `build.sh` with the trees built. So is
  `tools/check-tables.py`: every `FnlKey` in the three key tables and
  `KEY_NAMES`, every event, button and cursor named or mapped, and every
  `FnlDrawState` field compared in `fnl_draw_state_eq()`.
- `./run_all.sh` starts the release testbed four ways at once with the
  same arguments -- the Linux tree on Wayland and X11, the Windows tree
  under wine with Direct3D 12 and Vulkan -- and builds nothing.
- Clean means `rm -rf build`. `build/` is one gitignore entry for every tree,
  and `build.sh` has no clean mode. It also takes `build/drive/` (the
  harness's logs and pictures) and `build/run/` (`run_all.sh`'s logs).

### Running and driving

- **The X11 backend can be run and driven here** (`FNL_BACKEND=x11` on
  the `linux` tree). It is a real X11 window on
  XWayland, so `xwininfo -root -tree` finds it and `import -window <id>`
  captures it. `tools/drive/stress.py` is the round, and it drives every
  environment rather than one: three builds, and twenty-seven ways of
  running them (`ALL` in `environment.py`), each below; every one names
  its renderer to the program by `FNL_RENDERER`, and every Linux one its
  backend by `FNL_BACKEND`.
  Unlike the native Wayland window this one can be captured directly,
  which makes it the cheapest way to *look* at a frame. `x11-gl` is the
  same window drawn with OpenGL, an environment of its own (`X11GL`) that is
  X11's in everything but `RENDERER`; a checker round
  there has no Vulkan layer to prove loaded, and GL's debug output, which a
  Debug tree logs, is what the log check reads instead. `wayland-gl` is the
  same for the Wayland window (`WaylandGL`), and `x11-gl-bare` and
  `wayland-gl-bare` are the same on the laptops' Xorg and weston
  (`X11GLBare`, `WaylandGLBare`), and `kms-gl-bare` the third with neither
  up (`KmsGLBare`); `FNL_GPU` does not steer GL, so the
  harness forwards Mesa's `DRI_PRIME` and `LIBGL_*` there instead.
  OpenGL is Linux's and the Mac's (`macos-gl-native`, 4.1 through CGL).
  The four Win32 environments have Direct3D 12
  twins, the same `win64` tree told `FNL_RENDERER=d3d12`: `win64-d3d12`,
  `-wl`, `-vm` and `-native`. Its shaders are DXIL from Microsoft's `dxc`
  (`tools/dxc.py`, pinned, which `build.sh` names as `FNL_DXC`), and wine
  10.0's own vkd3d runs no DXIL, so the two wine environments run on
  vkd3d-proton with DXVK's `dxgi.dll` (`tools/vkd3d_proton.py`, pinned),
  staged with a copy of the program under `build/drive/` and loaded by
  `WINEDLLOVERRIDES`; never put those DLLs into a tree, where on Windows
  they would shadow the system's. The guests draw on WARP.
- **The Windows build can be run here, under wine.** Launching
  `build/win64/release/fenestal-testbed.exe` opens a real X11 window on
  XWayland; Vulkan goes through winevulkan onto the same RADV the native build
  uses. Same harness. Wine is not Windows and the code says so where they
  are known to differ.
- **And under wine on the compositor itself.** `win64-vulkan-wl` is the same
  binary with `DISPLAY` unset, which is all it takes for wine to draw
  through `winewayland.drv`; an environment and not a build, like the
  guest. The two wine environments share a wine server, so each waits for
  the other's to exit before it starts (`wine_clear()` in
  `environment.py`) — a program started sooner gets the wrong driver or no
  window. Wine 10.0's Wayland
  driver has no clipboard, and the round skips that section for it -- by
  looking for `wl_data_device` in the loaded `winewayland.so` and not by
  version, so a later wine turns the section on by itself.
  `FNL_DRIVE_WINE=stable` (`devel`, `staging`, or a path) puts the four
  wine environments on WineHQ's build under `/opt` instead, in a prefix of
  its own under the state directory, with each wine declaration held
  only for the wines it was seen to hold for (`WINE_HELD`) and the version
  in the ledger; `FNL_DRIVE_VKD3D=wine` puts
  the d3d12 ones on wine's own vkd3d. Never point a later wine at
  `~/.wine`: it upgrades the prefix past 10.0.
- **The Windows build can also be run on Windows, in a VirtualBox guest.**
  `win64-vulkan-vm` is a harness environment and not a build: the same `win64`
  binary, copied into the machine each round and driven at its emulated keyboard
  and mouse (`tools/drive/vbox_driver.py`), with `windows_agent.ps1` on the
  guest answering what only that desktop knows. No portal is involved. The guest
  has no GPU -- lavapipe draws the frame -- so it speaks for Windows' window
  manager and input and never for frame rates; `pace.py` leaves it out. Each
  guest is made to show two monitors before a round (`want_monitors()`, by
  `setscreenlayout`), since Windows often comes up on one and the move between
  outputs then skips without a word. Start a guest by hand with
  `tools/drive/vbox_up.py`, never a bare `startvm`: it starts each with its
  window, waits for the logon, connects the second monitor when it is missing
  (one cold start in two), holds the windows in place while Windows boots,
  reopens a screen's window VirtualBox left hidden (a reboot: a guest whose
  state was saved and restored refused the foreground to every program after
  it), and tiles the four on the primary monitor with `vbox_tile.py`, `win10`
  above `win11`, first screen left, each window its screen's aspect (the windows
  are scaled; a cell's shape would stretch the guest). Shut them down or reboot
  with `tools/drive/vbox_down.py [--reboot]`: `shutdown.exe` by guest control,
  since win11 ignores the ACPI button; never a `poweroff`. A round drives every
  machine `FNL_DRIVE_VM` lists (default `win10,win11`), one after the other. The
  password is `~/.local/state/fenestal/vbox-guest`, or `vbox-guest-<vm>` for a
  machine with its own; a machine that is off, or nobody logged in, is a skipped
  environment. So is one whose monitor is under `SCREEN` and will not be made
  larger: a guest shown in a window has auto-resize off, so that the window's
  size is not its screen's, and its display is then set in the guest.
- **And on a Windows laptop with a GPU of its own, over ssh.**
  `win64-vulkan-native` is the guest's environment with another way in
  (`tools/drive/native_driver.py`): the same agent, which there also sends
  the input (`SendInput`) and starts the program, because a laptop has no
  emulated devices and no guest control. One ssh connection carries the
  files and forwards the laptop's loopback home, and a scheduled task
  (`fenestal-agent`) puts the agent on the desktop. What both machines ask
  of the agent is `windows_driver.Desk`; add a question there, not to one
  driver. The machines are `FNL_DRIVE_NATIVE`'s, named as `~/.ssh/config`
  names them, or else the lines of `~/.local/state/fenestal/native-hosts`,
  so a bare round drives the laptop as it drives the guests. A machine's
  GPUs go after its name there (`schenker intel nvidia`) and are a round
  each, `win64-vulkan-native@schenker:intel`; a target on the command line
  is an environment and as much of `@machine:device` as narrows it. It asks
  first: a laptop a hand was on in the last two minutes
  (`FNL_DRIVE_NATIVE_BUSY`), or one that is off, locked or logged out, is a
  skipped environment.
  `native_driver.py --status <name>` says how a machine is set up and
  `--setup <name>` does what can be done from here;
  `tools/drive/native-windows.md` is the rest. `VK_*` in the environment
  reaches the program there, so the validation layer goes in front of a
  round as it does on this desktop -- and the round stops if the layer is
  not then *in* the program. `FNL_GPU=intel` (or `nvidia`) goes the same
  way and puts the program on that device, whichever program it is: an
  index does not, because the order follows the executable. The round
  says which device it is on and stops if it is not the one named.
  `FNL_DRIVE_WIN_OUTPUT=DISPLAY2` runs a Windows round or sweep on that
  monitor of the machine (`FNL_DRIVE_OUTPUT` is this desktop's).
  `pace.py` has a column for each laptop, and
  `tools/probe/native_run.py` runs one program there and prints what it
  says.
- **And on a Mac, over ssh.** `macos-metal-native`,
  `macos-vulkan-native` and `macos-gl-native` are the macOS tree, told
  each renderer, on schenker
  booted into macOS (`power.py boot schenker macos`), driven as the laptop
  is by an
  agent of their own (`tools/drive/mac_driver.py`, `mac_agent.m`, built
  here by the cross toolchain and copied over each round). ssh is the
  whole way in: a program started from an ssh session reaches the window
  server, so the agent is the last command of one session and the program
  of another, whose stderr is the echo. Input is posted `CGEvent`s and the
  picture is `screencapture`, matched to sRGB on the Mac, by ColorSync.
  Both want grants to `/usr/libexec/sshd-keygen-wrapper` that only a hand
  on the Mac can give -- Accessibility and Screen Recording -- and a round
  that finds either missing is a skipped environment; `mac_driver.py --status
  <name>` says which are there, and `--setup <name>` raises Accessibility's
  dialog there and writes the machine down.
  The machines are `FNL_DRIVE_MAC`'s or `~/.local/state/fenestal/mac-hosts`
  lines; input within two minutes that was not the agent's
  (`FNL_DRIVE_MAC_BUSY`) is a hand and a skipped environment, and
  `FNL_DRIVE_MAC_OUTPUT` names the monitor. `pace.py` has a column for
  each renderer there (`metal@schenker-mac`, `vulkan@schenker-mac`,
  `gl@schenker-mac`).
- **And on X11 with no window manager, on a Linux machine of its own.**
  `x11-vulkan-bare` is the `linux` binary, under `FNL_BACKEND=x11`,
  copied over ssh to another
  machine's Xorg, with no window manager or desktop there, which is where
  `x11.c`'s unmanaged paths run, and no environment on this desktop
  reaches them (`tools/drive/bare_driver.py`).
  Xorg is started there as root (`:9`, VT 7, unit `fenestal-xorg`) and left
  up; its socket is forwarded here, so `x11_driver.py` asks it questions
  and XTEST drives it. That is the one XTEST in the tree, and it is allowed
  because it goes to that server and never to `$DISPLAY`. The machines are
  `FNL_DRIVE_BARE`'s or `~/.local/state/fenestal/bare-hosts` lines (`name
  user@address`), and a name the variable gives alone takes its address
  from that file; `--setup`, `--status`, `--up` and `--down` take a name.
  Kernel drivers after a machine's address there (`schenker
  user@schenker nouveau nvidia`), or `FNL_DRIVE_BARE_DRIVER`, are a KMS
  round each, `kms-vulkan-bare@schenker:nouveau`: the servers stopped, the
  NVIDIA GPU moved to that driver with no reboot by `tools/gpu-driver.sh`
  over root ssh (`--driver <name> <driver>` by hand), drawn on, and moved
  to the other driver and back from the titlebar's GPU box by the round's
  hands (`FNL_GPU_DRIVER=switch` sent). KMS's
  alone, as the library's own move (`FNL_GPU_DRIVER`) is; under Xorg,
  weston or cage the boot entry chooses the driver (DESIGN *Another kernel
  driver, with the window open*). The keys' nouveau entry puts NVIDIA's
  Vulkan, layer and EGL files aside at boot; a round on a driver puts them
  back for itself (`nvidia_userspace()`), and every other round first has
  the GPU and the files as the boot does (`Bare.as_booted()`).
  A bare round drives it as it drives the guests and the laptops, every
  variant, the checkers' runtimes shipped with the program; one with a
  console login there active in the last two minutes
  (`FNL_DRIVE_BARE_BUSY`) is a skipped environment. `pace.py` has a column
  for it (`bare@thinkpad`): lavapipe draws there, so the figures are the
  pacing's and the CPU's, not a GPU's; `MESA_*`, `LP_*` and `FNL_GPU`
  reach the program there as `VK_*` does -- the round says which device
  it is on and stops if it is not the one named, and waits as for a
  checker tree on a discrete GPU left to runtime-suspend -- and
  `MESA_VK_WSI_DEBUG=noshm` is the mode to run it in
  (DESIGN *X11*: Mesa's MIT-SHM path leaks; Xorg there is the Intel
  driver with TearFree, which `noshm` needs); `tools/probe/bare_run.py`
  runs one program there and prints what it says, on KMS with
  `--kms` or on weston with `--weston` (`tools/probe/heldpresent.sh`
  builds that probe and runs it so), the GPU moved to a kernel driver
  first with `--driver`. Its
  outputs are laid out by the harness, the panel first and the rest to
  its right (`arrange()`), and `FNL_DRIVE_BARE_OUTPUT=HDMI1` runs a
  round on one. This desktop booted into `fenestal-console` is one more,
  `FNL_DRIVE_BARE="black user@black" FNL_DRIVE_BARE_BUSY=0` for a round
  (tools/drive/README.md), and never in `bare-hosts`.
  A machine booted from its stick (an Arch ISO for each host that touches
  nothing installed) is driven the same way: `bare_driver.DISTROS` tells
  Arch from Debian by `/etc/os-release` -- the package manager, Xorg's and
  lavapipe's paths -- and nothing else names either. The images the bare
  machines boot -- the lab hosts' persistent sticks and the Pi's SD card;
  a new machine is a new host there -- and the tooling that powers,
  updates and reflashes them are the boot images' repository's, private
  and no part of this tree; its builds never run under this tree's
  `build/`. Where it and this tree's checkout are is `CLAUDE.local.md`'s.
  The Raspberry Pi 4 (`pi4`, Arch Linux ARM, in `bare-hosts`) is one more:
  `bare_driver.TREES` sends a machine the tree its `uname -m` takes,
  `linux-arm64` there, and an environment with no tree for it -- the
  Windows ones, with no wine on ARM -- is skipped with the reason. With no
  monitor, its HDMI-A-1 is forced on by the kernel's line.
- **And on the same machine with weston, or with nothing.**
  `wayland-vulkan-bare` is the same binary, as `wayland`, on that machine's
  weston, started by root as a login session of the account's on the same
  VT (unit `fenestal-weston`, socket `wayland-fenestal`, no panel, no
  lock, unaccelerated pointer, a 10 ms repaint window -- at weston's 7 the
  Ironlake GPU there misses every vblank): a compositor that offers less
  than GNOME, so the Wayland backend's fallbacks for a missing global run
  there, and the library says at open which globals it found unbound.
  Every output is on, where weston's scene graph says it put each
  (`weston_layout()`), so the round's move between monitors runs there.
  `kms-vulkan-bare` is the same binary, as `kms`, with neither server up, so
  it opens the DRM node first and has the panel; lavapipe has no display,
  so there it runs the copy path (a headless surface, read back and
  page-flipped), and its picture is the scanned-out framebuffer
  (`kms_grab.py`, as root). Only one of Xorg and
  weston runs at a time; asking for either stops the other, and the KMS
  round stops both. Their input is uinput (`uinput_hands.py`, run there as
  root), devices the kernel offers like any others -- the Wayland and bare
  display's XTEST, allowed for the same reason: it reaches that machine
  and nothing of this one. `--weston <name>` starts weston by hand;
  `--setup` also puts the account in group `input`, loads `uinput` at
  boot, sends the kernel's messages to VT 12 (`setlogcons`; nouveau on
  aspire logs a fault each time the GPU wakes), quiets nouveau's bus
  subdev on a machine with an NVIDIA GPU (`debug=bus=fatal`, by
  modprobe.d: those faults are a VBIOS script's reads, DESIGN *Drawn
  across the move*), and fetches the debug info of every installed
  Vulkan driver and layer and of Mesa's OpenGL by build id (DESIGN: a
  checker then names the driver's frames, which `tsan.supp` keys on).
  The KMS window cannot be resized, so the round's size sections
  declare `resizable` and are skipped there. `pace.py` has a column for
  each, `weston@thinkpad` and `kms@thinkpad`, beside `bare@thinkpad`.
- **And the Windows tree there, under wine.** `win64-vulkan-bare`,
  `win64-vulkan-wl-bare` and `win64-vulkan-cage-bare` (and the `d3d12`
  twin of each: six) run the program under wine on that
  machine's Xorg, weston, and cage -- a kiosk compositor on the DRM node,
  since wine has no KMS driver (unit `fenestal-cage`; `--cage <name>`).
  The prefix is in `/tmp`, made per boot with `Managed=N`
  (`WINE_PREFIX_UP`); the cage round's hands are an absolute pointer,
  since cage's is accelerated. What is wine's and not the library's is
  declared and skipped with the reason (`COMPOSED_ELSEWHERE`,
  `MAXIMIZED_LOST_IN_FULLSCREEN`); DESIGN *Wine on the bare machine*.
- **Never use XTEST here — not in the harness, not in a throwaway script.**
  XWayland runs with `-enable-ei-portal`, so each process's first
  `xtest.fake_input` raises a RemoteDesktop consent dialog the user has to
  click, and until they do no X client gets pointer input. Every environment
  on this desktop is driven through the portal session instead, which holds
  a saved token:
  `tools/drive/environment.py` (`b.tap`, `b.click`, `b.drag`, `b.key_down`) or
  `wayland_driver.Portal` directly. `import -window` can return a stale frame;
  believe the event echo over a capture.
- **The harness types into the desktop, so it asks first.** A portal key goes
  to whatever the compositor has focused. `environment.py` checks before
  every key (`keyboard_is_ours()`: the X server's focus and `_NET_ACTIVE_WINDOW`
  together on X11 and wine, the echoed focus events on Wayland) and raises
  `InputRefused` if it cannot show the key is going to the window under test.
  Do not send keys through `b.portal` directly; that bypasses it. The rounds
  also overwrite the clipboard, and switch GNOME's keyboard layout to
  German with dead keys for a few keys and back (`desk_dead_layout()`),
  restored at exit as well.
- **A round holds the machine's own input** (`tools/drive/input_hold.py`):
  its keyboards, mice and touchpads, from the program's start to the
  round's end however it ends -- grabbed as root by evdev on this desktop
  (by the key `root-key` in the state directory names) and the bare
  machines, which the portal's and
  uinput's input never comes through, and by the agents' hooks and event
  tap on the Windows laptops and the Mac, which let the agent's own events
  through. Ctrl+Alt+Escape (Control+Option+Escape on a Mac) gives it back
  and stops the sweep after the round; the grab ends with the harness, and
  after 30 minutes regardless. `FNL_DRIVE_HOLD=0` holds nothing.
- **The harness waits on evidence, and only asks for the focus.** `b.tap()`
  and `b.key_down()` wait for the key in the echo and resend once after
  silence; pass `confirm=False` only for a key the application is never
  shown. `x11_driver.focus()` sends `_NET_ACTIVE_WINDOW` and nothing else: a
  `SetInputFocus` beside it makes the X server say "focused" about a window
  the compositor sends no key to, which is what the lost keys were.
- **The event echo is the library's** (`src/core/echo.c`, inside
  `fnl_poll_event()`): `FnlInitDesc::echo_events`, or `FNL_ECHO_EVENTS=1` in
  the environment for a program with no flag. The harness sets the variable
  for whatever it starts, so a program with nothing in it for the harness
  can still be driven. A new `FnlEventType` or `FnlKey` wants a line there.
- The native Wayland window **cannot** be captured: GNOME refuses the screenshot
  API to non-sandboxed callers. Driving it goes through the RemoteDesktop and
  Screenshot portals — see tools/drive/README.md *Driving a window*. That needs
  a one-time consent: `python3 tools/drive/wayland_driver.py --consent`, and
  `--status` to see whether it is held. The token lives in
  `~/.local/state/fenestal/` and survives `rm -rf build`; without it every
  desktop environment of `stress.py` is skipped with the reason rather than
  opening a dialog in the middle of a timed round. Composited captures are a
  *second* portal, and one the client cannot persist — whether it asks is the
  desktop's stored permission. On this machine it is already granted and
  `--capture` costs no dialog, only time (measured: a 51 s Wayland round
  against 37 s), so turn it on when the question is what the compositor did
  with the frame. It is off by default for the machine where it would ask.
- There is no test target and no `install()`. Correctness is the checker
  variants, the in-build `verifyfont` and `verifykeymap` steps, the fuzz
  harnesses in `tools/fuzz/` (replayed over their corpus on every native
  build, and on the Pi for the ARM tree by `tools/probe/bare_checks.py`;
  the clang trees also build them against libFuzzer), and
  `tools/drive/stress.py` — one
  round over every environment,
  where a check declares what it *needs*
  (`server-rect`, `wm-props`, `grab-ring`, `unminimize`, `composited`) and
  never tests an environment by name. Add checks there, not to a
  per-environment file.
  Those need a desktop, which is part of why there is no test target.
  A `tsan` round runs under `tsan.supp`, which the harness names for it,
  and every checker round (`asan`, `tsan`, `gc`) under the validation layer,
  which a check proves is loaded (`--validation off` declines). A round's
  first two checks are that a Linux program opened the backend its
  environment named (`backend_check()`) and that every program drew with
  the renderer it was named (`renderer_check()`).
  `tools/drive/options_wayland.sh` is beside the round and not in it: the
  Wayland build's options under the checkers, with no portal.
  `/checkers` runs the checker builds with the known false positives accounted
  for; `/capture` takes a frame and reads pixel values off it correctly.
- **What was run where is `tools/drive/status.py`.** Every round and sweep
  appends a line to `~/.local/state/fenestal/ledger.jsonl`
  (`tools/drive/ledger.py`): commit, environment, variant, layer, GPU,
  result. `status.py` prints the latest of each with how many commits old it
  is, and `never` where an environment is owed one. Read it before quoting
  a figure as current; a new instrument records through `ledger.record()`.
  `FNL_DRIVE_LEDGER` names another file, and set to nothing writes none.
- **Performance is `tools/drive/pace.py`.** It runs `fenestal-dev pace` — a
  caller that asks for its own sizes from a thread of its own, built into
  every tree —
  in the four environments of this desktop at a sweep of rates and
  tabulates frames presented and CPU. One instrument across them is the
  point: a pacing figure in one environment alone reads as "this client is
  fast". Takes a target,
  `--variant` and `--rates`; the full default sweep is about six minutes. A
  laptop the harness knows is a column more, started and sampled by the agent
  there. Its last row, `border`, is the one the harness drives: a corner of
  `fenestal-testbed`'s window held and carried in and out, since the harness
  finds a window by its echo and `fenestal-dev` drains no events. The latest
  full set, with the stress round's, is `docs/benchmarks.md`.
- **A frame with an application in it is `tools/drive/bench.py`**: the
  testbed's benchmark mode (`--bench`, DESIGN *Benchmarking*) run in an
  environment as a round starts a program, its `fenestal-bench/1` records
  brought home on stderr, checked by `tools/bench/check.py` against
  `tools/bench/schema.json`, kept in the state directory's `bench/` and
  written to the ledger; `tools/bench/report.py` makes the tables,
  `--compare`s two and with `--html` draws them, and `--fit` reads a
  `--sizes` sweep into a frame's fixed cost and its cost a megapixel.
  Every environment: this
  desk's, the bare machines', the guests', the laptops' and the Mac's;
  `--hands` moves the pointer through each measure, for the latency rows,
  everywhere but this desk. Arguments after `--` go to the program
  (`--bench-<key>=<value>`).

## House style

`.clang-format` encodes what follows, but the tree predates it and was not
formatted with it — treat it as a reference and a check on new code, not as
something to run over the tree. `.claude/hooks/style-check.sh` checks the
mechanical part on uncommitted lines only: C, shader and CMake sources, and the
scripts under `tools/` with `build.sh` and `run_all.sh`. The Python under
`tools/` is held to `ruff check tools` (`ruff.toml` says what is left out and
why), clean, and to `mypy`, whose five findings in `environment.py` -- class
attributes defaulting to an empty `()`, `[]`, `{}` or `frozenset()` that
subclasses override -- are the baseline (beside the missing stubs for PIL, Xlib,
dbus, gi, numpy and vboxapi); ruff and mypy are user-installed (`pipx`).
`clang-format` and `clang-tidy` are installed (21.1.8) and each has been run
once, check-only, and neither run is clean: `clang-format` would rewrite some
3,900 lines of deliberate alignment, and `clang-tidy` prints 66 findings over
the three trees, each read at its site and none a defect. Each file's header
says what its run said, and for `clang-tidy` that list is the baseline: a
finding not on it is new.

- **Tabs indent C, Objective-C, GLSL and the linker script**, a tab four
  columns wide (`.editorconfig`), one tab a level; continuation lines take
  tabs for every four columns and spaces for the rest, so a line looks as it
  did at any width of four. CMake, shell, PowerShell, Python and YAML indent
  with four spaces and carry no tab, as bovnar's do and Python's must.
  Wrap at 80 columns, a tab counted as four, in prose as well as in code.
- Allman braces for function *definitions*; K&R everywhere inside. Single
  statement bodies commonly go unbraced.
- `/* ... */` comments only — no `//` in C sources. **Comments explain why, not
  what**: a non-obvious decision carries the constraint that forced it and the
  alternative that was rejected, in as few lines as that takes. Do not narrate
  history — no "this used to", no "change N", no bug post-mortems. If a fact is
  load-bearing for a future edit, state it as a rule.
- `snake_case` functions and variables, `UPPER_SNAKE` macros and file-scope
  const tables, `PascalCase` typedef'd structs and enums. Functions are prefixed
  by subsystem: `deco_`, `surface_`, `text_`, `overlay_`, `vk_`,
  `create_`/`destroy_`.
- **`#include "core/app.h"` first in every `.c`**, ahead of any libc header — it
  pins `_POSIX_C_SOURCE 200809L`. Per-TU includes follow.
- Two-tier error handling: a recorded `FnlResult` for the unrecoverable (no
  window, no GPU, no memory), and `bool`/NULL plus one line through the log sink
  for anything degradable. Nothing calls `exit()`.
- Range-check untrusted input as division on a known-good size
  (`count <= (size - off) / stride`), never a multiplication that can wrap.
- British spelling in prose (`--licence`, with `--license` accepted).
- **Every source file opens with its licence**, as `LICENSE` sets out:
  `SPDX-License-Identifier: MIT` and `Copyright (c) 2026 Janos Sonntag`
  inside the opening comment (a comment of its own above a `#version` or a
  first line of text), after a shebang, and in `demos/` and `examples/`
  `CC0-1.0` with `Written in 2026 by Janos Sonntag and dedicated to the
  public domain.` in its place. Documentation (`docs/`, the Markdown) is CC
  BY 4.0 by directory and carries no tag. A file holding material this
  project did not write adds an `SPDX-FileComment` naming whose it is
  (`keysym_table.h`, from `mkkeysyms.py`), and the material gets a section
  in `THIRD_PARTY_NOTICES.md`. `mkdist.py` puts the licence lines back
  into every file it strips.
- The tree is **warning-clean** under the shared set in `CMakeLists.txt`
  (`-Wall -Wextra -Wshadow -Wstrict-prototypes -Wdouble-promotion -Wformat=2`
  and more), generated protocol stubs included, **under gcc and under clang
  alike** -- the `clang` trees build with `-Werror`. Keep it that way.
- A recovery or fallback path is forced by a named fault site,
  `FNL_FAULT("name")`, listed in `src/core/fault.c` and switched on by
  `FNL_FAULT` in a Debug tree's environment (DESIGN *Forcing the paths that
  never run*). Under `NDEBUG` it is the constant false. A new fallback wants
  a site and a row there, not a hand edit.
- `FNL_ASSERT` is for an invariant no input can break. In a Debug tree a
  failure logs one line and calls `abort()`, the one exception to "nothing
  calls `exit()`"; under `NDEBUG` it is nothing. What a caller or a peer can
  get wrong goes through the two error tiers in every build instead.
- Lock discipline is checked by clang's `-Wthread-safety`: `FnlMutex` is a
  capability, so a lock not released on some path fails the `clang` trees,
  and a function that expects a lock held says so with `FNL_REQUIRES`
  (`core/compiler.h`; no-ops under gcc). Never lock and unlock under two
  separate tests of one flag. Fields are not annotated: clang 21 does not
  bind a struct member's `guarded_by` to its instance in C.

## Invariants

- **A consumer links `fenestal` plus the system's own libraries, and
  nothing else** — `m`+`pthread` (and `CMAKE_DL_LIBS`, nothing from glibc
  2.34) on Linux, `user32`+`gdi32`+`d3d12`+`dxgi`+`dxguid` (a static library
  of interface ids) on Windows, AppKit (`-framework
  Cocoa`)+QuartzCore+Metal on macOS, and OpenGL+IOSurface where the tree
  has OpenGL. Every runtime that is not the system's
  -- the Vulkan loader, `libEGL`, `libwayland-client`, `libwayland-egl`,
  `libxcb` -- is opened at run time through `src/core/dynlib.h`, its entry
  points reached through pointers that a `#define` puts in each name's
  place (`vk_fns.h`, `egl_dyn.h`, `wl_dyn.h`, `xcb_dyn.h`), so a machine
  without one still loads the program and steps past what wanted it. **A
  new call into one of them wants a row in that list**; missed, it fails
  the shared object's link (`--no-undefined`) and `fenestal-dev`'s. A
  caller's own Vulkan scene needs no loader either: it asks
  `FnlVulkanGpu::get_instance_proc_addr` for its entries, as the examples'
  do under `VK_NO_PROTOTYPES` (a caller that links one anyway has, in a
  cross tree, the import library or MoltenVK stub synthesised from the
  sources). KMS adds nothing: its DRM and evdev are ioctls and its
  layout is read from xkeyboard-config's files. The X11 entry is *core* xcb and
  nothing beside it, which is why that backend hand-encodes the extension
  requests it needs and lays out two ICCCM structures itself. The Wayland
  core protocol's interface objects are the library's own, generated from
  the installed `wayland.xml`. FreeType and glslangValidator are tools this
  project *runs* at build time and never links. When something needs a
  library, the escape hatch is a subprocess, not a new entry on the link
  line.
- **The API is 0.9 and not yet stable** (`fenestal.h` *stability*, DESIGN
  *Compatibility*). A minor release may rename, change or remove anything,
  at the source and at the binary; `fnl_init()` refuses a header from
  another minor, and the soname carries it (`libfenestal.so.0.9`). Enum
  values are still appended rather than inserted, and descriptors still grow
  at their end, because that is what 1.0 will promise. The build compares
  the ABI and the export list against `tools/abi/` and fails on a
  difference: a change, intended, updates the baseline in the same commit.
  The version lives in the header and in `project()`, and configure refuses
  a mismatch. `CHANGELOG.md` gets a line for anything a caller would notice.
  MSVC is not supported.
- **The API reference is generated** (`tools/mkapidoc.py`, run by `build.sh`
  into `build/api/index.html`) from the headers, with `examples/*.c` as its
  examples. A new `FNL_API` function wants a comment directly above it, a thread
  tag, and a caller in a program the build makes -- an example, the testbed, a
  demo, `fenestal-dev`'s two modes or `tools/headercheck.c` (`CALLERS` there) --
  or `build.sh` fails; a snippet names what it shows in its first comment
  (`Shows: ...`).
- **`fenestal::static` exports the API and nothing else.** The archive is one
  partially linked object with every non-`FNL_API` symbol made local; the
  list is read from the headers at configure. A program linking it must be
  free to name its own `app_event`, which `fenestal-dev` does. On
  Windows the objects are `-mcmodel=small`, or mingw's `.refptr` slots break
  the localisation (DESIGN *Building*).
- `src/core/app.h` holds all shared state, constants and core's cross-TU
  prototypes, plus `_Static_assert`s locking the instance structs against the
  shaders; the Vulkan renderer's own prototypes and the push-constant structs
  with their asserts are in `src/renderer/vulkan/vk_internal.h`. **Changing a
  push-constant or instance struct means updating the shader, the C struct and
  the assert together** — see `/shader-sync`. `FnlDrawState` is pinned the same
  way for a different reason: a field added there and forgotten in
  `fnl_draw_state_eq()` is a frame that never re-records when that field
  changes. (It is prefixed because `<windows.h>` declares `DrawState`.)
- Threading: main thread owns the window server and input, render thread owns
  everything the graphics API touches. They share only what `App::lock` guards
  plus a few atomics. The Wayland epoll set must stay **level-triggered** —
  `wl_display_read_events()` does one read, not a drain.
- **The renderer can change under a live window**: a GPU of another
  renderer in the list is a swap on the render thread (`renderer_swap()`),
  which writes `App::renderer` and frees its state. Off the render thread
  read `renderer_any()`, never `RENDERER(a)`, and nothing it calls may
  reach `App::renderer_state`. A renderer's `list_gpus` must list as its
  pick does, in the same order, on a thread that is not the render thread
  and with no state of the renderer drawing.
  DESIGN *Moving to another renderer*.
- Per-image instance buffers are **device-scoped, not swapchain-scoped**, so a
  rebuild retires command buffers that still name them. `images_in_flight` is
  seeded with the previous frame slot's fence for exactly that reason — see
  DESIGN *Per-image instance buffers*.
- Every filled overlay rectangle is a **background**. Rectangles, then strokes,
  then text, each one draw for the whole frame; anything that must sit over
  something else flushes first. See DESIGN *Recording*.
- Colour: `testbed/shaders/scene.frag` linearises generated colour
  unconditionally, composites in linear, and encodes on output only when
  `pc.srgb == 0`. A sampler on an `_SRGB` image returns linear regardless of
  surface format.
- Client-side decorations are assumed, not negotiated. Known, logged limitation.
- **On Win32, and on X11 with no window manager, a window opens no larger
  than its monitor's work area** (`win32_open()`, x11.c's `fit_opening()`):
  nothing there declines a size, so the library gives the answer a window
  manager would have. A round that finds no room around its window takes
  it in by the border first (`ROOM` in `stress.py`).
- **On Win32 a size is held until its frame is shown** (`size_await()`),
  and the retired swapchain is destroyed on a thread of its own where the
  driver is slow at it (*the reaper*, `swapchain.c`) -- never beside
  `vkCreateSwapchainKHR`, which Intel's driver does not survive. A wait on
  the window's thread dispatches sent messages and takes nothing off the
  queue. DESIGN *Measured on Windows itself* has the measurements and what
  was tried and left out.
- **Input is a queue, not a callback.** `fnl_poll_event()`, drained from
  `on_overlay` — and from `on_record` first when input moves the scene, since
  that callback runs earlier in the frame; there is no `on_event`. The same
  stream carries the library's own notifications (resize, scale, window state,
  antialias, scene size, capture, output, GPU), which are **diffed once a
  frame** in `events_post_state()` rather than posted per site — add a new one
  there, not at the site that causes it. Motion coalesces; nothing else does.
  `FnlKey`/`FnlButton`/`FnlEventType`/`FnlAntialias`/`FnlCursor` values are
  pinned: append, never insert, never renumber.
- **Text is a mode, and the layout is ours on three backends of five.**
  `fnl_set_text_input()` turns on `FNL_EVENT_TEXT`, key repeats and a live
  Escape together; all three are read per key from one atomic, and the pump
  is told only for what its backend holds (`text_input_changed`: the input
  method, a composition, a held accent). Win32 delivers characters, and so
  does Cocoa, through AppKit's input method (`NSTextInputClient`). X11 and
  Wayland deliver keysyms and KMS key codes alone, and `src/backend/keymap.c`
  applies the layout for all three without libxkbcommon or libX11, dead keys
  included (`keymap_type()`, from a generated table; no Compose key).
  Wayland's keymap arrives as text, which `src/backend/keymap_xkb.c` parses;
  only that file carries keysym names. KMS's is built by `keymap_rules.c` from
  the console's names through xkeyboard-config's rules and symbols files,
  whose includes the same parser follows; `FNL_KMS_LAYOUT=de(nodeadkeys)`
  (the testbed's `--layout=`) names a layout in the console's place there
  and nowhere else -- under a window server the session's layout applies
  and core says so in one line. KMS keeps the layout group itself and
  switches it on a group key (`keymap_group_key()`), the one place a key's
  XKB type is read beyond its level count: which modifier selects a group
  key's second level, so Alt+Shift switches and Shift alone does not.
  On Wayland the compositor's input method comes first, through
  `zwp_text_input_v3`, and the two never overlap: a key it consumes is never
  delivered as a key. `FNL_NO_INPUT_METHOD=1` leaves it unbound, and a round
  started with that types through the library's composer instead. A repeat
  goes through `app_key_repeat()`, never `app_key()`: no binding may act on
  one.
- **X11 hears about a keymap change only through XKB's own events.** Having
  called `XkbUseExtension`, the client is no longer sent core `MappingNotify`;
  `xkb_ask()` selects `NewKeyboardNotify` and `MapNotify` instead, and
  `MapNotify` is selected by `affectMap`/`map`, not by `selectAll`. Lose
  either and the layout read at startup is the layout for the window's life.
  The events only set `keymap_stale`; the next key that types reloads.
- **Strings too long for an event are fetched, not carried.** The composition
  and the clipboard arrive as `FNL_EVENT_COMPOSE` / `FNL_EVENT_CLIPBOARD` with a
  length, and `fnl_compose_text()` / `fnl_clipboard_text()` copy them out under
  `App::text_lock`. A clipboard request is *always* answered with an event,
  empty when there is no text — a backend path that returns without calling
  `app_clipboard_deliver()` is a paste that hangs. The owner is another program,
  so the Wayland and X11 backends keep a deadline (`ClipIn::deadline` in
  `src/backend/clip_linux.h`, renewed per piece) and report a transfer cut short
  as *no text*, never as the part that arrived. Nothing on the pump thread may
  block on another program's descriptor: Wayland's `clip_write()` is
  non-blocking with a bounded wait.
- **Layout rules are pinned by `tools/verifykeymap.c`**, run in every native
  build. A change to `keymap.c` or to what `tools/mkkeysyms.py` emits wants a
  case there; a keysym *name* missing from the table reads as an empty level
  on Wayland, which is how the keypad once typed nothing.
- **`-fanalyzer` loses a heap pointer stored through a `void *` callback
  argument, and one stored inside a loop.** Both read as leaks at the store.
  The Wayland listeners therefore keep their strings in fixed buffers, and the
  paste reads one chunk per pump turn. Restructure; do not suppress.
- **A key is a row in four places**: `FnlKey` in `fenestal.h`,
  `src/backend/evdev_keys.h` (Wayland, X11 and KMS share it; X11 is the same
  code plus 8), `SC_PLAIN` / `SC_EXTENDED` in `win32.c` and `MAC_KEYS` in
  `cocoa.m` -- and a name in
  `KEY_NAMES` in
  `src/core/echo.c`, or the harness sees "unknown". The held set is sized by
  `KEY_HELD_WORDS`, and an assert says when it is outgrown.
- **The pointer's shape has one owner per zone.** `deco_cursor()` is the
  precedence for every backend: capture, then a resize edge, then the caller's
  shape where `deco_cursor_is_callers()` says `fnl_set_cursor()` applies. Each
  backend only maps the answer through its own table (on Win32 Windows draws
  the edge arrows itself, so `cursor_apply()` reads the shape alone). A new
  `FnlCursor` is a row in each backend's table, and X11's core cursor font is
  what limits the list.
- **What the backends do alike is written once, in core.** The hit test
  (`deco_hit()`), the left-press dispatch (`deco_press()`, `deco_drag()`),
  hover, pointer input (`app_pointer_motion/button/scroll()`), the
  size/scale/focus/output publish (`app_publish_*`, with the configure serial
  under the same lock), a close request (`app_close_requested()`), which keys
  repeat (`app_key_repeats()`), the size request's floor, the captured
  pointer's reticle (`app_reticle_*`) and the entries a backend has nothing
  to do for (`backend_noop*`) live in `src/core/decor.c` and `app.c`. A
  backend that grows its own copy of one has started the drift those replaced.
- **The titlebar's controls are the library's** (`src/core/controls.c`): three
  checkboxes and the output, GPU and size drop-downs, acting through the same
  public knobs a caller would use, so the event that reports a change comes
  from the frame's diff either way. The size list is *derived* from the output
  and the floor, never a shipped table. While a list is open, and the
  library's bar it drops from is shown, `ctl_modal()` makes every press the
  controls', which is what lets the dismissing click be consumed.
- **The titlebar is replaceable, and the split is picture/behaviour.**
  `fnl_titlebar_custom()` hands the drawing to the caller; the library keeps the
  hit test, the drag, the double-click and the resize edges, driven off
  `fnl_titlebar_claim()` rectangles that are **rebuilt every frame** from
  `on_overlay`. Claims registered once go stale the first time the window
  resizes. `deco_client()` asks whether there is a bar at all, `deco_bar_h()`
  how tall, and `App::deco_custom` whether *we* paint it — reaching for
  `DECO_TITLEBAR_H` at a geometry site is right for the library's bar and
  silently wrong for every caller's.
- Layout figures derived from a measurement want **re-measuring whenever
  anything joins the row they are measured against**.
- **Grep for the *writers* of a shared field, not its readers.** A gate written
  in one backend's vocabulary reads as a permanent zero in the others, and
  nothing fails, warns or degrades. `FnlBackendVtbl::invites` and
  `window_on_screen` are that lesson made into seam entries.
- `tools/` targets carry `-fno-sanitize=all` because they run *during* the
  build. Do not remove it. The exceptions are `verifykeymap` and the fuzz
  harnesses (`SANITIZED` in their `fnl_host_tool()` lines): what they look
  for -- an over-read in damaged input -- is only found when a checker is
  watching. Running during the build is also why a cross build cannot
  bake its own font; the
  cross target borrows the host tree's `generated/`, which `build.sh`
  wires up and `FNL_GENERATED_DIR` names. `FNL_FONT_DIR` is the same loan for
  the font alone, between trees of one native target.

## Source layout

`src/` is layered, and each directory means what it says — checked, not
asserted: no `.c` in `src/core/` names a window server, the kernel's DRM and
evdev, or a graphics API at all, and no file is excused: a backend's state and a
renderer's are each behind an opaque pointer of `App`'s -- `App::backend_state`
for the backend (`<p>_state.h`), `App::renderer_state` for the renderer
(`src/renderer/vulkan/vk_state.h`, reached as `VK(a)->field`).

```
src/core/              the lifetime, the two error tiers, the frame policy,
                       the client titlebar's layout and behaviour, the text
                       measure, and the two seams (backend.h, renderer.h) plus
                       the compiler and threading shims
src/backend/           what the Linux backends share and none owns: the
                       layout (keymap*.c), evdev's key table, the event
                       loop's plumbing (loop_linux.h) and a paste's
                       (clip_linux.h)
src/backend/wayland/   \
src/backend/x11/        > one per window server; each names no graphics API
src/backend/win32/     /
src/backend/cocoa/    /  (Objective-C: AppKit has no C interface)
src/backend/kms/       no window server: the DRM node and evdev, and no
                       graphics API either
src/renderer/vulkan/   \
src/renderer/gl/        > one per graphics API; it names no window server,
src/renderer/d3d12/    /  except the bridges (surface_*.c, GL's egl_*.c),
src/renderer/metal/   /   the one place that must see both halves (Metal's
                          are .m)
```

`tools/check-layering.py` turns that into a check rather than a claim --
`build.sh` runs it after every build that succeeds -- and it counts two
ambiguous prefixes in columns of their own rather than resolving them: `VK_`
is Vulkan's constants and Win32's virtual keys, and `WM_` is Win32's messages
and X11's ICCCM atoms.

Includes are paths from `src/`: `#include "core/app.h"`.

## Generated files — never edit

One generated file is tracked: `src/backend/keysym_table.h`, made by
`tools/mkkeysyms.py` from X11's `keysymdef.h` and run by hand. Regenerate it;
`keysym_notice.txt` beside it is the notice it travels under.

Everything in `build/<target>/<variant>/generated/` is generated and untracked:
`*-client-protocol.h` and `*-protocol.c` from `wayland-scanner`, `<shader>.h`
SPIR-V arrays from `glslangValidator`, their translations from `spirv-cross` in
`gl/` (GLSL 4.50, and its `.h` as a C string), `gl41/` (GLSL 4.10, for macOS),
`hlsl/` and `msl/` (DESIGN *One shader source*), and `font_data.h` /
`font_metrics.h` / `font_atlas.S` / `font_atlas.bin` / `font_cells.bin` from
`tools/mkfont` (a tree that borrows its bake has `font_borrowed.S` and no blobs
of its own). The testbed's shader headers are generated beside it, in
`testbed/`, and the cross tree's `vulkan-1.def`, `libvulkan-1.a` and
`generated/fenestal.def` are written at configure. Edit the sources:
`protocols/*.xml`, `shaders/*`, `testbed/shaders/*`, `tools/mkfont.c`.
