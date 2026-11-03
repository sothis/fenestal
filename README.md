# Fenestal

A portable C11 library: a window, a device, a swapchain, a render thread and a
text overlay, wrapped around a scene that is the caller's. Its backends
are Wayland, X11, Win32, Cocoa (macOS) and KMS -- a Linux console with no
window server, where the window is the whole output; its renderers are
Vulkan, OpenGL, Direct3D 12 and Metal; and a platform is one of each, such
as Vulkan on X11. The GPU is the device that draws. A build has every
backend its system has -- on Linux, Wayland, X11 and KMS, the one a
program opens chosen when it starts -- and every renderer its system has,
chosen the same way: one build per operating system, as `libfenestal.a`
and a shared object.

The library owns the frame — pacing, late latching, resize, DPI changes, render
scale, antialiasing, device-loss recovery — and stops at one place: the
`on_record` callback, where the caller records the scene. The overlay and the
client titlebar are composited over whatever was recorded.

```
  backend    window, input, outputs, scale, timing, wake-up   5 backends
  renderer   device, swapchain, targets, loss recovery        4 renderers
  frame      pacing, late latch, render scale, FSAA, MLAA     1 policy
> your scene a command buffer, inside a pass, viewport set    per graphics API
  overlay    text, rectangles, strokes, client decorations    platform-agnostic
```

Five headers ship, in `include/fenestal/`: `fenestal.h`, the portable
surface, which includes no window-system or graphics header; and one for
each renderer -- `fnl_vulkan.h`, `fnl_gl.h`, `fnl_d3d12.h` and
`fnl_metal.h` -- for what a caller records into and builds pipelines
against.

`DESIGN.md` is the spec: how each part works, what was measured, and what was
tried and left out. `CHANGELOG.md` is what each release changed, and `TODO.md`
what is still owed.

## Building

CMake ≥ 3.18, a C11 compiler (gcc or clang; mingw-w64's gcc for Windows), and:

| Dependency | Provides |
|---|---|
| `wayland-client` + `wayland-scanner` | the Wayland connection's headers; the protocol XML is vendored in `protocols/` |
| `xcb` | the X11 connection's headers — core protocol only |
| Linux's own headers (`linux-libc-dev`) | the KMS backend's DRM and evdev ioctls; nothing linked |
| Vulkan loader + headers | `find_package(Vulkan)`: the headers, which reach a consumer through `fenestal::static`; nothing links the loader |
| `glslangValidator` | compiles `shaders/*` to SPIR-V at build time |
| `spirv-cross` | optional: translates the shaders for OpenGL, Direct3D and Metal, which a Vulkan build only checks |
| `dxc` | Direct3D 12 only: Microsoft's shader compiler for Linux, fetched once by `tools/dxc.py`; run at build time |
| `freetype2` | build-time only: bakes the glyph atlases; not linked |
| pthreads | the render thread |
| `clang-21`, `lld-21` and Apple's SDK | macOS only, cross: the SDK copied once from a Mac by `tools/macos_sdk.py` |
| `gcc-aarch64-linux-gnu`, `g++-aarch64-linux-gnu` | ARM Linux only, cross (the Raspberry Pi 4); Ubuntu's, with their own glibc |

```sh
./build.sh linux                  # one target, release: Wayland, X11, KMS
./build.sh linux asan             # one target under a checker
./build.sh win64                  # cross build; needs linux first
./build.sh macos                  # cross build for macOS; needs linux
./build.sh linux-arm64            # cross build for ARM Linux; needs linux
./build.sh                        # every target and variant
./build.sh dist                   # the source zips alone, no build
./build/linux/release/fenestal-testbed
FNL_BACKEND=x11 FNL_RENDERER=gl ./build/linux/release/fenestal-testbed
FNL_RENDERER=vulkan wine build/win64/release/fenestal-testbed.exe
```

Every tree lands under `build/<target>/<variant>`. The targets are one
per operating system: `linux` (Wayland, X11 and KMS, drawn with Vulkan or
OpenGL 4.5), `win64` (Win32 drawn with Direct3D 12 or Vulkan) and
`macos` (Cocoa drawn with Metal, Vulkan on MoltenVK or OpenGL 4.1), with
`linux-arm64` the `linux` tree cross-compiled for the Raspberry Pi 4;
variants are `release`, `clang`, `asan`, `tsan`, `analyzer` and `gc` (the
Windows target takes `release`, `analyzer` and `gc`, the macOS one
`release` and `gc`): eleven trees, and the ARM one's `release`, `asan`,
`tsan` and `gc` where its cross compiler is. A Linux program opens the
backend its session has -- Wayland, else X11, else, with neither, KMS on
the console -- and says which in its log; `FNL_BACKEND=wayland`, `x11` or
`kms` in its environment opens that one or fails. Every program draws
with the first renderer that works there of those it has a scene for --
Direct3D 12, Vulkan on Windows; Vulkan, OpenGL on Linux; Metal, Vulkan,
OpenGL on macOS -- and says which; `FNL_RENDERER=d3d12`, `vulkan`, `gl`
or `metal` names one. Arguments after the target and
variant go to the configure step. Clean is `rm -rf build`.

The cross build borrows the `linux` release tree's baked font,
since the font tool runs during the build and would otherwise be compiled
for Windows. It produces `libfenestal.dll`, `libfenestal.a` and the `.exe`
programs, the library importing only Windows' own DLLs -- `KERNEL32`,
`msvcrt`, `USER32`, `GDI32`, `d3d12` and `dxgi` -- and opening
`vulkan-1.dll` when it tries Vulkan. The testbed imports nothing more:
its Vulkan scene asks the loader the library opened for every entry, so a
machine without one runs it on Direct3D 12.

The macOS tree is cross-compiled the same way, by clang and `ld64.lld`
against Apple's SDK, and borrows the same bake. It produces
`libfenestal.dylib`, `libfenestal.a` and the programs, linking AppKit,
QuartzCore, Metal, OpenGL and IOSurface, with MoltenVK beside the
programs (`tools/moltenvk.py`, pinned by SHA-256), which the library opens
when it draws with Vulkan and hands to the examples' Vulkan scenes. The
harness drives it on a Mac with each renderer (`macos-metal-native`,
`macos-vulkan-native`, `macos-gl-native`); `TODO.md` *macOS* has what is
owed.

Optimised builds target `x86-64-v2` (`-DFNL_NATIVE_ARCH=ON` for the build
machine's own CPU) and are hardened (`-DFNL_HARDEN=OFF` to leave that out).

## Using it from another project

`add_subdirectory()` and one line of linking. There is no `install()` and no
package config.

```cmake
add_subdirectory(third_party/fenestal)             # or FetchContent
target_link_libraries(my_program PRIVATE fenestal::static)
```

`fenestal::static` is the archive and `fenestal::fenestal` the shared object.
Each carries its own link line, the system's libraries and nothing else:
every runtime that is not the system's -- the Vulkan loader, EGL,
`wayland-client`, `wayland-egl`, `xcb` -- is opened at run time, so a
machine missing one still runs the program on what it has. A program whose
own scene calls Vulkan need not link the loader either:
`FnlVulkanGpu::get_instance_proc_addr` is the one the library opened, and
`testbed/scene_vulkan.c` asks it for every entry, with
`VK_NO_PROTOTYPES` defined so that a call it did not ask for fails to
compile. The Vulkan headers come with `fenestal::static`. A Linux build
has Wayland, X11 and KMS, and the program opens the first the session has:
Wayland where `WAYLAND_DISPLAY` is set, X11 where `DISPLAY` is, KMS where
neither is. `FNL_BACKEND=x11` in a program's environment opens that one or
fails. `FNL_RENDERER` (`vulkan`; `gl` on Linux and macOS, `d3d12` on Windows,
`metal` on macOS; or `all`, the default, for every one the system has) is
the one cache variable a consumer normally sets; a build of several
renderers wants `FnlInitDesc::renderers` to say which the program has a
scene for. `FNL_BACKEND` at configure (`wayland`, `x11` or `kms`; `linux`,
the default, for all three) builds one backend alone. A program that
links the loader anyway (`Vulkan::Vulkan`) gets, in a cross build, an
import library synthesised from the sources, and `FNL_VULKAN_SCAN` lists
globs over the consumer's own so that it has the entry points they call.

`demos/` holds the programs to copy, each a folder with its scenes, its
shaders and a `CMakeLists.txt` of its own that takes the library by those
lines -- configured on its own, or added by this tree's build, which builds
every demo in every tree that way. `demos/README.md` lists them.

```sh
cmake -S demos/texture -B build/texture
cmake --build build/texture
```

The smallest program is `examples/hello.c`:

```c
FnlInitDesc desc;
memset(&desc, 0, sizeof desc);
desc.size           = sizeof desc;
desc.header_version = FNL_VERSION;
desc.title          = "hello";
/* No scene of its own, so any renderer the build has will do. */
desc.renderers      = FNL_RENDERERS_ANY;

FnlApp *app;
FnlResult rc = fnl_init(&desc, &app);
if (rc < 0) {
    fprintf(stderr, "%s: %s\n", fnl_result_str(rc), fnl_last_error());
    return 1;
}

FnlCallbacks cb;
memset(&cb, 0, sizeof cb);
cb.size       = sizeof cb;
cb.on_overlay = on_overlay;      /* draws text with fnl_overlay_text() */
rc = fnl_run(app, &cb);
fnl_shutdown(app);
```

What a caller has to know beyond the API reference:

- **Input is a queue.** Drain `fnl_poll_event()` in `on_overlay`, and in
  `on_record` too when input moves the scene — it runs first, so a camera
  turned only in `on_overlay` is a frame late.
- **Animation asks for its next frame.** The recording is cached; call
  `fnl_invalidate()` from inside the frame while something moves.
- **Per-draw data is push constants**, which live in the recording.
- **Uploads go through the library's queue from `on_gpu_created`.** From
  anywhere else the queue is shared and wants a lock of the caller's.
- **Every filled overlay rectangle is a background.** Anything that must sit
  over text calls `fnl_overlay_flush()` first.
- `Escape`, `F2`, `F9` and `F11` are bound by the library, and so is `F5`,
  which shows the library's metrics panel until a program binds it to
  another key or to none (`fnl_set_metrics_key()`).

## The API reference

`build/api/index.html`, written after every successful build from the headers,
is the reference: every declaration with its comment and the thread it may be
called from, and the examples in `examples/` — a window with text, the event
queue, a text field with copy and paste, a titlebar of the caller's own, a panel
of the caller's, the display modes, the frame statistics, the GPU list, the
metrics panel, and a scene recorded through each renderer's seam. Every tree
compiles and links each of them that its renderers allow.

## Compatibility

Fenestal is 0.9 and not yet stable. Until 1.0 a minor release may rename,
change or remove anything `fenestal.h` and the renderers' headers declare,
at the source and at the binary; `CHANGELOG.md` says what moved,
`fnl_init()` refuses a header from another minor release, and the shared
object's soname carries the minor (`libfenestal.so.0.9`). The header's
*stability* block says what 1.0 will promise. The headers compile as C11
and as C++11 through C++20. MSVC is not supported.

## Running the example

`fenestal-testbed` is the demo program. The titlebar is the library's: window
buttons, a resolution drop-down, the `FSAA`, `MLAA` and `Fs Upsample`
checkboxes, and a GPU drop-down that moves the window onto another GPU
while it stays open -- a second card, lavapipe beside the hardware, or a
GPU of another renderer, OpenGL's beside Vulkan's -- drawn dim where there
is only one; and before it, where the window is wide enough, an output
drop-down naming the monitor the window is on, following it as it is
dragged, and moving it to another -- dim on Wayland, whose clients place
no window. Drag it to move, double-click to maximize.

| Input | Action |
|---|---|
| Left click in the scene | captures the pointer (`F9` releases it) |
| `F1` | keybindings and status lines |
| `F5` | the library's metrics panel (`fnl_set_metrics_panel()`), shown from the start here: FPS counter, frame cost, input latency, and this process's CPU (share of one core), RAM, VRAM on the GPU drawing (named) and GPU use (its frames' share of the GPU's time) |
| `F2` | set the test pattern moving |
| `F3` | text entry on and off |
| `F4` / `F6` | copy a fixed string / paste |
| `F11` | fullscreen |
| `Esc` | quit |

| Option | Effect |
|---|---|
| `--scale=<n>` | surface scale 1 to 8 instead of the desktop's |
| `--gpu=<n>` / `--gpu=<name>` | the device by its number in the startup log, or by part of its name (also `FNL_GPU`) |
| `--layout=<spec>` | KMS: type through this layout instead of the console's, `de(nodeadkeys)`, `de,us` for two groups, XKB options after a space (also `FNL_KMS_LAYOUT`; KMS's alone) |
| `--gpu-driver=<d>` | KMS: offer a move of an NVIDIA GPU between nouveau and NVIDIA's driver with the window open, an entry in the GPU box for each renderer, `nouveau`/`nvidia` binding it first (also `FNL_GPU_DRIVER`) |
| `--gpu-rank=<r>` | the GPU box ranked by each entry's measured frame rate, kept in the user's cache, best first, every start opened on the first; measured for each entry and display not measured before, the window showing the time left and taking no input meanwhile. `calibrate`: measure every entry on this display again; `keep`: use it, never measure; `order`: order the box by it and start as without; `off`: use none (also `FNL_GPU_RANK`) |
| `--no-late-latch`, `--late-latch=<ms>` | sample state at the top of the frame, or at a fixed delay |
| `--custom-titlebar` | the demo draws the titlebar instead of the library |
| `--frame-times` | where each frame's time went, a line each half second on stderr; `FNL_COST_PARTS=1` adds what the two cost figures are made of, a diagnostic (`docs/comparable-frame-costs.md`) |
| `--no-vsync` | draw flat out, for measuring (`FNL_VSYNC=0` for any program); vsync is otherwise always on |
| `--transition=<ms>` | how long the first frames after a move to another GPU, renderer or kernel driver dissolve from the last frame before it; the library's own is 250, and 0 is a cut |
| `FNL_VK_DXGI=1` / `0` | Windows: Vulkan's frames go through a DXGI swapchain, copied by Direct3D 11, or never do; by default they do wherever the GPU can share images with Direct3D 11, and the GPU box lists both ways for such a GPU |
| `FNL_VK_VALIDATION=1` | Khronos' validation layer in the library's Vulkan instance alone, and so in a caller's scene drawn on it, not in Zink's or vkd3d-proton's; a loader without the layer makes the instance without it and says so |
| `FNL_D3D12_DXGI11=1` / `0` | Windows: Direct3D 12's frames go through a Direct3D 11 swapchain, copied there, or never do; by default they do where DWM is measured composing the window at rest, with vsync on |
| `--echo-events` | print every event (`FNL_ECHO_EVENTS=1` for any program) |
| `--text-input` | text entry on at startup: with `--echo-events` each character a key types is a line of its own, and Escape no longer closes the window, so close it from the titlebar |
| `--footprint-log` | this process's CPU, RAM and VRAM -- the metrics panel's figures -- on stderr twice a second, for a harness that cannot read the screen |
| `--bench` | the benchmark mode: a panel of options worked with the mouse, a run in a window of its own, and a `fenestal-bench/1` record of it; `--bench-run` starts at once, `--bench-<key>=<value>` sets an option, `--bench-replay=<record>` takes a record's, `--bench-list` names them (`docs/benchmark-mode.md`) |
| `--licence` | the font's licence and notice |
| `-h`, `--help` | usage |

`./run_all.sh` starts the release testbed four times side by side with the
same arguments: on Wayland and X11, and under wine with Direct3D 12 and
Vulkan.

### On a bare display

The Linux programs open KMS from a text console, or over ssh, on a machine
where no X server or compositor holds the GPU -- by themselves where the
session has neither `WAYLAND_DISPLAY` nor `DISPLAY`, or named with
`FNL_BACKEND=kms`: the program opens
`/dev/dri/card*` first and takes an output whole, so its user wants groups
`video` and `input`. The window is the output, opened at its preferred
mode and maximized for its whole life, so its titlebar has fullscreen and
close and no minimize or maximize; `F11` takes the titlebar away and
brings it back. Beside the checkboxes the titlebar lists the output's
display modes, refresh and all, and the card's other connected outputs: a
pick sets the mode, or moves the window to that output at its preferred
mode and turns the one left off -- what `fnl_request_mode()` and
`fnl_request_output()` do for a program; there is no `Fs Upsample` box.
A monitor plugged in joins the list, and the window leaves one unplugged
for another, or waits for one. `FNL_KMS_OUTPUT=HDMI-A-1` picks
the output it opens on by the kernel's name for it, where the default is
the first panel connected, else the first output. The pointer is drawn on
the output's cursor plane, the layout is the console's
(`/etc/default/keyboard` or `/etc/vconsole.conf`, built from
xkeyboard-config's own files, so no tool is run) or the one
`FNL_KMS_LAYOUT=de(nodeadkeys)` names -- `--layout=` in the testbed; under a
window server the variable does nothing and the session's layout applies
-- and the keyboards are grabbed for as long as the program runs, so
nothing typed reaches the console underneath; `Esc` quits. A driver
offering `VK_EXT_acquire_drm_display` presents the display itself; where
none does (lavapipe has no display), each frame is drawn on a headless
surface and copied onto the output, and the log says which.

## Licence

| Component | Licence |
|---|---|
| Source code (C, Objective-C, GLSL, Python, shell, CMake) | [MIT](LICENSE) |
| Documentation (`docs/`, and the Markdown that documents) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) |
| The programs to copy (`demos/`) and the API reference's snippets (`examples/`) | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| The fonts, the Wayland protocols, the keysym table and the rest of [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) Part 1 | their authors' -- see that file |

Copyright © 2026 Janos Sonntag. Every source file says which of the three it
is under in an `SPDX-License-Identifier` line at its top; [`LICENSE`](LICENSE)
holds the rules and the MIT text. Redistributing Fenestal, in source or
binary form, carries `THIRD_PARTY_NOTICES.md` with it.

The baked glyphs, Fenestal Glyphs Sans and Fenestal Glyphs Mono, are renamed
modified versions of Ubuntu Sans and Ubuntu Sans Mono (`fonts/ubuntu/`),
under the Ubuntu Font Licence 1.0. Its licence and notice must travel with
every binary that carries the atlas: they are built into the library, and a
program reaches them through `fnl_font_notice()` and `fnl_font_licence()`.
